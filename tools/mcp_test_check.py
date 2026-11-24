#!/usr/bin/env python3
"""ceesafe_test and ceesafe_doc over the MCP server (ADR 0220; spec/tooling.md §6.3).

- ceesafe_test on tests/packages/test-command: each test's outcome is the `--interp` column of the package's
  expected.tsv (reviewed by hand, ADR 0049), except that `timed out` is `fault` (the interpreter's budget is steps, not
  time, ADR 0220 §2.1), and the message holds the expected part; the counts add up; a `filter` runs only the tests
  whose names contain it, in the order of their names; a filter no name contains is a tool error.
- ceesafe_test on `files`: a test that reads a file is a `fault` that names the sandbox's refusal, a passing one passes.
- ceesafe_doc on `files`: the page has each public item's heading, and `missing` lists exactly the public items
  written here without a doc comment, as `path:line: kind name` would, by line.
Nothing is written to disk: the check runs the server in a copy of the package.

usage: python3 tools/mcp_test_check.py --compiler out/ceesafec
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "tests", "packages", "test-command")

FILE_TESTS = """\
@test
fn reads() -> Result<void, IoError> {
    let _ = File.open("data.txt")?;
    return Ok();
}

@test
fn adds() {
    assert(1 + 1 == 2);
}
"""

DOC_SOURCE = """\
/// The documented one.
public fn documented() -> i32 => 1;

public fn bare() -> i32 => 2;

fn hidden() -> i32 => 3;

/// A point.
public struct Point {
    x: i32;
}

public enum Shade {
    Light,
    Dark,
}
"""
# What `missing` must list for DOC_SOURCE, by hand: the public items without a doc comment.
DOC_MISSING = [("lib.cee", 4, "fn", "bare"), ("lib.cee", 13, "enum", "Shade")]


def expected_outcomes():
    rows = []
    for line in open(os.path.join(FIXTURE, "expected.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        name, _native, interp, part = line.rstrip("\n").split("\t")
        rows.append((name, "fault" if interp == "timed out" else interp, None if part == "-" else part))
    return rows


def call_all(compiler, directory, calls):
    messages = [{"jsonrpc": "2.0", "id": 0, "method": "initialize",
                 "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                            "clientInfo": {"name": "check", "version": "1"}}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 1, "method": "tools/list"}]
    for k, (name, arguments) in enumerate(calls):
        messages.append({"jsonrpc": "2.0", "id": k + 2, "method": "tools/call",
                         "params": {"name": name, "arguments": arguments}})
    run = subprocess.run([compiler, "mcp"], input="".join(json.dumps(m) + "\n" for m in messages),
                         capture_output=True, text=True, cwd=directory, timeout=1800)
    answers = {}
    for line in run.stdout.splitlines():
        reply = json.loads(line)
        answers[reply.get("id")] = reply
    tools = [t["name"] for t in answers.get(1, {}).get("result", {}).get("tools", [])]
    return tools, [answers.get(k + 2) for k in range(len(calls))]


def content(reply):
    result = reply["result"]
    if "structuredContent" in result:
        return result["structuredContent"], result.get("isError", False)
    return json.loads(result["content"][0]["text"]), result.get("isError", False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    work = tempfile.mkdtemp(prefix="mcp-test-")
    try:
        package = os.path.join(work, "test-command")
        shutil.copytree(FIXTURE, package)
        calls = [
            ("ceesafe_test", {"package": "."}),
            ("ceesafe_test", {"package": ".", "filter": "fails"}),
            ("ceesafe_test", {"package": ".", "filter": "no-such-test"}),
            ("ceesafe_test", {"files": [{"path": "t.cee", "text": FILE_TESTS}]}),
            ("ceesafe_doc", {"files": [{"path": "lib.cee", "text": DOC_SOURCE}]}),
            ("ceesafe_doc", {"files": [{"path": "lib.cee", "text": DOC_SOURCE}], "missing": True}),
        ]
        tools, replies = call_all(compiler, package, calls)
        for name in ("ceesafe_test", "ceesafe_doc"):
            if name not in tools:
                problems.append(f"tools/list does not list {name}: {tools}")
        if any(r is None or "result" not in r for r in replies):
            problems.append(f"a call had no result: {[r for r in replies if r is None or 'result' not in r][:1]}")
            replies = [r if r and "result" in r else {"result": {"structuredContent": {}}} for r in replies]

        # The whole package, against expected.tsv.
        whole, _ = content(replies[0])
        got = {t["name"]: t for t in whole.get("tests", [])}
        expected = expected_outcomes()
        if [t["name"] for t in whole.get("tests", [])] != [name for name, _, _ in expected]:
            problems.append(f"tests or their order: {[t['name'] for t in whole.get('tests', [])]}")
        for name, outcome, part in expected:
            t = got.get(name)
            if t is None:
                continue
            if t["outcome"] != outcome:
                problems.append(f"{name}: {t['outcome']}, expected {outcome}")
            if part and part not in t["message"]:
                problems.append(f"{name}: message {t['message'][:120]!r} lacks {part!r}")
        passed = sum(1 for _, o, _ in expected if o == "passed")
        skipped = sum(1 for _, o, _ in expected if o == "skipped")
        if (whole.get("passed"), whole.get("skipped"), whole.get("failed")) != (passed, skipped,
                                                                              len(expected) - passed - skipped):
            problems.append(f"counts {whole.get('passed')}/{whole.get('failed')}/{whole.get('skipped')}")
        if not all(isinstance(t.get("steps"), int) and t["steps"] > 0 for t in whole.get("tests", [])
                   if t["outcome"] != "skipped"):
            problems.append("a test that ran has no positive `steps`")
        hangs = got.get("hangs", {}).get("message", "")
        if "step limit" not in hangs:
            problems.append(f"hangs's fault does not name the step limit: {hangs[:120]!r}")

        # A filter.
        filtered, _ = content(replies[1])
        names = [t["name"] for t in filtered.get("tests", [])]
        wanted = sorted(n for n, _, _ in expected if "fails" in n)
        if names != wanted:
            problems.append(f"filter 'fails' ran {names}, expected {wanted}")
        _, failed_filter = content(replies[2])
        if not failed_filter:
            problems.append("a filter no name contains was not a tool error")

        # A test that reads a file, from `files`.
        from_files, _ = content(replies[3])
        by_name = {t["name"]: t for t in from_files.get("tests", [])}
        reads = by_name.get("reads", {})
        if reads.get("outcome") != "fault" or "sandbox" not in reads.get("message", ""):
            problems.append(f"reads: {reads.get('outcome')} {reads.get('message', '')[:120]!r}, expected a fault "
                            "naming the sandbox")
        if by_name.get("adds", {}).get("outcome") != "passed":
            problems.append(f"adds: {by_name.get('adds')}")

        # ceesafe_doc.
        page, _ = content(replies[4])
        markdown = page.get("markdown", "")
        for heading in ("documented", "bare", "Point", "Shade"):
            if heading not in markdown:
                problems.append(f"the page lacks {heading}")
        if "hidden" in markdown:
            problems.append("the page documents a private function")
        listed, _ = content(replies[5])
        items = [(i["path"], i["line"], i["kind"], i["name"]) for i in listed.get("missing", [])]
        if items != DOC_MISSING:
            problems.append(f"missing listed {items}, expected {DOC_MISSING}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for p in problems:
        print("FAIL", p)
    print(f"mcp_test_check: ceesafe_test on {len(expected_outcomes())} tests, a filter and files; ceesafe_doc: "
          f"{len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
