#!/usr/bin/env python3
"""The book says what the compiler does, and its pages have their shape (Decision 0011 §6, book/WRITING.md).

For every written page of book/src (a page named in book/toc.mjs whose file exists):
- its header has a title and a one-sentence summary with no ": " in it; a section (not a chapter's opening page, the
  title page or the introduction) says what it teaches and ends with a Summary;
- listings are numbered `Listing <chapter>-<n>` in order through the chapter, each once;
- a diagnostic the header names is one the page shows (in a listing that fails with it, or in a compiler message
  it prints), and a listing that fails with a code has it named in the header;
- a link to another page of the book names a written page;
- a listing marked `skip` is a file under book/projects/, or a run of lines of one, byte for byte.
Then every `ceesafe` block is compiled and run as the guide's are (tools/guide_check.py, on a copy of the pages), and
every package under book/projects/ is checked, and tested if it has tests. A package that is there to show a failure
says so in a file `.book-expect` beside its ceesafe.toml: `check fails N0312 N0201` (the check must fail, with those
codes) or `test fails` (the check passes and `ceesafec test` must fail).

usage: python3 tools/book_check.py --ceesafec out/ceesafec [--jobs N]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BOOK = os.path.join(ROOT, "book")
SOURCE = os.path.join(BOOK, "src")
PROJECTS = os.path.join(BOOK, "projects")

CAPTION = re.compile(
    r'<span class="listing-caption"><strong>Listing ([0-9]+)-([0-9]+)</strong>(?:, <code>([^<]+)</code>)?: .*?</span>\s*\n+'
    r"(?:<!-- ceesafe: ([^>]*?) -->\n)?```[a-z]*\n(.*?)\n```(?:\s*\n+```text\n(.*?)\n```)?",
    re.S,
)
CODE = re.compile(r"\bN[0-9]{4}\b")


def written_pages():
    """The written pages in reading order: (file, number), from book/toc.mjs, which node evaluates."""
    script = (
        "import('./toc.mjs').then(t => console.log(JSON.stringify(t.writtenPages().map(p => [p.file, p.number]))))"
    )
    result = subprocess.run(["node", "-e", script], cwd=BOOK, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"book_check: book/toc.mjs: {result.stderr.strip()}")
    return json.loads(result.stdout)


def header_of(text):
    match = re.match(r"---\n(.*?)\n---\n", text, re.S)
    if not match:
        return None, text
    header = {}
    for line in match.group(1).split("\n"):
        entry = re.match(r"([a-z]+):\s*(.*)$", line)
        if entry:
            value = entry.group(2).strip()
            if value.startswith("["):
                value = [item.strip().strip('"') for item in value[1:-1].split(",") if item.strip()]
            header[entry.group(1)] = value
    return header, text[match.end():]


def project_files():
    files = []
    for directory, names, leaves in os.walk(PROJECTS):
        names[:] = [n for n in names if n != "out"]
        for leaf in leaves:
            with open(os.path.join(directory, leaf), "rb") as f:
                files.append(f.read())
    return files


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ceesafec", required=True)
    parser.add_argument("--jobs", type=int, default=0)
    args = parser.parse_args()
    cee = os.path.abspath(args.ceesafec)
    problems = []
    pages = written_pages()
    ids = {file for file, _ in pages}
    copies = project_files() if os.path.isdir(PROJECTS) else []
    last = {}  # chapter → the last listing number seen
    listings = 0
    for file, number in pages:
        path = os.path.join(SOURCE, file + ".md")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        header, body = header_of(text)
        where = f"book/src/{file}.md"
        if header is None:
            problems.append(f"{where}: no header")
            continue
        for key in ("title", "summary"):
            if not header.get(key):
                problems.append(f"{where}: its header has no '{key}'")
        if ": " in str(header.get("summary", "")):
            problems.append(f"{where}: its summary has ': ' in it (the header's parser stops there)")
        section = "." in number
        if section:
            if "**This section teaches**" not in body:
                problems.append(f"{where}: does not say what it teaches ('**This section teaches**')")
            if not re.search(r"^## Summary\s*$", body, re.M):
                problems.append(f"{where}: has no '## Summary'")
        failing = set()
        shown = set(re.findall(r"(?:error|warning)\[(N[0-9]{4})\]", body))
        for match in CAPTION.finditer(body):
            chapter, n, name, directive, code, result = match.groups()
            listings += 1
            if number and chapter != number.split(".")[0]:
                problems.append(f"{where}: Listing {chapter}-{n} is not of chapter {number.split('.')[0]}")
            if int(n) != last.get(chapter, 0) + 1:
                problems.append(f"{where}: Listing {chapter}-{n} follows Listing {chapter}-{last.get(chapter, 0)}")
            last[chapter] = int(n)
            directive = (directive or "").strip()
            if directive.startswith(("fail", "warns")):
                failing.update(CODE.findall(directive))
            if directive == "skip" and not any(code.encode() in copy for copy in copies):
                problems.append(f"{where}: Listing {chapter}-{n} ({name}) is marked skip and is in no file of book/projects/")
        captions = len(re.findall(r'class="listing-caption"', body))
        if captions != len(list(CAPTION.finditer(body))):
            problems.append(f"{where}: a listing's caption is not followed by its ceesafe block in the form of book/WRITING.md")
        named = set(header.get("diagnostics") or [])
        for code in sorted(named - shown - failing):
            problems.append(f"{where}: its header names {code}, which the page shows in no listing or compiler message")
        for code in sorted(failing - named):
            problems.append(f"{where}: a listing fails with {code}, which its header does not name")
        for target in re.findall(r"\]\(\./([A-Za-z0-9_-]+)(?:#[^)]*)?\)", body):
            if target not in ids:
                problems.append(f"{where}: links to './{target}', which is not a written page")
    # Every block, compiled and run: on a copy, so that the generated src/public/ is not checked twice.
    with tempfile.TemporaryDirectory() as work:
        for file, _ in pages:
            shutil.copy(os.path.join(SOURCE, file + ".md"), work)
        command = [sys.executable, os.path.join(ROOT, "tools", "guide_check.py"), "--ceesafec", cee, "--guide", work]
        if args.jobs:
            command += ["--jobs", str(args.jobs)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        lines = (result.stdout + result.stderr).strip().split("\n")
        if result.returncode != 0:
            problems.extend(line.replace(work, "book/src") for line in lines[:-1] if line.strip())
        examples = lines[-1]
    # The packages of the projects.
    packages = 0
    if os.path.isdir(PROJECTS):
        for directory, names, leaves in sorted(os.walk(PROJECTS)):
            names[:] = sorted(n for n in names if n != "out")
            if "ceesafe.toml" not in leaves:
                continue
            packages += 1
            where = os.path.relpath(directory, ROOT)
            expect = ""
            if ".book-expect" in leaves:
                with open(os.path.join(directory, ".book-expect"), encoding="utf-8") as f:
                    expect = f.read().strip()
            check = subprocess.run([cee, "check", "--offline"], cwd=directory, capture_output=True, text=True)
            said = check.stdout + check.stderr
            if expect.startswith("check fails"):
                if check.returncode == 0:
                    problems.append(f"{where}: ceesafec check passes, and .book-expect says it fails")
                for code in CODE.findall(expect):
                    if f"[{code}]" not in said:
                        problems.append(f"{where}: ceesafec check does not report {code}, which .book-expect names")
                continue
            if check.returncode != 0:
                problems.append(f"{where}: ceesafec check fails: {said.strip().splitlines()[0]}")
                continue
            has_tests = False
            for sub, _, files in os.walk(directory):
                for leaf in files:
                    if leaf.endswith(".cee"):
                        with open(os.path.join(sub, leaf), encoding="utf-8") as f:
                            has_tests = has_tests or "@test" in f.read()
            if has_tests:
                test = subprocess.run([cee, "test", "--interp", "--offline", "--timeout", "5"], cwd=directory,
                                      capture_output=True, text=True)
                tail = (test.stdout + test.stderr).strip().splitlines()
                if expect == "test fails":
                    if test.returncode == 0:
                        problems.append(f"{where}: ceesafec test passes, and .book-expect says it fails")
                elif test.returncode != 0:
                    problems.append(f"{where}: ceesafec test fails: {tail[-1] if tail else ''}")
    for problem in problems:
        print("FAIL " + problem)
    print(f"book_check: {len(pages)} pages, {listings} listings, {packages} packages; {examples}; {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
