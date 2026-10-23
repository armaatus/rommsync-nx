#!/usr/bin/env python3
"""Every thread's stack, against the deepest JSON document it can be handed (M9-18, #216).

`sysmodule/sys-rommsync.json` sizes the main thread's stack (`main_thread_stack_size`)
and `core/include/rommsync/json.hpp` sizes the one recursion in the tree
(`json::kMaxDepth`). Nothing held the two against each other, and at 0x8000 and
64 they did not fit: 64 levels of `ParseValue` -> `ParseObject` were ~31 KiB on
their own, under a `main` that had already spent ~4 KiB getting there. A
Horizon stack overflow is a data abort with no crash report, and the document
can come from the overlay over IPC or off the card at boot.

`sysmodule/source/stack_budget.hpp` now records the measured costs as
ceilings, and this script holds every side of the arithmetic:

  fit       (every push) the npdm's `main_thread_stack_size` is the header's
            `kMainThreadStackBytes`, and for each thread that parses,
            `(kMaxDepth + 1) * kJsonLevelBytes` plus its rest ceiling fits in
            **half** its stack. The other half is the stated margin, and it is
            for what no static walk sees (below). Also: no thread is started
            except through `SizedThread`, and the inner heap is still small
            enough for `SIZE_BOUNDED`'s level counts.
  contract  (every push) the deepest typed response shape in every pinned RomM
            OpenAPI snapshot uses at most half of `kMaxDepth`, so the limit that
            keeps the stack safe is not one a real RomM response reaches.
  walk      (every push) the call-graph arithmetic below, against hand-built
            graphs whose answers are worked out in the comments beside them.
  measured  (every push, in the image) reads the `.ci` files that
            `-fcallgraph-info=su` leaves beside a cross-compile, finds the
            deepest chain from `main` and `__appInit` and from each thread's
            entry, and fails when the measured level or rest outgrows the
            header's ceilings or a chain outgrows half its stack -- which is
            how a frame that grew is caught.

Every other thread too (M9-31, #268). The process starts three more, each
through `SizedThread` -- the worker, the pairing thread and the PSC watcher --
and each is walked from the `SizedThread::Enter<Method, T>` that
`pthread_create` is handed, with the accounting below, and held to half its own
stack (`kThreadStackBytes`, `kWatcherStackBytes` in `sized_thread.hpp`). The two
that parse have a rest ceiling in the budget header, which `fit` holds inside
that half on every push; the watcher parses nothing, and reaching the JSON
reader is a failure. `THREADS` is the table, `fit` fails on a thread started any
other way, and `measured` fails on an `Enter` no row claims.

Where the call graph comes from (#253). The build is LTO, so the compile of
each unit generates no code and leaves no `.ci`: the code is generated at the
link, and its graph is the `<elf>.ltransN.ltrans.ci` files the LTRANS step
writes beside the ELF when the link is handed `-fcallgraph-info=su`
(`switch.mk` puts `$(DEFINES)` on it for this). Those describe the code that
ships -- after cross-unit inlining, which is the point of reading them -- but
the LTO back end names each node by its bare identifier (`ParseValue`,
`__dt_base`, `_M_copy`), with no class, namespace or signature; every rule
below is about qualified names. Each node's title is still its assembler name,
so the LTRANS graph is read with each label's name replaced by the demangled
title -- `aarch64-none-elf-c++filt`, which is why this phase runs in the image.
A per-unit graph (a build without LTO) is read as it was, by its labels; a
directory holding both is refused, since only one of them is the code that
ships.

How a chain is costed. Each function's frame is GCC's own static figure. Every
recursion reachable from a root must be one `kMaxDepth` bounds -- the parser,
and `json::Value`'s destructor, whose tree is no deeper than the document that
built it -- or one the size of what it walks bounds: a red-black tree's
rebalancing and `std::sort`'s introsort, each costed whole at its worst depth
(`SIZE_BOUNDED`). Any other is a failure, because nothing here can bound it. A
bounded recursion costs `(kMaxDepth + 1)` times its heaviest cycle: one level
per accepted nesting, plus the frame that refuses the next. The recursions on a
chain share that budget rather than adding up, because a frame at parse depth
`d` only ever destroys a subtree of depth `kMaxDepth - d`; so a chain costs its
non-recursive frames plus `(kMaxDepth + 1)` times the heaviest level it crosses,
and the parser's level is asserted to be the heaviest one.

What it cannot see, and why the margin is half. An indirect call is an edge
with no target: `fs::FileSystem` and `http::HttpClient` are interfaces, and the
build has a few hundred such sites. One family is resolved, because the main
thread's whole IPC surface hangs off it -- `ipc::ServiceCore` calls an
`ipc::Engine&`, and the sysmodule's only `ipc::Engine` is `SdEngine` -- so every
`SdEngine` method overriding one `ipc::Engine` declares virtual (read out of
`ipc.hpp`, since a demangled name does not say `virtual`) is treated as
callable from every indirect call in `ServiceCore`, or in `ipc::Dispatch`, into
which LTO inlines `ServiceCore`'s handlers. The PSC watcher's family is the
second, for the same reason: that thread is `power::Watcher::Run` calling
through a `power::Sink&` and a `power::Module&`, whose only implementations are
`SdEngine` and `PscModule`. Everything else, and every frame
inside newlib and libnx, counts as zero. The measured figure is a floor on the
truth, not a bound, and half the stack is what is left for the difference.

Re-deriving the numbers by hand is `DEFINES="-fstack-usage -fcallgraph-info=su"
make -C sysmodule` in the pinned image, then `stack-budget.py measured --ci
sysmodule` there; `tests/test_stack_budget.sh measured` does exactly that in a
scratch copy.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

NPDM = "sysmodule/sys-rommsync.json"
BUDGET = "sysmodule/source/stack_budget.hpp"
JSON_HPP = "core/include/rommsync/json.hpp"
IPC_HPP = "core/include/rommsync/ipc.hpp"
CONTRACTS = "server/contract/romm-openapi-*.json"

SIZED_THREAD = "sysmodule/source/sized_thread.hpp"

# The functions every chain starts from. `__appInit` runs on the main thread
# before `main` does, on the same stack, and returns before it starts.
ROOTS = ("main", "__appInit")


class Thread:
    """One stack this process runs on (#268).

    `entry` names the `SizedThread::Enter<Method, T>` instantiation that
    `pthread_create` is handed -- the thread's first frame, and the one root
    its walk starts from. `stack` and `rest` name constants in the headers:
    the stack the thread is given, and the ceiling on what its deepest parsing
    chain spends beside the JSON levels. A thread with no `rest` parses
    nothing, and reaching the JSON reader on it is a failure."""

    def __init__(self, name, entry, stack, rest):
        self.name, self.stack, self.rest = name, stack, rest
        self.parses = rest is not None
        self.entry = re.compile(entry) if entry else None


THREADS = (
    Thread("main", None, "kMainThreadStackBytes", "kMainRestBytes"),
    Thread("worker", r"::SdEngine::RunWorker\b", "kThreadStackBytes", "kWorkerRestBytes"),
    Thread("pairing", r"::SdEngine::DrivePairing\b", "kThreadStackBytes", "kPairingRestBytes"),
    Thread("PSC watcher", r"::PscSubscription::Run\b", "kWatcherStackBytes", None),
)
THREAD = {t.name: t for t in THREADS}
# Both label forms: `Enter<&...RunWorker, ...>(void*)` demangled, and GCC's own
# `Enter(void*) [with auto Method = &...RunWorker; ...]` from a build without LTO.
ENTER = re.compile(r"rommsync::sysmodule::SizedThread::Enter\b")
# What starts a thread without `SizedThread`, and so without a row above.
# `std::thread::id` and its statics start nothing, hence the `(?!::)`.
THREAD_START = re.compile(r"\b(?:pthread_create|threadCreate)\s*\(|\bstd::j?thread\b(?!::)")
THREAD_SOURCES = ("sysmodule/source", "core/src", "core/include")

# A recursion is allowed only when every function in it belongs to the JSON
# reader, whose depth `kMaxDepth` bounds.
BOUNDED = re.compile(r"rommsync::json::")
# The other recursion the standard library brings: `std::map` and `std::set`
# copy and erase a red-black tree by recursing down one side of it. A red-black
# tree of n nodes is at most 2*log2(n+1) tall, and no tree in this process can
# reach 2^16 nodes -- every node is a heap block of at least 64 bytes, and the
# whole inner heap is 0x180000 -- so 32 levels bounds every one of them.
TREE = re.compile(r"std::_Rb_tree<")
TREE_LEVELS = 32
# And `std::sort`'s introsort, which recurses on one side of each partition
# with a depth limit of 2 * floor(log2(n)) and falls back to a heap sort when it
# runs out: at most that many levels plus the one that falls back. No vector in
# this process reaches 2^21 elements -- the inner heap is 0x180000 bytes, under
# 2^21, and an element is at least a byte -- so 2 * 20 + 1 levels bounds them.
# GCC's own label has no template arguments (`__introsort_loop(` and then
# `[with ...]`), a demangled name has them.
SORT = re.compile(r"std::__introsort_loop[<(]")
SORT_LEVELS = 41
# Recursions bounded by the size of what they walk rather than by any document:
# each adds to its chain like a frame of its own, `levels` trips round its
# heaviest cycle, rather than sharing the JSON depth budget.
SIZE_BOUNDED = (("red-black tree", TREE, TREE_LEVELS), ("introsort", SORT, SORT_LEVELS))
# Both level counts rest on the inner heap's size, so `fit` reads it: a heap
# grown past either premise would leave them a number that silently went stale.
MAIN_CPP = "sysmodule/source/main.cpp"
TREE_NODE_BYTES = 64


def inner_heap(repo):
    found = re.search(r"constexpr\s+(?:std::)?size_t\s+kInnerHeapSize\s*=\s*"
                      r"(0x[0-9A-Fa-f]+|\d+)\s*;", read(repo, MAIN_CPP))
    return int(found.group(1), 0) if found else None


def size_bound_problems(heap):
    """Where `heap` breaks the premise behind `TREE_LEVELS` or `SORT_LEVELS`."""
    problems = []
    # A tree of n nodes is at most 2 * log2(n + 1) tall, and n < heap / 64.
    if heap // TREE_NODE_BYTES >= 1 << (TREE_LEVELS // 2):
        problems.append("an inner heap of %s holds %d red-black tree nodes, and TREE_LEVELS %d "
                        "bounds a tree of fewer than %d" % (hex(heap), heap // TREE_NODE_BYTES,
                                                            TREE_LEVELS, 1 << (TREE_LEVELS // 2)))
    # Introsort recurses 2 * floor(log2(n)) + 1 deep, and n <= heap.
    if heap.bit_length() - 1 > (SORT_LEVELS - 1) // 2:
        problems.append("an inner heap of %s holds a vector of %s one-byte elements, and "
                        "SORT_LEVELS %d bounds an introsort of fewer than %s"
                        % (hex(heap), hex(heap), SORT_LEVELS, hex(1 << ((SORT_LEVELS + 1) // 2))))
    return problems
# The JSON reader's recursions are three and four functions. One much larger is
# not one of them, whatever it is named.
MAX_RECURSION = 8
# ...and the parser's own cycle is told apart from the destructor's by name.
# They are templates: GCC's own label puts the arguments after the signature
# (`[with bool kKeeps = false]`), a demangled name before it (`ParseValue<false>(`).
PARSER = re.compile(r"rommsync::json::Parser::Parse(Value|Object|Array)(?:<[^()]*>)?\(")

# The indirect call this walk resolves, and why. See the module docstring.
# Where the call is made: `ServiceCore`'s handlers, or `ipc::Dispatch` once LTO
# has inlined them into it.
ENGINE_CALLER = re.compile(r"rommsync::ipc::(?:ServiceCore::|Dispatch\()")
ENGINE_WHY = "ipc::ServiceCore holds an ipc::Engine&, and the sysmodule's only ipc::Engine is SdEngine"
# What it may reach: an `SdEngine` method named like one of `ipc::Engine`'s
# virtuals, where the name starts or follows a return type -- so not a template
# instantiated with one (`EnqueueAndStore<...SdEngine::Enqueue...>`). A lambda
# inside one starts the same way, and `resolve_indirect` drops it by name.
ENGINE_CALLEE = r"(?:^|[ *&])rommsync::sysmodule::SdEngine::(?:%s)\("

# The PSC watcher's thread is nothing but indirect calls (#268): `power::Watcher`
# holds a `power::Module&` and a `power::Sink&`, and LTO inlines its `Run` into
# the thread's `Enter`. Unresolved, that thread measures as `Enter` alone. The
# sysmodule's only `power::Sink` is `SdEngine` and its only `power::Module` is
# `power_psc.cpp`'s `PscModule`, so each virtual is resolved to that override.
POWER_HPP = "sysmodule/source/power.hpp"
WATCHER_CALLER = re.compile(r"rommsync::sysmodule::(?:power::Watcher::Run\(|"
                            r"SizedThread::Enter\b.*::PscSubscription::Run\b)")
SINK_WHY = "power::Watcher holds a power::Sink&, and the sysmodule's only power::Sink is SdEngine"
MODULE_WHY = ("power::Watcher holds a power::Module&, and the sysmodule's only power::Module is "
              "power_psc.cpp's PscModule")
# Anonymous: demangled `(anonymous namespace)`, GCC's own label `{anonymous}`.
MODULE_CALLEE = (r"(?:^|[ *&])rommsync::sysmodule::power::(?:\(anonymous namespace\)|"
                 r"\{anonymous\})::PscModule::(?:%s)\(")

NODE = re.compile(r'^node: \{ title: "(?P<title>[^"]*)" label: "(?P<label>(?:[^"\\]|\\.)*)"')
EDGE = re.compile(r'^edge: \{ sourcename: "(?P<source>[^"]*)" targetname: "(?P<target>[^"]*)"')
FRAME = re.compile(r"\\n(\d+) bytes \((?:static|dynamic)")
# `dynamic` without `,bounded` is an alloca or a VLA whose size GCC cannot
# bound, so the figure beside it is a floor and no budget can hold it.
UNBOUNDED = re.compile(r"\\n\d+ bytes \(dynamic\)")
# An LTRANS node's title is its assembler name, prefixed `<ltrans object>:`
# when it is local to that partition.
LTRANS_SYMBOL = re.compile(r"^(?:.*\.o:)?(?P<symbol>_Z[^:]*)$")


def fail(message):
    print("FAIL: " + message)
    return 1


def read(repo, relative):
    with open(os.path.join(repo, relative), "r", encoding="utf-8") as handle:
        return handle.read()


# --- the tree --------------------------------------------------------------


def npdm_stack(repo):
    value = json.loads(read(repo, NPDM)).get("main_thread_stack_size")
    if value is None:
        return None
    return int(value, 16) if isinstance(value, str) else int(value)


def max_depth(repo):
    found = re.search(r"inline\s+constexpr\s+int\s+kMaxDepth\s*=\s*(\d+)\s*;", read(repo, JSON_HPP))
    return int(found.group(1)) if found else None


def budget(repo):
    """Every `inline constexpr std::size_t kName = <number>;` in the budget
    header and in `sized_thread.hpp`, which holds the other threads' stacks."""
    numbers = {}
    for header in (BUDGET, SIZED_THREAD):
        if not os.path.exists(os.path.join(repo, header)):
            continue
        for name, value in re.findall(
                r"inline\s+constexpr\s+std::size_t\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;",
                read(repo, header)):
            numbers[name] = int(value, 0)
    return numbers


def _thread_start_lines(path, text):
    """`path:line` for each line of `text` that starts a thread, comments aside."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        if THREAD_START.search(line.split("//", 1)[0]):
            found.append("%s:%d" % (path, number))
    return found


def thread_starts(repo):
    """Every place the sysmodule's sources start a thread other than through
    `SizedThread` -- which would be a stack no row of `THREADS` walks."""
    found = []
    for top in THREAD_SOURCES:
        for root, _, names in os.walk(os.path.join(repo, top)):
            for name in sorted(names):
                path = os.path.relpath(os.path.join(root, name), repo)
                if path == SIZED_THREAD or not name.endswith((".cpp", ".hpp", ".h", ".c")):
                    continue
                found += _thread_start_lines(path, read(repo, path))
    return sorted(found)


def virtual_methods(repo, header, cls):
    """The names `class cls` in `header` declares virtual, its destructor
    aside: what an indirect call through a reference to it can reach."""
    block = re.search(r"\bclass\s+%s\s*\{(.*?)\n\};" % cls, read(repo, header), re.S)
    if not block:
        return []
    names = set()
    for head in re.findall(r"\bvirtual\s+([^;(]*)\(", block.group(1)):
        name = re.search(r"(~?\w+)\s*$", head)
        if name and not name.group(1).startswith("~"):
            names.add(name.group(1))
    return sorted(names)


def engine_methods(repo):
    return virtual_methods(repo, IPC_HPP, "Engine")


def power_methods(repo):
    """(`power::Sink`'s virtuals, `power::Module`'s), for the watcher's rules."""
    return virtual_methods(repo, POWER_HPP, "Sink"), virtual_methods(repo, POWER_HPP, "Module")


def _rule(caller, callee, methods, why):
    if not methods:
        return (caller, None, why)
    return (caller, re.compile(callee % "|".join(map(re.escape, methods))), why)


def indirect_rules(methods, power=None):
    """(where the call is made, what it may reach, why), for `measure`. `power`
    is `power_methods`, and adds the watcher thread's rules."""
    rules = [_rule(ENGINE_CALLER, ENGINE_CALLEE, methods, ENGINE_WHY)]
    if power is not None:
        sink, module = power
        rules += [_rule(WATCHER_CALLER, ENGINE_CALLEE, sink, SINK_WHY),
                  _rule(WATCHER_CALLER, MODULE_CALLEE, module, MODULE_WHY)]
    return rules


def worst_case(depth, level, rest):
    return (depth + 1) * level + rest


# --- the call graph --------------------------------------------------------


class Graph:
    """What `-fcallgraph-info=su` says, over every translation unit at once.

    A global function is one node whichever unit names it; a local one is
    titled `<file>:<symbol>` by GCC already, so two units' statics cannot merge.
    A comdat inline emitted by several units is the same title in each, and
    keeps the largest frame any of them reports.

    `ltrans=True` reads an LTRANS unit's graph, whose labels carry bare
    identifiers: the node's mangled title is kept aside, and `demangle` later
    names it by that instead.
    """

    def __init__(self):
        self.frame = {}
        self.label = {}
        self.calls = {}
        self.unbounded = set()
        self.mangled = {}

    def add(self, text, ltrans=False):
        for line in text.splitlines():
            node = NODE.match(line)
            if node:
                title, label = node.group("title"), node.group("label")
                self.label.setdefault(title, label.split("\\n")[0])
                symbol = LTRANS_SYMBOL.match(title) if ltrans else None
                if symbol:
                    self.mangled[title] = symbol.group("symbol")
                frame = FRAME.search(label)
                if frame:
                    self.frame[title] = max(self.frame.get(title, 0), int(frame.group(1)))
                if UNBOUNDED.search(label):
                    self.unbounded.add(title)
                continue
            edge = EDGE.match(line)
            if edge:
                self.calls.setdefault(edge.group("source"), set()).add(edge.group("target"))

    def demangle(self, demangler):
        """Name every LTRANS node by its demangled title. `demangler` takes a
        list of assembler names and returns their demangled forms, in order.
        Returns the problems."""
        titles = sorted(self.mangled)
        if not titles:
            return []
        names = demangler([self.mangled[t] for t in titles])
        if len(names) != len(titles):
            return ["the demangler returned %d names for %d symbols" % (len(names), len(titles))]
        for title, name in zip(titles, names):
            self.label[title] = name
        return []

    def resolve_indirect(self, rules):
        """Apply `rules` (`indirect_rules`). Returns the problems: a rule that
        matched nothing has rotted, and would silently stop following the IPC
        handlers."""
        problems = []
        for caller, callee, why in rules:
            if callee is None:
                problems.append("no virtual method found for the indirect-call rule at `%s`, "
                                "which exists because %s; the walk cannot say what those calls "
                                "reach" % (caller.pattern, why))
                continue
            sources = [t for t, out in self.calls.items()
                       if "__indirect_call" in out and caller.search(self.label.get(t, ""))]
            targets = [t for t, label in self.label.items()
                       if callee.search(label) and "{lambda" not in label]
            if not sources or not targets:
                problems.append("the indirect-call rule `%s` -> `%s` matched %d callers and %d "
                                "callees; it exists because %s, and a rule that matches nothing "
                                "stops the walk following those calls"
                                % (caller.pattern, callee.pattern, len(sources), len(targets), why))
                continue
            for source in sources:
                self.calls[source].update(targets)
        return problems

    def name(self, title):
        return self.label.get(title, title)


def strongly_connected(graph, roots):
    """Tarjan's, iteratively: the build's chains are deep enough that Python's
    own recursion limit is a real one. Components come out callees first."""
    index, low, on_stack, stack, components = {}, {}, set(), [], []
    counter = [0]
    for root in roots:
        if root in index:
            continue
        work = [(root, iter(sorted(graph.calls.get(root, ()))))]
        index[root] = low[root] = counter[0]
        counter[0] += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            node, children = work[-1]
            advanced = False
            for child in children:
                if child not in index:
                    index[child] = low[child] = counter[0]
                    counter[0] += 1
                    stack.append(child)
                    on_stack.add(child)
                    work.append((child, iter(sorted(graph.calls.get(child, ())))))
                    advanced = True
                    break
                if child in on_stack:
                    low[node] = min(low[node], index[child])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
            if low[node] == index[node]:
                component = []
                while True:
                    member = stack.pop()
                    on_stack.discard(member)
                    component.append(member)
                    if member == node:
                        break
                components.append(component)
    return components


def heaviest_cycle(graph, members):
    """The most stack one trip round a recursion can take. The recursions this
    allows are a handful of functions each, so every simple cycle is tried."""
    inside = set(members)
    best = 0
    for start in members:
        path = [(start, iter(sorted(graph.calls.get(start, set()) & inside)))]
        seen = {start}
        cost = graph.frame.get(start, 0)
        while path:
            node, children = path[-1]
            step = next(children, None)
            if step is None:
                path.pop()
                seen.discard(node)
                cost -= graph.frame.get(node, 0)
                continue
            if step == start:
                best = max(best, cost)
            elif step not in seen and step > start:
                # `step > start`: each cycle is found once, from its least member.
                seen.add(step)
                cost += graph.frame.get(step, 0)
                path.append((step, iter(sorted(graph.calls.get(step, set()) & inside))))
    return best


class Measured:
    def __init__(self):
        self.problems = []
        self.level = 0          # the parser's heaviest cycle
        self.rest = 0           # everything else on the parser's deepest chain
        self.other_levels = {}  # other bounded recursions, by a member's name
        self.other_rest = 0     # the deepest chain that does not parse at all
        self.chain = []         # the deepest chain, for the report
        self.other_chain = []   # the deepest chain that does not parse, likewise
        self.sized = {}         # the costliest size-bounded recursion of each kind, whole

    def worst(self, depth):
        """The deepest parsing chain at `depth`; nothing, on a thread that never parses."""
        return worst_case(depth, self.level, self.rest) if self.chain else 0


def measure(graph, depth, rules):
    """The main thread's deepest chain, split into what scales with `depth` and
    what does not."""
    problems = graph.resolve_indirect(rules)
    roots = [r for r in ROOTS if r in graph.calls or r in graph.frame]
    if "main" not in roots:
        result = Measured()
        result.problems = problems + ["no `main` in the call graph; is this a sysmodule build "
                                      "with -fcallgraph-info=su?"]
        return result
    result = measure_thread(graph, depth, roots, THREAD["main"])
    result.problems = problems + result.problems
    return result


def thread_entries(graph):
    """Each `SizedThread` row's entry titles, by thread name. Returns (entries,
    problems): an entry no row claims is a thread nothing measures, and a row
    whose entry is gone has rotted."""
    entries, problems = {}, []
    for title in sorted(t for t, label in graph.label.items() if ENTER.search(label)):
        owners = [t for t in THREADS if t.entry and t.entry.search(graph.name(title))]
        if len(owners) != 1:
            problems.append("`%s` starts a thread that %s row of THREADS walks; every stack this "
                            "process runs on is measured (#268)"
                            % (graph.name(title), "no" if not owners else "more than one"))
            continue
        entries.setdefault(owners[0].name, []).append(title)
    for thread in THREADS:
        if thread.entry and thread.name not in entries:
            problems.append("no `SizedThread::Enter` for the %s thread (`%s`) in the call graph; "
                            "was it renamed?" % (thread.name, thread.entry.pattern))
    return entries, problems


def measure_thread(graph, depth, roots, thread):
    """One thread's deepest chains from `roots`, split into what scales with
    `depth` and what does not."""
    result = Measured()
    components = strongly_connected(graph, roots)
    reachable = {m for component in components for m in component}
    for title in sorted(graph.unbounded & reachable):
        result.problems.append("`%s` has a stack frame GCC cannot bound (an alloca or a "
                               "variable-length array) and is reachable on the %s thread; no "
                               "budget holds a frame whose size is decided at run time"
                               % (graph.name(title), thread.name))
    owner = {}
    for number, component in enumerate(components):
        for member in component:
            owner[member] = number

    # Per component: its level (0 when it does not recurse) and its own frame.
    level, own, parser = {}, {}, set()
    for number, component in enumerate(components):
        recursive = len(component) > 1 or component[0] in graph.calls.get(component[0], ())
        if not recursive:
            level[number], own[number] = 0, graph.frame.get(component[0], 0)
            continue
        names = sorted(graph.name(m) for m in component)
        sized = [(kind, levels) for kind, pattern, levels in SIZE_BOUNDED
                 if all(pattern.search(n) for n in names)]
        if sized and len(component) <= MAX_RECURSION:
            # Bounded by the size of what it walks, not by any document: it
            # adds to the chain like a frame of its own rather than sharing a budget.
            kind, levels = sized[0]
            level[number], own[number] = 0, levels * heaviest_cycle(graph, component)
            result.sized[kind] = max(result.sized.get(kind, 0), own[number])
            continue
        if not all(BOUNDED.search(n) for n in names) or len(component) > MAX_RECURSION:
            # Not costed: there is no number to give it, and enumerating the
            # cycles of a large accidental component is exponential.
            result.problems.append("a recursion on the %s thread that json::kMaxDepth does not "
                                   "bound, so no stack size is safe against it: %s"
                                   % (thread.name, " -> ".join(names[:MAX_RECURSION])))
            level[number], own[number] = 0, 0
            continue
        if not thread.parses:
            result.problems.append("the %s thread reaches the JSON reader (%s), and its budget "
                                   "is the one a thread that parses nothing gets; give it a "
                                   "rest ceiling in THREADS (#268)" % (thread.name, names[0]))
        level[number], own[number] = heaviest_cycle(graph, component), 0
        if any(PARSER.search(n) for n in names):
            parser.add(number)
        else:
            result.other_levels[names[0]] = level[number]

    # Longest path over the condensation, callees first (Tarjan's order). Each
    # component keeps, per heaviest level crossed, the most non-recursive stack
    # any chain below it spends -- a Pareto front with very few entries.
    best = {}
    for number, component in enumerate(components):
        below = {}
        for member in component:
            for callee in graph.calls.get(member, ()):
                target = owner.get(callee)
                if target is None or target == number:
                    continue
                for key, (cost, chain) in best[target].items():
                    if key not in below or cost > below[key][0]:
                        below[key] = (cost, chain)
        if not below:
            below = {(0, False): (0, [])}
        mine = {}
        for (key_level, through_parser), (cost, chain) in below.items():
            key = (max(key_level, level[number]), through_parser or number in parser)
            entry = (cost + own[number], [component[0]] + chain)
            if key not in mine or entry[0] > mine[key][0]:
                mine[key] = entry
        best[number] = mine

    fronts = [best[owner[r]] for r in roots]
    for front in fronts:
        for (key_level, through_parser), (cost, chain) in front.items():
            if through_parser:
                if cost > result.rest or not result.chain:
                    result.rest, result.chain = cost, chain
            else:
                # A chain that never parses can still cross the destructor; its
                # recursion costs what it costs at this depth, and lands in rest.
                whole = cost + (depth + 1) * key_level
                if whole > result.other_rest or not result.other_chain:
                    result.other_rest, result.other_chain = whole, chain
    result.level = max((level[n] for n in parser), default=0)
    if not parser and thread.parses:
        result.problems.append("no json::Parser recursion reachable on the %s thread; the walk "
                               "is not seeing the parser this budget is about" % thread.name)
    for name, other in result.other_levels.items():
        if other > result.level:
            result.problems.append("the recursion through `%s` costs %d bytes a level, more than "
                                   "the parser's %d; this budget assumes the parser's is the heaviest"
                                   % (name, other, result.level))
    return result


def judge(thread, measured, numbers, depth):
    """What is wrong with one thread's measured chains against the ceilings in
    `numbers` (`budget`). The ceilings are checked, and so is the stack itself:
    a ceiling is only the record of a measurement, and the stack is what
    overflows. Half of it, for the reason the module docstring gives."""
    problems = []
    stack = numbers.get(thread.stack, 0)
    half = stack // 2
    level_ceiling = numbers.get("kJsonLevelBytes", 0)
    if measured.level > level_ceiling:
        problems.append("a JSON nesting level now costs %d bytes on the %s thread and %s budgets "
                        "%d for it; raise kJsonLevelBytes and see whether `stack.fit` still "
                        "passes (#216)" % (measured.level, thread.name, BUDGET, level_ceiling))
    if thread.rest and measured.rest > numbers.get(thread.rest, 0):
        problems.append("the %s thread spends %d bytes on its deepest parsing chain beside the "
                        "levels themselves and %s budgets %d (%s); raise it and see whether "
                        "`stack.fit` still passes (#268)"
                        % (thread.name, measured.rest, BUDGET, numbers.get(thread.rest, 0),
                           thread.rest))
    worst = measured.worst(depth)
    for what, cost in (("deepest parsing chain, at kMaxDepth %d," % depth, worst),
                       ("deepest chain that parses nothing", measured.other_rest)):
        if cost > half:
            problems.append("the %s thread's %s is %d bytes, more than half its %s stack (%s, "
                            "%d bytes); the other half is for what no static walk sees, and a "
                            "Horizon stack overflow is a data abort with no crash report (#268)"
                            % (thread.name, what, cost, hex(stack), thread.stack, half))
    return problems


def report(graph, thread, measured, depth):
    print("measured, %s thread: a JSON level is %d bytes; the rest of the deepest parsing chain "
          "is %d bytes" % (thread.name, measured.level, measured.rest))
    print("measured, %s thread: at kMaxDepth %d that chain is %d bytes; the deepest chain that "
          "does not parse is %d bytes"
          % (thread.name, depth,
             measured.worst(depth),
             measured.other_rest))
    for kind, _, levels in SIZE_BOUNDED:
        if kind in measured.sized:
            print("measured, %s thread: the costliest %s recursion is %d bytes at %d levels"
                  % (thread.name, kind, measured.sized[kind], levels))
    deeper = measured.chain
    if not deeper or measured.other_rest > measured.worst(depth):
        deeper = measured.other_chain
    for title in deeper:
        print("  %6d  %s" % (graph.frame.get(title, 0), graph.name(title)[:150]))


# --- phases ----------------------------------------------------------------


def phase_fit(repo):
    stack = npdm_stack(repo)
    depth = max_depth(repo)
    numbers = budget(repo)
    if stack is None:
        return fail("%s has no main_thread_stack_size" % NPDM)
    if depth is None:
        return fail("no `inline constexpr int kMaxDepth = N;` in %s" % JSON_HPP)
    wanted = ["kJsonLevelBytes"] + [n for t in THREADS for n in (t.stack, t.rest) if n]
    missing = [n for n in wanted if n not in numbers]
    if missing:
        return fail("neither %s nor %s defines %s" % (BUDGET, SIZED_THREAD, ", ".join(missing)))
    failures = 0
    if numbers["kMainThreadStackBytes"] != stack:
        failures += fail("%s gives the main thread %s and %s says kMainThreadStackBytes is %s; "
                         "they are one number (#216)"
                         % (NPDM, hex(stack), BUDGET, hex(numbers["kMainThreadStackBytes"])))
    heap = inner_heap(repo)
    if heap is None:
        failures += fail("no `constexpr size_t kInnerHeapSize = N;` in %s; the red-black tree and "
                         "introsort level counts are argued from it" % MAIN_CPP)
    else:
        for problem in size_bound_problems(heap):
            failures += fail(problem + "; raise it in scripts/stack-budget.py (#268)")
    for where in thread_starts(repo):
        failures += fail("%s starts a thread without `SizedThread`, so no row of THREADS walks "
                         "its stack (#268)" % where)
    fits = []
    for thread in THREADS:
        if not thread.rest:
            continue
        worst = worst_case(depth, numbers["kJsonLevelBytes"], numbers[thread.rest])
        size = numbers[thread.stack]
        half = size // 2
        if worst > half:
            failures += fail(
                "the deepest JSON document the parser accepts costs the %s thread up to %d bytes "
                "-- (kMaxDepth %d + 1) x %d a level + %s %d beside it -- and the budget is half "
                "of its %s stack (%s), %d bytes. The other half is for the indirect calls no "
                "walk follows, and a Horizon stack overflow is a data abort with no crash report "
                "(#216, #268)" % (thread.name, worst, depth, numbers["kJsonLevelBytes"],
                                  thread.rest, numbers[thread.rest], hex(size), thread.stack,
                                  half))
        fits.append("the %s thread at most %d bytes of its %s (%d to spare)"
                    % (thread.name, worst, hex(size), half - worst))
    if failures:
        return 1
    print("ok: kMaxDepth %d costs %s, inside the half of each stack this budget allows"
          % (depth, "; ".join(fits)))
    return 0


def schema_depth(schema, schemas, seen):
    """How deep a value of this schema nests, counted the way the parser counts:
    the root is depth 0 and each array or object puts its children one deeper.
    A free-form object counts as one level -- its contents are not RomM's to
    promise -- and a recursive `$ref` is a failure, because it is unbounded."""
    if "$ref" in schema:
        name = schema["$ref"].split("/")[-1]
        if name in seen:
            raise ValueError("schema %s refers to itself" % name)
        return schema_depth(schemas[name], schemas, seen | {name})
    deepest = 0
    for key in ("anyOf", "oneOf", "allOf"):
        for option in schema.get(key, []):
            deepest = max(deepest, schema_depth(option, schemas, seen))
    kind = schema.get("type")
    if kind == "object" or "properties" in schema or "additionalProperties" in schema:
        inner = 0
        for child in schema.get("properties", {}).values():
            inner = max(inner, schema_depth(child, schemas, seen))
        extra = schema.get("additionalProperties")
        if isinstance(extra, dict) and extra:
            inner = max(inner, schema_depth(extra, schemas, seen))
        deepest = max(deepest, 1 + inner)
    if kind == "array":
        deepest = max(deepest, 1 + schema_depth(schema.get("items", {}), schemas, seen))
    return deepest


def phase_contract(repo):
    depth = max_depth(repo)
    if depth is None:
        return fail("no `inline constexpr int kMaxDepth = N;` in %s" % JSON_HPP)
    snapshots = sorted(glob.glob(os.path.join(repo, CONTRACTS)))
    if not snapshots:
        return fail("no pinned OpenAPI snapshot matches %s" % CONTRACTS)
    deepest, where = 0, ""
    for path in snapshots:
        spec = json.load(open(path, encoding="utf-8"))
        schemas = spec.get("components", {}).get("schemas", {})
        for route, operations in spec.get("paths", {}).items():
            for verb, operation in operations.items():
                if not isinstance(operation, dict):
                    continue
                for status, response in operation.get("responses", {}).items():
                    for content in response.get("content", {}).values():
                        if "schema" not in content:
                            continue
                        try:
                            found = schema_depth(content["schema"], schemas, set())
                        except ValueError as error:
                            return fail("%s %s %s in %s: %s; nothing bounds how deep that nests"
                                        % (verb.upper(), route, status,
                                           os.path.basename(path), error))
                        if found > deepest:
                            deepest = found
                            where = "%s %s %s (%s)" % (verb.upper(), route, status,
                                                       os.path.basename(path))
    if deepest * 2 > depth:
        return fail("RomM's deepest response shape, %s, nests %d deep and kMaxDepth is %d. The "
                    "limit is meant to sit at twice the deepest shape any pinned RomM promises, "
                    "so a free-form field inside it still has room; raising it is a stack "
                    "budget question first (#216)" % (where, deepest, depth))
    print("ok: RomM's deepest response shape, %s, nests %d deep; kMaxDepth %d is at least "
          "twice that" % (where, deepest, depth))
    return 0


def find_demangler():
    """The toolchain's c++filt: the one that demangles what this GCC mangled."""
    tool = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"),
                        "devkitA64", "bin", "aarch64-none-elf-c++filt")
    return tool if os.access(tool, os.X_OK) else None


def run_demangler(tool):
    def demangle(symbols):
        # One symbol a line on stdin, one name a line back: a single process
        # for the whole graph rather than one per node.
        done = subprocess.run([tool], input="\n".join(symbols) + "\n", capture_output=True,
                              text=True, check=True)
        return done.stdout.splitlines()
    return demangle


def load_graph(directory, demangler=None):
    """Every `.ci` under `directory`. Returns (graph, files, problems)."""
    graph = Graph()
    files = sorted(os.path.join(root, name) for root, _, names in os.walk(directory)
                   for name in names if name.endswith(".ci"))
    ltrans = [f for f in files if f.endswith(".ltrans.ci")]
    if ltrans and len(ltrans) != len(files):
        return graph, files, ["%s holds both LTRANS call graphs (%d) and per-unit ones (%d); only "
                              "one of them is the code that ships -- measure a clean build"
                              % (directory, len(ltrans), len(files) - len(ltrans))]
    for path in files:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            graph.add(handle.read(), ltrans=bool(ltrans))
    if not graph.mangled:
        return graph, files, []
    if demangler is None:
        return graph, files, ["these are LTRANS call graphs, named only by their mangled titles, "
                              "and there is no devkitA64 c++filt to read them with; run this in "
                              "the image"]
    try:
        return graph, files, graph.demangle(run_demangler(demangler))
    except (OSError, subprocess.CalledProcessError) as error:
        return graph, files, ["%s could not demangle the graph: %s" % (demangler, error)]


def phase_measured(repo, directory):
    graph, files, problems = load_graph(directory, find_demangler())
    if not files:
        return fail("no .ci files under %s; build with DEFINES=\"-fstack-usage "
                    "-fcallgraph-info=su\"" % directory)
    if problems:
        for problem in problems:
            fail(problem)
        return 1
    depth = max_depth(repo)
    if depth is None:
        return fail("no `inline constexpr int kMaxDepth = N;` in %s" % JSON_HPP)
    # The main thread's stack is `kMainThreadStackBytes`, which `fit` holds to
    # the npdm, so every thread's size comes out of the headers alike.
    numbers = budget(repo)
    missing = [n for t in THREADS for n in (t.stack, t.rest) if n and n not in numbers]
    if missing:
        # Rather than holding a chain to half of a stack of zero bytes.
        return fail("neither %s nor %s defines %s" % (BUDGET, SIZED_THREAD, ", ".join(missing)))
    # The main thread's walk resolves the indirect calls first, and every
    # thread after it walks the same graph.
    main = measure(graph, depth, indirect_rules(engine_methods(repo), power_methods(repo)))
    entries, problems = thread_entries(graph)
    walked = [(THREAD["main"], main)]
    walked += [(t, measure_thread(graph, depth, entries[t.name], t))
               for t in THREADS if t.name in entries]
    failures = 0
    for problem in problems:
        failures += fail(problem)
    for thread, measured in walked:
        report(graph, thread, measured, depth)
        for problem in measured.problems + judge(thread, measured, numbers, depth):
            failures += fail(problem)
    if failures:
        return 1
    print("ok: measured over %d units, every thread within the ceilings %s and %s record"
          % (len(files), BUDGET, SIZED_THREAD))
    return 0


# --- the walk's own test ---------------------------------------------------


def _ci(nodes, edges):
    """A `.ci` file. nodes: (title, label, frame or None); edges: (from, to)."""
    lines = ['graph: { title: "fixture"']
    for title, label, frame in nodes:
        suffix = "\\n%d bytes (static)" % frame if frame is not None else ""
        lines.append('node: { title: "%s" label: "%s\\nfile.cpp:1:1%s" }' % (title, label, suffix))
    for source, target in edges:
        lines.append('edge: { sourcename: "%s" targetname: "%s" }' % (source, target))
    lines.append("}")
    return "\n".join(lines) + "\n"


PV = ("PV", "bool rommsync::json::Parser::ParseValue(int, Value*)")
PO = ("PO", "bool rommsync::json::Parser::ParseObject(int, Value*)")
PA = ("PA", "bool rommsync::json::Parser::ParseArray(int, Value*)")
PARSE = ("Parse", "rommsync::json::ParseResult rommsync::json::Parse(std::string_view)")
DV = ("DV", "constexpr rommsync::json::Value::~Value()")
DM = ("DM", "constexpr std::vector<_Tp, _Alloc>::~vector() [with _Tp = rommsync::json::Member]")
CORE = ("Core", "rommsync::ipc::Error rommsync::ipc::ServiceCore::RestoreBackup(long)")
RESTORE = ("Restore",
           "virtual rommsync::ipc::RestoreState rommsync::sysmodule::SdEngine::RequestRestore(long)")


_ENTER = "void* rommsync::sysmodule::SizedThread::Enter<&%s, %s>(void*)"
_ENGINE = "rommsync::sysmodule::SdEngine"
_PSC = "rommsync::sysmodule::power::(anonymous namespace)::PscSubscription"
THREAD_NODES = [
    ("EnterW", _ENTER % (_ENGINE + "::RunWorker", _ENGINE), 16),
    ("Worker", "void rommsync::sysmodule::SdEngine::RunWorker()", 1000),
    ("Sort", "void std::__introsort_loop<long*, long, __gnu_cxx::__ops::_Iter_less_iter>"
             "(long*, long*, long, __gnu_cxx::__ops::_Iter_less_iter)", 100),
    ("EnterP", _ENTER % (_ENGINE + "::DrivePairing", _ENGINE), 160),
    ("Pairing", "void rommsync::sysmodule::SdEngine::DrivePairing()", 500),
    ("EnterS", _ENTER % (_PSC + "::Run", _PSC), 64),
    ("Run", "void rommsync::sysmodule::power::Watcher::Run()", 200),
    ("Quiesce", "virtual void rommsync::sysmodule::SdEngine::Quiesce()", 300),
    ("Next", "virtual bool rommsync::sysmodule::power::(anonymous namespace)::PscModule::"
             "NextRequest(rommsync::sysmodule::power::State*)", 100),
]
# The watcher reaches its sink and its module only indirectly, as it does in
# the build.
THREAD_EDGES = [("EnterW", "Worker"), ("Worker", "Parse"), ("Worker", "Sort"), ("Sort", "Sort"),
                ("EnterP", "Pairing"), ("Pairing", "Parse"),
                ("EnterS", "Run"), ("Run", "__indirect_call")]


def _ltrans(nodes, edges):
    """The same graph as LTO's back end writes it: each C++ function titled by
    an assembler name -- `Str` local to its partition -- and labelled by its
    bare identifier. Returns the text and a demangler for it, which, like
    c++filt, says nothing of `virtual`."""
    titles, names = {}, {}
    out = []
    for title, label, frame in nodes:
        if "::" not in label:
            titles[title] = title
            out.append((title, label, frame))
            continue
        symbol = "_Z" + title
        titles[title] = ("/tmp/cc0.ltrans3.o:" if title == "Str" else "") + symbol
        names[symbol] = label.replace("virtual ", "")
        out.append((titles[title], re.search(r"(~?\w+)\(", label).group(1), frame))
    text = _ci(out, [(titles[a], titles[b]) for a, b in edges])
    return text, lambda symbols: [names[s] for s in symbols]


def _fixture(extra_nodes=(), extra_edges=(), destructor=50, ltrans=False):
    nodes = [("main", "int main(int, char**)", 1000), ("__appInit", "void __appInit()", 300),
             ("Load", "void SdEngine::Load()", 500),
             (PARSE[0], PARSE[1], 300), (PV[0], PV[1], 100), (PO[0], PO[1], 300),
             (PA[0], PA[1], 200), ("Str", "bool rommsync::json::Parser::ParseString()", 60),
             ("Fail", "bool rommsync::json::Parser::Fail()", 40),
             (DV[0], DV[1], 30), (DM[0], DM[1], destructor),
             (CORE[0], CORE[1], 100), (RESTORE[0], RESTORE[1], 2000),
             ("__indirect_call", "Indirect Call Placeholder", None),
             ("memcpy", "void* memcpy()", None)]
    edges = [("main", "Load"), ("Load", "Parse"), ("Parse", "PV"),
             ("PV", "PO"), ("PV", "PA"), ("PO", "PV"), ("PA", "PV"),
             ("PO", "Str"), ("PV", "Fail"), ("Str", "memcpy"),
             ("PO", "DV"), ("DV", "DM"), ("DM", "DV"),
             ("main", "Core"), ("Core", "__indirect_call")]
    if ltrans:
        return _ltrans(nodes + list(extra_nodes), edges + list(extra_edges))
    return _ci(list(nodes) + list(extra_nodes), edges + list(extra_edges))


def phase_walk(repo):
    failures = 0

    def check(what, observed, expected):
        nonlocal failures
        if observed != expected:
            failures += fail("walk: %s: got %r, expected %r" % (what, observed, expected))

    # What an `ipc::Engine&` can reach, read out of the real header.
    methods = engine_methods(repo)
    check("engine methods include RequestRestore", "RequestRestore" in methods, True)
    check("engine methods leave out power::Sink's", "Quiesce" in methods, False)
    check("engine methods leave out the destructor", [m for m in methods if "~" in m], [])
    rules = indirect_rules(methods)
    check("no engine methods is a problem",
          len(Graph().resolve_indirect(indirect_rules([]))), 1)

    graph = Graph()
    graph.add(_fixture())
    measured = measure(graph, 4, rules)
    check("problems", measured.problems, [])
    # PV 100 + max(PO 300, PA 200): the heavier of the parser's two cycles.
    check("level", measured.level, 400)
    # main 1000 + Load 500 + Parse 300 above the levels, and below them the
    # heavier tail: Str 60 (PO -> Str -> memcpy, memcpy counting zero) beats
    # Fail 40. The destructor adds nothing here: its 80 a level shares the
    # parser's depth budget, and 80 < 400. 1000 + 500 + 300 + 60.
    check("rest", measured.rest, 1860)
    # main -> Core -> (resolved) Restore: 1000 + 100 + 2000.
    check("other", measured.other_rest, 3100)
    check("destructor level", measured.other_levels, {DV[1]: 80})
    check("worst at depth 4", worst_case(4, measured.level, measured.rest), 3860)

    # The same graph as an LTRANS unit writes it (#253) measures the same,
    # once its titles are demangled...
    text, demangle = _fixture(ltrans=True)
    graph = Graph()
    graph.add(text, ltrans=True)
    check("ltrans demangled", graph.demangle(demangle), [])
    measured = measure(graph, 4, rules)
    check("ltrans problems", measured.problems, [])
    check("ltrans level", measured.level, 400)
    check("ltrans rest", measured.rest, 1860)
    check("ltrans other", measured.other_rest, 3100)
    # ...and with ServiceCore's handler inlined into `ipc::Dispatch`, the
    # indirect call is Dispatch's and still reaches the engine.
    text, demangle = _fixture(ltrans=True)
    graph = Graph()
    graph.add(text, ltrans=True)
    graph.demangle(lambda symbols: [n.replace("ipc::ServiceCore::RestoreBackup(",
                                              "ipc::Dispatch(rommsync::ipc::ServiceCore&, ")
                                    for n in demangle(symbols)])
    check("ltrans dispatch other", measure(graph, 4, rules).other_rest, 3100)
    check("a demangled parser instantiation is the parser",
          bool(PARSER.search("bool rommsync::json::Parser::ParseObject<false, true>(int, "
                             "rommsync::json::Value*)")), True)
    # Read by its bare labels, as the issue found it, the parser is invisible.
    graph = Graph()
    graph.add(_fixture(ltrans=True)[0], ltrans=True)
    check("ltrans undemangled loses the parser",
          [p for p in measure(graph, 4, rules).problems if "no json::Parser" in p] != [], True)

    # The indirect rule has to match, or the IPC handlers vanish from the walk.
    graph = Graph()
    graph.add(_fixture().replace("ServiceCore", "SomethingElse"))
    measured = measure(graph, 4, rules)
    check("unmatched rule reported", len(measured.problems), 1)
    check("unmatched rule loses the handler", measured.other_rest, 1100)

    # A red-black tree's recursion is bounded by its height, and costs
    # TREE_LEVELS trips round its cycle wherever it is called: 1100 + 32 * 24.
    graph = Graph()
    graph.add(_fixture([("Erase", "void std::_Rb_tree<K, V>::_M_erase(_Node_ptr)", 24)],
                       [("Core", "Erase"), ("Erase", "Erase")]))
    measured = measure(graph, 4, rules)
    check("tree problems", measured.problems, [])
    check("tree cost", measured.sized, {"red-black tree": 768})
    check("tree chain", measured.other_rest, 3100)
    graph = Graph()
    graph.add(_fixture([("Erase", "void std::_Rb_tree<K, V>::_M_erase(_Node_ptr)", 100)],
                       [("Core", "Erase"), ("Erase", "Erase")]))
    check("tree chain when it is the deepest", measure(graph, 4, rules).other_rest, 1100 + 3200)

    # A recursion outside the JSON reader is not one any depth bounds.
    graph = Graph()
    graph.add(_fixture([("Walk", "void rommsync::scan::Walk(Dir)", 200)],
                       [("main", "Walk"), ("Walk", "Walk")]))
    measured = measure(graph, 4, rules)
    check("unbounded recursion reported",
          [p for p in measured.problems if "does not bound" in p] != [], True)

    # A frame GCC cannot size is one no budget holds.
    graph = Graph()
    graph.add(_fixture().replace('"%s\\nfile.cpp:1:1\\n2000 bytes (static)"' % RESTORE[1],
                                 '"%s\\nfile.cpp:1:1\\n2000 bytes (dynamic)"' % RESTORE[1]))
    check("unbounded frame reported",
          [p for p in measure(graph, 4, rules).problems if "cannot bound" in p] != [], True)
    graph = Graph()
    graph.add(_fixture().replace('"%s\\nfile.cpp:1:1\\n2000 bytes (static)"' % RESTORE[1],
                                 '"%s\\nfile.cpp:1:1\\n2000 bytes (dynamic,bounded)"'
                                 % RESTORE[1]))
    check("bounded dynamic frame counted", measure(graph, 4, rules).problems, [])

    # A destructor heavier than the parser breaks the shared-budget argument.
    graph = Graph()
    graph.add(_fixture(destructor=900))
    measured = measure(graph, 4, rules)
    check("heavy destructor reported",
          [p for p in measured.problems if "heaviest" in p] != [], True)

    # The threads `SizedThread` starts (#268). Each is walked from its
    # `Enter<...>` -- what `pthread_create` is handed -- with the main thread's
    # accounting, and each is held to half its own stack.
    threads_graph = _fixture(THREAD_NODES, THREAD_EDGES)
    power = power_methods(repo)
    check("sink methods", power[0], ["Quiesce", "Resume"])
    check("module methods include NextRequest", "NextRequest" in power[1], True)
    # Unresolved, the watcher is its first two frames and nothing else.
    graph = Graph()
    graph.add(threads_graph)
    entries, _ = thread_entries(graph)
    check("watcher unresolved", measure_thread(graph, 4, entries["PSC watcher"],
                                     THREAD["PSC watcher"]).other_rest, 264)
    graph = Graph()
    graph.add(threads_graph)
    check("thread rules resolve", graph.resolve_indirect(indirect_rules(methods, power)), [])
    entries, problems = thread_entries(graph)
    check("thread entries problems", problems, [])
    check("thread entries", sorted(entries), sorted(t.name for t in THREADS if t.name != "main"))
    worker = measure_thread(graph, 4, entries["worker"], THREAD["worker"])
    check("worker problems", worker.problems, [])
    # The parser's level is the same 400 whichever thread reaches it.
    check("worker level", worker.level, 400)
    # Enter 16 + RunWorker 1000 + Parse 300 above the levels, Str 60 below.
    check("worker rest", worker.rest, 1376)
    # Enter 16 + RunWorker 1000 + 41 trips round a 100-byte introsort level.
    check("worker sort", worker.sized, {"introsort": 4100})
    check("worker other", worker.other_rest, 5116)
    pairing = measure_thread(graph, 4, entries["pairing"], THREAD["pairing"])
    check("pairing problems", pairing.problems, [])
    # Enter 160 + DrivePairing 500 + Parse 300 + Str 60.
    check("pairing rest", pairing.rest, 1020)
    watcher = measure_thread(graph, 4, entries["PSC watcher"], THREAD["PSC watcher"])
    check("watcher problems", watcher.problems, [])
    # Enter 64 + Watcher::Run 200 + the heavier of the two resolved overrides,
    # Quiesce 300 -- and no level: it parses nothing.
    check("watcher level", watcher.level, 0)
    check("watcher other", watcher.other_rest, 564)
    # The main thread's walk does not wander into a thread's: nothing calls an
    # `Enter`, `pthread_create` is handed its address.
    check("main unchanged by threads", measure(graph, 4, rules).other_rest, 3100)

    # Held to its stack. The worker's sort chain is 5,116 bytes: inside half of
    # 0x4000 and outside half of 0x2000 -- the shrunken ceiling is a failure.
    ceilings = {"kJsonLevelBytes": 448, "kWorkerRestBytes": 2048, "kThreadStackBytes": 0x4000}
    check("worker fits 0x4000", judge(THREAD["worker"], worker, ceilings, 4), [])
    ceilings["kThreadStackBytes"] = 0x2000
    check("worker overflows 0x2000",
          [p for p in judge(THREAD["worker"], worker, ceilings, 4) if "worker" in p and "5116" in p]
          != [], True)
    ceilings["kThreadStackBytes"] = 0x4000
    ceilings["kWorkerRestBytes"] = 1024
    check("worker rest over its ceiling",
          [p for p in judge(THREAD["worker"], worker, ceilings, 4) if "kWorkerRestBytes" in p]
          != [], True)
    # A thread that parses nothing has no rest ceiling, and its stack still holds.
    check("watcher fits", judge(THREAD["PSC watcher"], watcher,
                                {"kJsonLevelBytes": 448, "kWatcherStackBytes": 0x1000}, 4), [])
    check("watcher overflows",
          judge(THREAD["PSC watcher"], watcher,
                {"kJsonLevelBytes": 448, "kWatcherStackBytes": 0x400}, 4) != [], True)

    # A thread nobody listed is a thread nobody measures; a listed one gone
    # from the graph is a row that has rotted.
    graph = Graph()
    graph.add(threads_graph.replace("SdEngine::DrivePairing,", "SdEngine::DriveSomething,"))
    _, problems = thread_entries(graph)
    check("unlisted thread reported", [p for p in problems if "DriveSomething" in p] != [], True)
    check("missing thread reported", [p for p in problems if "pairing" in p] != [], True)
    # The watcher's budget assumes it never parses; reaching the parser is a failure.
    graph = Graph()
    graph.add(_fixture(THREAD_NODES, THREAD_EDGES + [("Run", "Parse")]))
    entries, _ = thread_entries(graph)
    check("watcher parsing reported",
          [p for p in measure_thread(graph, 4, entries["PSC watcher"],
                                     THREAD["PSC watcher"]).problems
           if "parses nothing" in p] != [], True)
    # A recursion on a worker that nothing bounds fails there as on main.
    graph = Graph()
    graph.add(_fixture(THREAD_NODES + [("Walk", "void rommsync::scan::Walk(Dir)", 200)],
                       THREAD_EDGES + [("Worker", "Walk"), ("Walk", "Walk")]))
    entries, _ = thread_entries(graph)
    check("worker unbounded recursion reported",
          [p for p in measure_thread(graph, 4, entries["worker"], THREAD["worker"]).problems
           if "on the worker" in p and "does not bound" in p] != [], True)
    # Either label form names an introsort: a demangled one, and GCC's own.
    check("introsort, demangled", bool(SORT.search(
        "void std::__introsort_loop<__gnu_cxx::__normal_iterator<long*, std::vector<long> >, "
        "long, __gnu_cxx::__ops::_Iter_less_iter>(...)")), True)
    check("introsort, gcc label", bool(SORT.search(
        "void std::__introsort_loop(_RandomAccessIterator, _RandomAccessIterator, _Size, "
        "_Compare) [with _RandomAccessIterator = long*]")), True)

    # The level counts' premise, against the real heap and one too big for it.
    check("heap fits the size bounds", size_bound_problems(inner_heap(repo)), [])
    check("a 0x200000 heap outgrows SORT_LEVELS",
          [p for p in size_bound_problems(0x200000) if "SORT_LEVELS" in p] != [], True)
    check("a 0x400000 heap outgrows TREE_LEVELS",
          [p for p in size_bound_problems(0x400000) if "TREE_LEVELS" in p] != [], True)

    # `stack.fit` holds every thread to `SizedThread` by reading the tree for
    # what else starts one, comments not counted.
    check("a thread start, comments aside",
          _thread_start_lines("x.cpp", "// std::thread is not used\nstd::thread t(f);\n"
                                       "auto me = std::thread::id();\n"
                                       "  ::pthread_create(&h, nullptr, f, p);  // raw\n"
                                       "/// `threadCreate` with a static buffer\n"),
          ["x.cpp:2", "x.cpp:4"])

    # Comdat: the same inline in two units keeps the larger frame.
    graph = Graph()
    graph.add(_ci([("f", "f()", 16)], []))
    graph.add(_ci([("f", "f()", 48)], []))
    check("comdat frame", graph.frame["f"], 48)

    # The schema walk counts the way the parser does.
    schemas = {"Row": {"type": "object", "properties": {"tags": {"type": "array",
                                                                 "items": {"type": "string"}}}}}
    page = {"type": "object", "properties": {"items": {"type": "array",
                                                       "items": {"$ref": "#/c/Row"}}}}
    # page{ items[ Row{ tags[ "x" ] } ] }: four containers.
    check("schema depth", schema_depth(page, schemas, set()), 4)
    try:
        schema_depth({"$ref": "#/c/Loop"}, {"Loop": {"type": "array",
                                                     "items": {"$ref": "#/c/Loop"}}}, set())
        failures += fail("walk: a self-referential schema was given a depth")
    except ValueError:
        pass

    if failures:
        return 1
    print("ok: the walk's arithmetic holds on its fixtures")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("phase", choices=("fit", "contract", "walk", "measured"))
    parser.add_argument("--repo", default=os.path.join(os.path.dirname(__file__), ".."))
    parser.add_argument("--ci", help="measured: the directory the build's .ci files are under")
    args = parser.parse_args()
    repo = os.path.abspath(args.repo)
    if args.phase == "fit":
        return phase_fit(repo)
    if args.phase == "contract":
        return phase_contract(repo)
    if args.phase == "walk":
        return phase_walk(repo)
    if not args.ci:
        parser.error("measured needs --ci")
    return phase_measured(repo, args.ci)


if __name__ == "__main__":
    sys.exit(main())
