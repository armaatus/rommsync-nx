#!/usr/bin/env python3
"""The main thread's stack, against the deepest JSON document it can be handed (M9-18, #216).

`sysmodule/sys-rommsync.json` sizes the main thread's stack (`main_thread_stack_size`)
and `core/include/rommsync/json.hpp` sizes the one recursion in the tree
(`json::kMaxDepth`). Nothing held the two against each other, and at 0x8000 and
64 they did not fit: 64 levels of `ParseValue` -> `ParseObject` were ~31 KiB on
their own, under a `main` that had already spent ~4 KiB getting there. A
Horizon stack overflow is a data abort with no crash report, and the document
can come from the overlay over IPC or off the card at boot.

`sysmodule/source/stack_budget.hpp` now records the two measured costs as
ceilings, and this script holds every side of the arithmetic:

  fit       (every push) the npdm's `main_thread_stack_size` is the header's
            `kMainThreadStackBytes`, and `(kMaxDepth + 1) * kJsonLevelBytes +
            kMainRestBytes` fits in **half** of it. The other half is the stated
            margin, and it is for what no static walk sees (below).
  contract  (every push) the deepest typed response shape in every pinned RomM
            OpenAPI snapshot uses at most half of `kMaxDepth`, so the limit that
            keeps the stack safe is not one a real RomM response reaches.
  walk      (every push) the call-graph arithmetic below, against hand-built
            graphs whose answers are worked out in the comments beside them.
  measured  (where devkitPro is) reads the `.ci` files `-fcallgraph-info=su`
            leaves beside a cross-compile, finds the deepest chain from `main`
            and `__appInit`, and fails when the measured level or rest outgrows
            the header's ceilings -- which is how a frame that grew is caught.

How a chain is costed. Each function's frame is GCC's own static figure. Every
recursion reachable from `main` must be one `kMaxDepth` bounds -- the parser,
and `json::Value`'s destructor, whose tree is no deeper than the document that
built it -- and any other is a failure, because nothing here can bound it. A
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
virtual `SdEngine` method is treated as callable from every indirect call in
`ServiceCore`. Everything else, and every frame inside newlib and libnx, counts
as zero. The measured figure is a floor on the truth, not a bound, and half the
stack is what is left for the difference.

Re-deriving the numbers by hand is `make -C sysmodule DEFINES="-fstack-usage
-fcallgraph-info=su"` in the pinned image; `tests/test_stack_budget.sh measured`
does exactly that in a scratch copy.
"""
import argparse
import glob
import json
import os
import re
import sys

NPDM = "sysmodule/sys-rommsync.json"
BUDGET = "sysmodule/source/stack_budget.hpp"
JSON_HPP = "core/include/rommsync/json.hpp"
CONTRACTS = "server/contract/romm-openapi-*.json"

# The functions every chain starts from. `__appInit` runs on the main thread
# before `main` does, on the same stack, and returns before it starts.
ROOTS = ("main", "__appInit")

# A recursion is allowed only when every function in it belongs to the JSON
# reader, whose depth `kMaxDepth` bounds.
BOUNDED = re.compile(r"rommsync::json::")
# The other recursion the standard library brings: `std::map` and `std::set`
# copy and erase a red-black tree by recursing down one side of it. A red-black
# tree of n nodes is at most 2*log2(n+1) tall, and no tree in this process can
# reach 2^16 nodes -- every node is a heap block of at least 64 bytes, and the
# whole inner heap is 0x160000 -- so 32 levels bounds every one of them.
TREE = re.compile(r"std::_Rb_tree<")
TREE_LEVELS = 32
# The JSON reader's recursions are three and four functions. One much larger is
# not one of them, whatever it is named.
MAX_RECURSION = 8
# ...and the parser's own cycle is told apart from the destructor's by name.
PARSER = re.compile(r"rommsync::json::Parser::Parse(Value|Object|Array)\(")

# The indirect calls this walk resolves: (where the call is made, what it may
# reach, why). See the module docstring.
INDIRECT = (
    ("rommsync::ipc::ServiceCore::", re.compile(r"^virtual .*rommsync::sysmodule::SdEngine::"),
     "ipc::ServiceCore holds an ipc::Engine&, and the sysmodule's only ipc::Engine is SdEngine"),
)

NODE = re.compile(r'^node: \{ title: "(?P<title>[^"]*)" label: "(?P<label>(?:[^"\\]|\\.)*)"')
EDGE = re.compile(r'^edge: \{ sourcename: "(?P<source>[^"]*)" targetname: "(?P<target>[^"]*)"')
FRAME = re.compile(r"\\n(\d+) bytes \((?:static|dynamic)")
# `dynamic` without `,bounded` is an alloca or a VLA whose size GCC cannot
# bound, so the figure beside it is a floor and no budget can hold it.
UNBOUNDED = re.compile(r"\\n\d+ bytes \(dynamic\)")


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
    """Every `inline constexpr std::size_t kName = <number>;` in the header."""
    numbers = {}
    if not os.path.exists(os.path.join(repo, BUDGET)):
        return numbers
    for name, value in re.findall(
            r"inline\s+constexpr\s+std::size_t\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;",
            read(repo, BUDGET)):
        numbers[name] = int(value, 0)
    return numbers


def worst_case(depth, level, rest):
    return (depth + 1) * level + rest


# --- the call graph --------------------------------------------------------


class Graph:
    """What `-fcallgraph-info=su` says, over every translation unit at once.

    A global function is one node whichever unit names it; a local one is
    titled `<file>:<symbol>` by GCC already, so two units' statics cannot merge.
    A comdat inline emitted by several units is the same title in each, and
    keeps the largest frame any of them reports.
    """

    def __init__(self):
        self.frame = {}
        self.label = {}
        self.calls = {}
        self.unbounded = set()

    def add(self, text):
        for line in text.splitlines():
            node = NODE.match(line)
            if node:
                title, label = node.group("title"), node.group("label")
                self.label.setdefault(title, label.split("\\n")[0])
                frame = FRAME.search(label)
                if frame:
                    self.frame[title] = max(self.frame.get(title, 0), int(frame.group(1)))
                if UNBOUNDED.search(label):
                    self.unbounded.add(title)
                continue
            edge = EDGE.match(line)
            if edge:
                self.calls.setdefault(edge.group("source"), set()).add(edge.group("target"))

    def resolve_indirect(self):
        """Apply `INDIRECT`. Returns the problems: a rule that matched nothing
        has rotted, and would silently stop following the IPC handlers."""
        problems = []
        for caller, callee, why in INDIRECT:
            sources = [t for t, out in self.calls.items()
                       if "__indirect_call" in out and caller in self.label.get(t, "")]
            targets = [t for t, label in self.label.items() if callee.search(label)]
            if not sources or not targets:
                problems.append("the indirect-call rule `%s` -> `%s` matched %d callers and %d "
                                "callees; it exists because %s, and a rule that matches nothing "
                                "stops the walk following the main thread's IPC handlers"
                                % (caller, callee.pattern, len(sources), len(targets), why))
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
        self.tree = 0           # the costliest red-black tree recursion, whole


def measure(graph, depth):
    """The main thread's deepest chain, split into what scales with `depth` and
    what does not."""
    result = Measured()
    result.problems += graph.resolve_indirect()
    roots = [r for r in ROOTS if r in graph.calls or r in graph.frame]
    if "main" not in roots:
        result.problems.append("no `main` in the call graph; is this a sysmodule build with "
                               "-fcallgraph-info=su?")
        return result
    components = strongly_connected(graph, roots)
    reachable = {m for component in components for m in component}
    for title in sorted(graph.unbounded & reachable):
        result.problems.append("`%s` has a stack frame GCC cannot bound (an alloca or a "
                               "variable-length array) and is reachable from main; no budget "
                               "holds a frame whose size is decided at run time" % graph.name(title))
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
        if all(TREE.search(n) for n in names):
            # Bounded by the tree's height, not by any document: it adds to
            # the chain like a frame of its own rather than sharing a budget.
            level[number], own[number] = 0, TREE_LEVELS * heaviest_cycle(graph, component)
            result.tree = max(result.tree, own[number])
            continue
        if not all(BOUNDED.search(n) for n in names) or len(component) > MAX_RECURSION:
            # Not costed: there is no number to give it, and enumerating the
            # cycles of a large accidental component is exponential.
            result.problems.append("a recursion on the main thread that json::kMaxDepth does not "
                                   "bound, so no stack size is safe against it: "
                                   + " -> ".join(names[:MAX_RECURSION]))
            level[number], own[number] = 0, 0
            continue
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
                result.other_rest = max(result.other_rest, cost + (depth + 1) * key_level)
    result.level = max((level[n] for n in parser), default=0)
    if not parser:
        result.problems.append("no json::Parser recursion reachable from main; the walk is not "
                               "seeing the parser this budget is about")
    for name, other in result.other_levels.items():
        if other > result.level:
            result.problems.append("the recursion through `%s` costs %d bytes a level, more than "
                                   "the parser's %d; this budget assumes the parser's is the heaviest"
                                   % (name, other, result.level))
    return result


def report(graph, measured, depth):
    print("measured: a JSON level is %d bytes; the rest of the deepest parsing chain is %d bytes"
          % (measured.level, measured.rest))
    print("measured: at kMaxDepth %d that chain is %d bytes; the deepest chain that does not "
          "parse is %d bytes" % (depth, worst_case(depth, measured.level, measured.rest),
                                 measured.other_rest))
    if measured.tree:
        print("measured: the costliest red-black tree recursion is %d bytes at %d levels"
              % (measured.tree, TREE_LEVELS))
    for title in measured.chain:
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
    missing = [n for n in ("kMainThreadStackBytes", "kJsonLevelBytes", "kMainRestBytes")
               if n not in numbers]
    if missing:
        return fail("%s does not define %s" % (BUDGET, ", ".join(missing)))
    failures = 0
    if numbers["kMainThreadStackBytes"] != stack:
        failures += fail("%s gives the main thread %s and %s says kMainThreadStackBytes is %s; "
                         "they are one number (#216)"
                         % (NPDM, hex(stack), BUDGET, hex(numbers["kMainThreadStackBytes"])))
    worst = worst_case(depth, numbers["kJsonLevelBytes"], numbers["kMainRestBytes"])
    half = stack // 2
    if worst > half:
        failures += fail(
            "the deepest JSON document the parser accepts costs the main thread up to %d bytes "
            "-- (kMaxDepth %d + 1) x %d a level + %d beside it -- and the budget is half of its "
            "%s stack, %d bytes. The other half is for the indirect calls no walk follows, and a "
            "Horizon stack overflow is a data abort with no crash report (#216)"
            % (worst, depth, numbers["kJsonLevelBytes"], numbers["kMainRestBytes"],
               hex(stack), half))
    if failures:
        return 1
    print("ok: kMaxDepth %d costs the main thread at most %d bytes of its %s, inside the half "
          "(%d) this budget allows; %d bytes to spare" % (depth, worst, hex(stack), half,
                                                         half - worst))
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


def load_graph(directory):
    graph = Graph()
    files = sorted(glob.glob(os.path.join(directory, "*.ci")))
    for path in files:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            graph.add(handle.read())
    return graph, files


def phase_measured(repo, directory):
    graph, files = load_graph(directory)
    if not files:
        return fail("no .ci files in %s; build with DEFINES=\"-fstack-usage "
                    "-fcallgraph-info=su\"" % directory)
    depth = max_depth(repo)
    stack = npdm_stack(repo)
    if depth is None:
        return fail("no `inline constexpr int kMaxDepth = N;` in %s" % JSON_HPP)
    if stack is None:
        return fail("%s has no main_thread_stack_size" % NPDM)
    numbers = budget(repo)
    measured = measure(graph, depth)
    report(graph, measured, depth)
    failures = 0
    for problem in measured.problems:
        failures += fail(problem)
    if measured.level > numbers.get("kJsonLevelBytes", 0):
        failures += fail("a JSON nesting level now costs %d bytes and %s budgets %d for it; "
                         "raise kJsonLevelBytes and see whether `stack.fit` still passes (#216)"
                         % (measured.level, BUDGET, numbers.get("kJsonLevelBytes", 0)))
    if measured.rest > numbers.get("kMainRestBytes", 0):
        failures += fail("the main thread spends %d bytes on its deepest parsing chain beside "
                         "the levels themselves and %s budgets %d; raise kMainRestBytes and see "
                         "whether `stack.fit` still passes (#216)"
                         % (measured.rest, BUDGET, numbers.get("kMainRestBytes", 0)))
    if measured.other_rest > stack // 2:
        failures += fail("the main thread's deepest chain that parses nothing is %d bytes, more "
                         "than half its %s stack (#216)" % (measured.other_rest, hex(stack)))
    if failures:
        return 1
    print("ok: measured over %d units, within the ceilings %s records" % (len(files), BUDGET))
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
RESTORE = ("Restore", "virtual rommsync::ipc::Error rommsync::sysmodule::SdEngine::Restore(long)")


def _fixture(extra_nodes=(), extra_edges=(), destructor=50):
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
    return _ci(list(nodes) + list(extra_nodes), edges + list(extra_edges))


def phase_walk():
    failures = 0

    def check(what, observed, expected):
        nonlocal failures
        if observed != expected:
            failures += fail("walk: %s: got %r, expected %r" % (what, observed, expected))

    graph = Graph()
    graph.add(_fixture())
    measured = measure(graph, 4)
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

    # The indirect rule has to match, or the IPC handlers vanish from the walk.
    graph = Graph()
    graph.add(_fixture().replace("ServiceCore", "SomethingElse"))
    measured = measure(graph, 4)
    check("unmatched rule reported", len(measured.problems), 1)
    check("unmatched rule loses the handler", measured.other_rest, 1100)

    # A red-black tree's recursion is bounded by its height, and costs
    # TREE_LEVELS trips round its cycle wherever it is called: 1100 + 32 * 24.
    graph = Graph()
    graph.add(_fixture([("Erase", "void std::_Rb_tree<K, V>::_M_erase(_Node_ptr)", 24)],
                       [("Core", "Erase"), ("Erase", "Erase")]))
    measured = measure(graph, 4)
    check("tree problems", measured.problems, [])
    check("tree cost", measured.tree, 768)
    check("tree chain", measured.other_rest, 3100)
    graph = Graph()
    graph.add(_fixture([("Erase", "void std::_Rb_tree<K, V>::_M_erase(_Node_ptr)", 100)],
                       [("Core", "Erase"), ("Erase", "Erase")]))
    check("tree chain when it is the deepest", measure(graph, 4).other_rest, 1100 + 3200)

    # A recursion outside the JSON reader is not one any depth bounds.
    graph = Graph()
    graph.add(_fixture([("Walk", "void rommsync::scan::Walk(Dir)", 200)],
                       [("main", "Walk"), ("Walk", "Walk")]))
    measured = measure(graph, 4)
    check("unbounded recursion reported",
          [p for p in measured.problems if "does not bound" in p] != [], True)

    # A frame GCC cannot size is one no budget holds.
    graph = Graph()
    graph.add(_fixture().replace('"%s\\nfile.cpp:1:1\\n2000 bytes (static)"' % RESTORE[1],
                                 '"%s\\nfile.cpp:1:1\\n2000 bytes (dynamic)"' % RESTORE[1]))
    check("unbounded frame reported",
          [p for p in measure(graph, 4).problems if "cannot bound" in p] != [], True)
    graph = Graph()
    graph.add(_fixture().replace('"%s\\nfile.cpp:1:1\\n2000 bytes (static)"' % RESTORE[1],
                                 '"%s\\nfile.cpp:1:1\\n2000 bytes (dynamic,bounded)"'
                                 % RESTORE[1]))
    check("bounded dynamic frame counted", measure(graph, 4).problems, [])

    # A destructor heavier than the parser breaks the shared-budget argument.
    graph = Graph()
    graph.add(_fixture(destructor=900))
    measured = measure(graph, 4)
    check("heavy destructor reported",
          [p for p in measured.problems if "heaviest" in p] != [], True)

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
    parser.add_argument("--ci", help="measured: the directory holding the build's .ci files")
    args = parser.parse_args()
    repo = os.path.abspath(args.repo)
    if args.phase == "fit":
        return phase_fit(repo)
    if args.phase == "contract":
        return phase_contract(repo)
    if args.phase == "walk":
        return phase_walk()
    if not args.ci:
        parser.error("measured needs --ci")
    return phase_measured(repo, args.ci)


if __name__ == "__main__":
    sys.exit(main())
