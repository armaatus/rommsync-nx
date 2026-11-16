#!/usr/bin/env python3
"""The project's front door -- SECURITY.md, SUPPORT.md, CONTRIBUTING.md and the
issue forms -- held to the product that ships.

These pages were written before there was a client, and said so: "not yet
implemented", "nothing to install", "no released binaries". Nothing broke when
the client landed, so nothing told them; a reporter read them for months after
they stopped being true (#302). And the bug form asked for a RomM version and a
pasted log, while the first questions a console bug gets are the firmware, the
Atmosphere build, the loader and the model.

Four phases, one CTest entry each so a red run names what broke:

  stale       no user-facing file says the client does not exist, that there
              is nothing to install, or that the client only speaks HTTPS --
              plain `http://` is supported (docs/INSTALL.md).
  forms       every issue form under .github/ISSUE_TEMPLATE/ parses and keeps
              to GitHub's issue-form schema, so it renders rather than falling
              back to a blank issue.
  bug_fields  the bug form requires what triage needs (#302's list), names the
              log files where log.hpp puts them, and no longer tells a reporter
              to redact a log the redactor has already scrubbed.
  versions    SECURITY.md's supported-versions table names the series VERSION
              is in.

PyYAML is not something CI's interpreter is promised to have, so the forms are
read by the small strict reader below. It understands the subset issue forms
use and refuses anything else, which is the right failure: a form this reader
cannot read is one worth a second look before GitHub gets to decide.
"""
import argparse
import json
import os
import re
import sys

# Every page a user or a reporter is sent to. Docs written for contributors
# about the tree's history may say a thing "was not implemented" -- these may not.
USER_FACING = [
    "README.md",
    "SECURITY.md",
    "SUPPORT.md",
    "CONTRIBUTING.md",
    "docs/INSTALL.md",
    "docs/TROUBLESHOOTING.md",
    "docs/SECURITY.md",
    "docs/INTEROP.md",
    "packaging/README.txt.in",
]

# What the pre-client pages said, as patterns rather than exact sentences so a
# rewording of the same claim is caught too.
STALE = [
    (r"not\s+(?:yet\s+)?implemented(?:\s+yet)?", "says something is not implemented"),
    (r"isn['’]t\s+(?:yet\s+)?implemented", "says something is not implemented"),
    (r"nothing\s+to\s+install", "says there is nothing to install"),
    (r"no\s+released\s+binaries", "says there are no released binaries"),
    (r"only\s+calls?\s+\**out\**\s+over\s+HTTPS",
     "says the client only speaks HTTPS; plain http:// is supported"),
    (r"start\s+at\s+(?:milestone\s+)?\**M0\b", "sends contributors to M0, long closed"),
]


def fail(msg: str) -> int:
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def template_dir(repo: str) -> str:
    return os.path.join(repo, ".github", "ISSUE_TEMPLATE")


def user_facing(repo: str) -> list:
    paths = [p for p in USER_FACING if os.path.exists(os.path.join(repo, p))]
    for name in sorted(os.listdir(template_dir(repo))):
        paths.append(os.path.join(".github", "ISSUE_TEMPLATE", name))
    return paths


# --- stale ----------------------------------------------------------------------

def phase_stale(args) -> int:
    bad = []
    for rel in user_facing(args.repo):
        for n, line in enumerate(read(os.path.join(args.repo, rel)).splitlines(), 1):
            for pattern, why in STALE:
                if re.search(pattern, line, re.IGNORECASE):
                    bad.append(f"{rel}:{n}: {why}: {line.strip()}")
    for line in bad:
        print(f"FAIL: {line}", file=sys.stderr)
    if bad:
        return 1
    print(f"no stale claim in {len(user_facing(args.repo))} user-facing files")
    return 0


# --- a strict reader for the YAML subset issue forms use ----------------------

class YamlError(Exception):
    pass


KEY = re.compile(r"^([A-Za-z_][\w-]*):(?:[ ](.*))?$")


def scalar(text: str, where: str):
    text = text.strip()
    if text.startswith('"'):
        if not text.endswith('"') or len(text) < 2:
            raise YamlError(f"{where}: unterminated double-quoted string")
        try:
            return json.loads(text)
        except json.JSONDecodeError as e:
            raise YamlError(f"{where}: {e}")
    if text.startswith("'"):
        if not text.endswith("'") or len(text) < 2:
            raise YamlError(f"{where}: unterminated single-quoted string")
        return text[1:-1].replace("''", "'")
    if text.startswith("["):
        if not text.endswith("]"):
            raise YamlError(f"{where}: unterminated flow sequence")
        inner = text[1:-1].strip()
        if not inner:
            return []
        items = re.findall(r'\s*("(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\'|[^,]+)\s*(?:,|$)', inner)
        return [scalar(i, where) for i in items]
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if text and text[0] in "{&*!%@`|>":
        raise YamlError(f"{where}: plain scalar starts with {text[0]!r}; quote it")
    if ": " in text or " #" in text:
        raise YamlError(f"{where}: plain scalar holds ': ' or ' #'; quote it")
    return text


def parse_yaml(source: str, name: str):
    lines = []
    for n, raw in enumerate(source.split("\n"), 1):
        if "\t" in raw[: len(raw) - len(raw.lstrip())]:
            raise YamlError(f"{name}:{n}: tab in indentation")
        lines.append((n, len(raw) - len(raw.lstrip(" ")), raw.strip(), raw))

    def skip(i):
        while i < len(lines) and (not lines[i][2] or lines[i][2].startswith("#")):
            i += 1
        return i

    def block(i, indent):
        i = skip(i)
        n, ind, text, _ = lines[i]
        if ind != indent:
            raise YamlError(f"{name}:{n}: expected indent {indent}, got {ind}")
        if text == "-" or text.startswith("- "):
            return sequence(i, indent)
        return mapping(i, indent)

    def literal(i, parent):
        out, base = [], None
        while i < len(lines):
            n, ind, text, raw = lines[i]
            if text and ind <= parent:
                break
            if text and base is None:
                base = ind
            out.append(raw[base:] if text else "")
            i += 1
        while out and not out[-1]:
            out.pop()
        if base is None:
            raise YamlError(f"{name}: empty literal block")
        return "\n".join(out) + "\n", i

    def value_after(i, indent, rest, where):
        """The value of a key whose text after the colon is `rest`."""
        if rest is None or rest == "":
            j = skip(i + 1)
            if j >= len(lines) or lines[j][1] <= indent:
                raise YamlError(f"{where}: key with no value")
            return block(j, lines[j][1])
        if rest == "|":
            return literal(i + 1, indent)
        if rest.startswith(("|", ">")):
            raise YamlError(f"{where}: only plain '|' block scalars are understood")
        return scalar(rest, where), i + 1

    def mapping(i, indent, first=None):
        out = {}
        while True:
            # A sequence item's first key sits on the `- ` line, two columns
            # left of the indent the rest of its keys share.
            if first is None:
                i = skip(i)
                if i >= len(lines) or lines[i][1] < indent:
                    return out, i
            n, ind, text, _ = lines[i]
            if first is not None:
                text, first = first, None
            elif ind != indent:
                raise YamlError(f"{name}:{n}: unexpected indent {ind}")
            elif text.startswith("- "):
                return out, i
            m = KEY.match(text)
            if not m:
                raise YamlError(f"{name}:{n}: not a `key: value` line: {text!r}")
            key = m.group(1)
            if key in out:
                raise YamlError(f"{name}:{n}: duplicate key {key!r}")
            out[key], i = value_after(i, indent, m.group(2), f"{name}:{n}")

    def sequence(i, indent):
        out = []
        while True:
            i = skip(i)
            if i >= len(lines) or lines[i][1] != indent:
                if i < len(lines) and lines[i][1] > indent:
                    raise YamlError(f"{name}:{lines[i][0]}: unexpected indent")
                return out, i
            n, _, text, _ = lines[i]
            if not text.startswith("- "):
                return out, i
            content = text[2:].strip()
            if KEY.match(content):
                item, i = mapping(i, indent + 2, first=content)
            else:
                item, i = scalar(content, f"{name}:{n}"), i + 1
            out.append(item)

    i = skip(0)
    if i >= len(lines):
        raise YamlError(f"{name}: empty document")
    value, i = block(i, 0)
    if skip(i) < len(lines):
        raise YamlError(f"{name}:{lines[skip(i)][0]}: trailing content")
    return value


# --- forms ----------------------------------------------------------------------

# docs.github.com, "Syntax for issue forms" and "Syntax for GitHub's form schema".
TOP_REQUIRED = {"name", "description", "body"}
TOP_ALLOWED = TOP_REQUIRED | {"title", "labels", "assignees", "projects", "type"}
ATTRIBUTES = {
    "markdown": {"value"},
    "textarea": {"label", "description", "placeholder", "value", "render"},
    "input": {"label", "description", "placeholder", "value"},
    "dropdown": {"label", "description", "multiple", "options", "default"},
    "checkboxes": {"label", "description", "options"},
}
ID = re.compile(r"^[A-Za-z0-9_-]+$")


def form_problems(form, name: str) -> list:
    if not isinstance(form, dict):
        return [f"{name}: top level is not a mapping"]
    bad = [f"{name}: missing `{k}`" for k in sorted(TOP_REQUIRED - form.keys())]
    bad += [f"{name}: unknown top-level key `{k}`" for k in sorted(form.keys() - TOP_ALLOWED)]
    for k in ("name", "description"):
        if k in form and (not isinstance(form[k], str) or not form[k].strip()):
            bad.append(f"{name}: `{k}` must be a non-empty string")
    body = form.get("body")
    if not isinstance(body, list) or not body:
        return bad + [f"{name}: `body` must be a non-empty list"]
    ids, labels, inputs = set(), set(), 0
    for idx, el in enumerate(body):
        where = f"{name}: body[{idx}]"
        if not isinstance(el, dict):
            bad.append(f"{where}: not a mapping")
            continue
        kind = el.get("type")
        if kind not in ATTRIBUTES:
            bad.append(f"{where}: unknown type {kind!r}")
            continue
        extra = el.keys() - {"type", "id", "attributes", "validations"}
        bad += [f"{where}: unknown key `{k}`" for k in sorted(extra)]
        attrs = el.get("attributes")
        if not isinstance(attrs, dict):
            bad.append(f"{where}: `attributes` missing")
            continue
        bad += [f"{where}: unknown attribute `{k}` for {kind}"
                for k in sorted(attrs.keys() - ATTRIBUTES[kind])]
        if "id" in el:
            if not isinstance(el["id"], str) or not ID.match(el["id"]):
                bad.append(f"{where}: id {el['id']!r} is not [A-Za-z0-9_-]+")
            elif el["id"] in ids:
                bad.append(f"{where}: duplicate id {el['id']!r}")
            ids.add(el.get("id"))
        if kind == "markdown":
            if not isinstance(attrs.get("value"), str) or not attrs["value"].strip():
                bad.append(f"{where}: markdown needs a non-empty `value`")
            if "validations" in el:
                bad.append(f"{where}: markdown takes no validations")
            continue
        inputs += 1
        label = attrs.get("label")
        if not isinstance(label, str) or not label.strip():
            bad.append(f"{where}: needs a non-empty `label`")
        elif label in labels:
            bad.append(f"{where}: duplicate label {label!r}")
        labels.add(label)
        val = el.get("validations")
        if val is not None and (not isinstance(val, dict) or set(val) - {"required"}
                                or not isinstance(val.get("required", False), bool)):
            bad.append(f"{where}: `validations` may only hold a boolean `required`")
        if kind == "dropdown":
            opts = attrs.get("options")
            if (not isinstance(opts, list) or not opts
                    or not all(isinstance(o, str) and o.strip() for o in opts)):
                bad.append(f"{where}: dropdown needs a list of non-empty string options")
            elif len(set(opts)) != len(opts) or "None" in opts:
                bad.append(f"{where}: dropdown options must be unique and not 'None'")
            elif "default" in attrs and not (isinstance(attrs["default"], int)
                                             and 0 <= attrs["default"] < len(opts)):
                bad.append(f"{where}: dropdown `default` is not an option index")
        if kind == "checkboxes":
            opts = attrs.get("options")
            if not isinstance(opts, list) or not opts:
                bad.append(f"{where}: checkboxes need options")
            else:
                for o in opts:
                    if (not isinstance(o, dict) or not isinstance(o.get("label"), str)
                            or set(o) - {"label", "required"}
                            or not isinstance(o.get("required", False), bool)):
                        bad.append(f"{where}: checkbox option {o!r} is not "
                                   "{label, required?}")
    if not inputs:
        bad.append(f"{name}: a form needs at least one field that is not markdown")
    return bad


def forms(repo: str) -> dict:
    out = {}
    for name in sorted(os.listdir(template_dir(repo))):
        if name.endswith((".yml", ".yaml")) and name not in ("config.yml", "config.yaml"):
            out[name] = parse_yaml(read(os.path.join(template_dir(repo), name)), name)
    return out


def phase_forms(args) -> int:
    try:
        parsed = forms(args.repo)
    except YamlError as e:
        return fail(str(e))
    if "bug_report.yml" not in parsed:
        return fail("no .github/ISSUE_TEMPLATE/bug_report.yml")
    bad = []
    for name, form in parsed.items():
        bad += form_problems(form, name)
    for line in bad:
        print(f"FAIL: {line}", file=sys.stderr)
    if bad:
        return 1
    print(f"{len(parsed)} issue forms keep to the form schema: " + ", ".join(parsed))
    return 0


# --- bug_fields -----------------------------------------------------------------

# #302's list, by the id each field carries. The words are what a reporter
# would be asked in a support thread if the form had not asked first.
REQUIRED_FIELDS = {
    "version": ("rommsync-nx version", ["status screen"]),
    "atmosphere": ("Atmosphère version", []),
    "firmware": ("firmware", []),
    "loader": ("Ultrahand and nx-ovlloader versions", ["Ultrahand", "nx-ovlloader"]),
    "mmc": ("emuMMC or sysMMC", []),
    "model": ("console model", []),
    "romm-version": ("RomM version", []),
    "sysmodules": ("other sysmodules", []),
    "logs": ("the log files", []),
    "crash-reports": ("crash reports", ["crash_reports"]),
}
OPTIONS = {
    "mmc": ["emuMMC", "sysMMC"],
    "model": ["Erista", "Mariko", "Lite", "OLED"],
}


def text_of(el) -> str:
    a = el.get("attributes", {})
    return " ".join(str(a.get(k, "")) for k in ("label", "description", "placeholder", "value"))


def phase_bug_fields(args) -> int:
    try:
        form = forms(args.repo)["bug_report.yml"]
    except (YamlError, KeyError) as e:
        return fail(f"cannot read bug_report.yml: {e}")
    by_id = {el.get("id"): el for el in form.get("body", []) if isinstance(el, dict)}
    bad = []
    for fid, (what, words) in REQUIRED_FIELDS.items():
        el = by_id.get(fid)
        if el is None:
            bad.append(f"no field with id `{fid}` ({what})")
            continue
        if not (el.get("validations") or {}).get("required"):
            bad.append(f"`{fid}` ({what}) is not required")
        for w in words:
            if w.lower() not in text_of(el).lower():
                bad.append(f"`{fid}` ({what}) does not mention {w!r}")
    for fid, wanted in OPTIONS.items():
        el = by_id.get(fid)
        if el is None:
            continue
        if el.get("type") != "dropdown":
            bad.append(f"`{fid}` is not a dropdown")
            continue
        opts = " ".join(el["attributes"].get("options", []))
        bad += [f"`{fid}` has no {w} option" for w in wanted if w not in opts]

    header = read(os.path.join(args.repo, "core", "include", "rommsync", "log.hpp"))
    m = re.search(r'kLogSdPath = "([^"]+)"', header)
    if not m:
        return fail("cannot find `kLogSdPath` in core/include/rommsync/log.hpp")
    logs = text_of(by_id.get("logs", {}))
    for path in (m.group(1), m.group(1) + ".old"):
        if path not in logs:
            bad.append(f"the logs field does not name `{path}` (log.hpp's kLogSdPath)")
    # The log is redacted when it is written (docs/TROUBLESHOOTING.md, "Reading
    # the log"), and the guide says to paste it as it is. A form saying the
    # opposite is how the line that explains a failure gets edited away.
    if re.search(r"redact\s+tokens", logs, re.IGNORECASE):
        bad.append("the logs field still says to redact tokens; the log is scrubbed already")
    if "TROUBLESHOOTING.md" not in logs:
        bad.append("the logs field does not point at docs/TROUBLESHOOTING.md")
    if not re.search(r"scrubbed|redacted", logs, re.IGNORECASE):
        bad.append("the logs field does not say the log is already scrubbed")
    for b in bad:
        print(f"FAIL: {b}", file=sys.stderr)
    if bad:
        return 1
    print(f"the bug form requires {len(REQUIRED_FIELDS)} triage fields and names "
          f"{m.group(1)} and its .old")
    return 0


# --- versions -------------------------------------------------------------------

def phase_versions(args) -> int:
    version = read(os.path.join(args.repo, "VERSION")).strip()
    m = re.match(r"(\d+)\.(\d+)\.", version)
    if not m:
        return fail(f"VERSION {version!r} is not MAJOR.MINOR.PATCH")
    series = f"{m.group(1)}.{m.group(2)}.x"
    rows = [line for line in read(os.path.join(args.repo, "SECURITY.md")).splitlines()
            if line.startswith("|")]
    if not any(f"`{series}`" in r for r in rows):
        return fail(f"SECURITY.md's supported-versions table has no `{series}` row "
                    f"(VERSION is {version})")
    print(f"SECURITY.md supports `{series}`, the series VERSION {version} is in")
    return 0


PHASES = {
    "stale": phase_stale,
    "forms": phase_forms,
    "bug_fields": phase_bug_fields,
    "versions": phase_versions,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=sorted(PHASES))
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()
    return PHASES[args.phase](args)


if __name__ == "__main__":
    sys.exit(main())
