#!/usr/bin/env python3
"""CeeSafe's specification against the compiler that embeds it (spec/testing.md §4, Decision 0006).

- explain: every diagnostic code in the catalog (src/diagnostics/catalog.cee) has an entry `ceesafec explain` finds
  , and the embedded documents are the tree's (src/library/documents.cee is up to date);
- links: every relative link in spec/, guide/ and templates/ names a file that exists;
- api: spec/api/core.md and spec/api/std.md are what `ceesafec doc lib/core --title "lib/core"` and
  `ceesafec doc lib/std --title "lib/std"` write for the sources tools/gen_library.py embeds (Decision 0008);
- names: no tracked file names the retired predecessor language or its compiler, in any case or as a C prefix, but
  COPYING.RUNTIME, whose notice says where CeeSafe came from (Decision 0010). The word is spelled in two pieces here
  so that this file passes its own check;
- fixes: every diagnostic code the compiler's sources give a fix (errorWithFix, errorWithEdit, errorWithTwoEdits) has
  a row in tooling.md §5.2's table;
- agents: AGENTS.md's section between the markers is templates/AGENTS.md, what `ceesafec agents` writes.

usage: python3 tools/spec_check.py --ceesafec out/ceesafec
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

# The predecessor's name, alone or starting a word, in any case (its compiler's, its C prefixes, its file extension,
# names built on it), after a C escape ("\\n") too; capitalised or in capitals, anywhere (camelCase). Not inside a
# lower-case word ("innovation").
_WORD = "no" + "va"
RETIRED = re.compile(r"(?i:(?:(?<![A-Za-z])|(?<=\\[nrt]))" + _WORD + r")|" + _WORD.capitalize() + "|" + _WORD.upper())
# The one file that keeps the name: the licence notice (Decision 0010).
KEEPS_NAME = ("COPYING.RUNTIME",)


def documents():
    found = []
    for top in ("spec", "guide", "templates"):
        found += sorted(glob.glob(os.path.join(ROOT, top, "**", "*.md"), recursive=True))
    return found


def check_explain(cee, problems):
    with open(os.path.join(ROOT, "src", "diagnostics", "catalog.cee"), encoding="utf-8") as f:
        codes = sorted({int(n) for n in re.findall(r"^\s+(\d+) =>", f.read(), re.M)})
    missing = [c for c in codes
               if subprocess.run([cee, "explain", f"N{c:04d}"], capture_output=True).returncode != 0]
    for c in missing:
        problems.append(f"explain: N{c:04d} has no entry in the specification")
    # A bar inside a table cell is written `\|`: `explain` prints the whole cell, with a plain bar.
    shown = subprocess.run([cee, "explain", "N0356"], capture_output=True, text=True).stdout
    if "`Some(x) | null =>`" not in shown:
        problems.append(f"explain: N0356's example is cut at its escaped bar: {shown.strip()[-60:]!r}")
    with tempfile.TemporaryDirectory() as work:
        fresh = os.path.join(work, "documents.cee")
        subprocess.run([sys.executable, os.path.join(HERE, "gen_docs.py"), "-o", fresh], check=True,
                       env={**os.environ, "CEESAFEC": cee})
        with open(fresh, "rb") as a, open(os.path.join(ROOT, "src", "library", "documents.cee"), "rb") as b:
            if a.read() != b.read():
                problems.append("explain: src/library/documents.cee is not the tree's documents; run tools/gen_docs.py "
                                "and rebuild")
        # The runtime ceesafec embeds: an Android Java change rewrites the window runtime's dex, and runtime.cee made
        # before it builds apps with the old classes (seen: Photos.videoFrame missing, ADR 0273).
        runtime = os.path.join(work, "runtime.cee")
        subprocess.run([sys.executable, os.path.join(HERE, "gen_runtime.py"), "-o", runtime], check=True,
                       capture_output=True, env={**os.environ, "CEESAFEC": cee})
        with open(runtime, "rb") as a, open(os.path.join(ROOT, "src", "library", "runtime.cee"), "rb") as b:
            if a.read() != b.read():
                problems.append("runtime: src/library/runtime.cee is not the tree's runtime/; run tools/gen_runtime.py "
                                "and rebuild")
    return len(codes)


def check_links(problems):
    count = 0
    for path in documents():
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for m in re.finditer(r"\]\(([^)\s]+)\)", text):
            target = m.group(1)
            if re.match(r"[a-z]+:", target) or target.startswith("#"):
                continue
            count += 1
            if not os.path.exists(os.path.normpath(os.path.join(os.path.dirname(path), target.split("#")[0]))):
                problems.append(f"links: {os.path.relpath(path, ROOT)} links to {target}, which does not exist")
    return count


def check_api(cee, problems):
    """spec/api/core.md and spec/api/std.md against `ceesafec doc` of the libraries' sources."""
    for library in ("core", "std"):
        with tempfile.TemporaryDirectory() as work:
            for name in ("core", "std"):  # lib/std's items name lib/core's, so both are present
                directory = os.path.join(work, "lib", name)
                os.makedirs(directory)
                for source in sorted(glob.glob(os.path.join(ROOT, "lib", name, "*.cee"))):
                    with open(source, "rb") as f, open(os.path.join(directory, os.path.basename(source)), "wb") as g:
                        g.write(f.read())
            page = subprocess.run([cee, "doc", f"lib/{library}", "--title", f"lib/{library}"], cwd=work,
                                  capture_output=True)
        with open(os.path.join(ROOT, "spec", "api", f"{library}.md"), "rb") as f:
            if page.returncode != 0 or page.stdout != f.read():
                problems.append(f"api: spec/api/{library}.md is not what `ceesafec doc lib/{library}` writes")


def check_names(problems):
    """Every tracked file's path and text but KEEPS_NAME's."""
    count = 0
    tracked = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
    for relative in tracked.decode("utf-8").split("\0"):
        if not relative or relative in KEEPS_NAME:
            continue
        if RETIRED.search(relative):
            count += 1
            problems.append(f"names: {relative}: the path")
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue  # binary: an image
        for number, line in enumerate(text.splitlines(), 1):
            if RETIRED.search(line):
                count += 1
                problems.append(f"names: {relative}:{number}: {line.strip()[:120]}")
    return count


def check_fixes(problems):
    """A fix lands with its row in tooling.md §5.2 (spec before code): each code named at a fix site is in the table."""
    codes = {}
    sites = []
    call = re.compile(r"\berrorWith(?:Fix|Edit|TwoEdits)\(\s*([A-Za-z_][A-Za-z0-9_]*)\(\)")
    code = re.compile(r"^fn ([A-Za-z_][A-Za-z0-9_]*)\(\) -> u32 => (\d+);", re.M)
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "**", "*.cee"), recursive=True)):
        if os.sep + "library" + os.sep in path:
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for name, number in code.findall(text):
            codes.setdefault(name, int(number))
        for m in call.finditer(text):
            sites.append((os.path.relpath(path, ROOT), text.count("\n", 0, m.start()) + 1, m.group(1)))
    with open(os.path.join(ROOT, "spec", "tooling.md"), encoding="utf-8") as f:
        spec = f.read()
    start = spec.index("### 5.2 Fixes")
    table = set(re.findall(r"^\| N(\d{4}) \|", spec[start:spec.index("### 5.3", start)], re.M))
    for path, line, name in sites:
        if name not in codes:
            problems.append(f"fixes: {path}:{line}: no code found for '{name}()'")
        elif f"{codes[name]:04d}" not in table:
            problems.append(f"fixes: {path}:{line}: N{codes[name]:04d} offers a fix that tooling.md §5.2 does not list")
    return len(sites)


def check_agents(problems):
    """The repository's own AGENTS.md carries the template's section, as `ceesafec agents` would write it now."""
    with open(os.path.join(ROOT, "templates", "AGENTS.md"), encoding="utf-8") as f:
        template = f.read()
    with open(os.path.join(ROOT, "AGENTS.md"), encoding="utf-8") as f:
        own = f.read()
    start, end = "<!--CEESAFE START-->", "<!--CEESAFE END-->"
    if start not in own or end not in own:
        problems.append("agents: AGENTS.md has no section between the markers")
        return
    section = own[own.index(start):own.index(end) + len(end)]
    if section.strip() != template.strip():
        problems.append("agents: AGENTS.md's section is not templates/AGENTS.md; run `out/ceesafec agents`")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ceesafec", required=True)
    args = parser.parse_args()
    cee = os.path.abspath(args.ceesafec)
    problems = []
    codes = check_explain(cee, problems)
    links = check_links(problems)
    check_api(cee, problems)
    check_names(problems)
    check_fixes(problems)
    check_agents(problems)
    for p in problems:
        print("FAIL", p)
    print(f"spec_check: {len(documents())} documents, {codes} codes explained, {links} links, the API page, names: "
          f"{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
