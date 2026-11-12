#!/usr/bin/env python3
"""Every example of the guide does what its page says (spec/testing.md §4, Decision 0006).

A `ceesafe` code block in guide/ is a whole file, compiled as `main.cee`. It must type-check; with a `main`, it is also run
in the interpreter (no arguments, empty standard input), and a `text` block right after it is its exact output. An HTML
comment directly before the block changes that:
- `<!-- ceesafe: fail N0241 N0205 -->`: the block fails with exactly these codes, and a `text` block after it is exactly
  the compiler's messages;
- `<!-- ceesafe: warns N0400 -->`: as `fail`, but the codes are warnings: the check still succeeds (ADR 0073 §2.4);
- `<!-- ceesafe: freestanding -->`: checked with the freestanding profile, not run; it combines with `fail` and
  `warns`, as in `<!-- ceesafe: fail N0293 freestanding -->`;
- `<!-- ceesafe: check -->`: type-checked, not run (it calls C);
- `<!-- ceesafe: panics -->`, `<!-- ceesafe: exits N -->`: the run's exit status (134 for a panic);
- `<!-- ceesafe: skip -->`: not checked.

usage: python3 tools/guide_check.py --ceesafec out/ceesafec [--guide guide] [--jobs N]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIRECTIVE = re.compile(r"^<!--\s*ceesafe:\s*(.*?)\s*-->$")
HEADER = re.compile(r"^main\.cee:\d+:\d+: (error|warning)\[")


def examples(guide):
    found = []
    for directory, _, names in sorted(os.walk(guide)):
        for name in sorted(names):
            if not name.endswith(".md"):
                continue
            page = os.path.relpath(os.path.join(directory, name), guide).replace(os.sep, "/")
            with open(os.path.join(directory, name), encoding="utf-8") as f:
                lines = f.read().replace("\r\n", "\n").split("\n")
            i = 0
            while i < len(lines):
                if lines[i].strip() != "```ceesafe":
                    i += 1
                    continue
                m = DIRECTIVE.match(lines[i - 1].strip()) if i > 0 else None
                directive = m.group(1).strip() if m else ""
                start = end = i + 1
                while end < len(lines) and lines[end].strip() != "```":
                    end += 1
                code = "\n".join(lines[start:end]) + "\n"
                output = None
                after = end + 1
                while after < len(lines) and not lines[after].strip():
                    after += 1
                if after < len(lines) and lines[after].strip() == "```text":
                    out_end = after + 1
                    while out_end < len(lines) and lines[out_end].strip() != "```":
                        out_end += 1
                    output = "\n".join(lines[after + 1:out_end]) + "\n"
                found.append((page, start, directive, code, output))
                i = end + 1
    return found


def rendered(stderr):
    """The messages as the page shows them: each diagnostic's rendering, separated by an empty line (SiteTests joins
    DiagnosticRenderer.Render's texts, which end in a newline, with another)."""
    lines = []
    for line in stderr.rstrip("\n").split("\n"):
        if lines and HEADER.match(line):
            lines.append("")
        lines.append(line)
    return "\n".join(lines) + "\n"


def check(cee, example):
    page, line, directive, code, output = example
    words = directive.split()
    where = f"{page}:{line}"
    if words[:1] == ["skip"]:
        return None
    # `freestanding` goes with any other directive, before or after it: it is the profile, not one of `fail`'s codes.
    profile = ["--profile", "freestanding"] if "freestanding" in words else []
    words = [word for word in words if word != "freestanding"]
    with tempfile.TemporaryDirectory() as work:
        with open(os.path.join(work, "main.cee"), "w", encoding="utf-8") as f:
            f.write(code)
        text = subprocess.run([cee, "check", *profile, "main.cee"], cwd=work, capture_output=True, text=True)
        if words[:1] == ["fail"] or words[:1] == ["warns"]:
            if words[:1] == ["warns"] and text.returncode != 0:
                return f"{where}: warns, so it must check; it fails:\n{text.stderr}"
            report = subprocess.run([cee, "check", *profile, "--diagnostics-format=json", "main.cee"], cwd=work,
                                    capture_output=True, text=True)
            codes = sorted({d["code"] for d in json.loads(report.stdout)["diagnostics"]})
            if codes != sorted(words[1:]):
                return f"{where}: fails with {' '.join(codes) or 'nothing'}, the page says {' '.join(words[1:])}"
            if output is not None and output != rendered(text.stderr):
                return f"{where}: the messages differ from the page's:\n{text.stderr}"
            return None
        if text.returncode != 0:
            return f"{where}: does not check:\n{text.stderr}"
        if profile or "check" in words or not re.search(r"\bfn main\(", code):
            return f"{where}: an output block needs a program that runs" if output is not None else None
        run = subprocess.run([cee, "run", "--interp", "main.cee"], cwd=work, capture_output=True, text=True,
                             stdin=subprocess.DEVNULL, timeout=120)
        if output is not None and run.stdout != output:
            return f"{where}: prints {run.stdout!r}, the page shows {output!r}"
        expected = 134 if "panics" in words else int(words[1]) if words[:1] == ["exits"] else 0
        if run.returncode != expected:
            return f"{where}: exit {run.returncode}, expected {expected}\n{run.stderr}"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ceesafec", required=True)
    parser.add_argument("--guide", default=os.path.join(ROOT, "guide"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()
    cee = os.path.abspath(args.ceesafec)
    found = examples(args.guide)
    with ThreadPoolExecutor(args.jobs) as pool:
        problems = [p for p in pool.map(lambda e: check(cee, e), found) if p]
    for p in problems:
        print("FAIL", p)
    skipped = sum(1 for e in found if e[2].split()[:1] == ["skip"])
    print(f"guide_check: {len(found)} examples ({skipped} skipped): {len(problems)} problems")
    return 1 if problems or not found else 0


if __name__ == "__main__":
    sys.exit(main())
