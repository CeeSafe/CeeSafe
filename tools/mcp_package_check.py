#!/usr/bin/env python3
"""The MCP server's `package` source (spec/tooling.md §6.2; ADR 0208), on tests/packages' fixtures:

- agreement: `ceesafe_check {"package": "."}` reports the diagnostics `ceesafec check --offline` reports for the same
  package (code, file, line and column), where `paths` of the root's sources alone cannot find the dependencies;
- names: from the directory above, `{"package": "app"}` names the files from the server's working directory;
- overlay: a `files` entry replaces a source's text for the call, and the file on disk is unchanged;
- fix: fixes are applied to the root package, and only the changed files come back;
- refusals: `package` with `paths` or `profile` is InvalidParams; no manifest, a package without its lock and a
  `files` entry that is no source are tool errors.

The expectations are the command's own report and what ADR 0208 states, not output recorded from the server.

usage: python3 tools/mcp_package_check.py --compiler out/ceesafec
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
IMPORTS = os.path.join(ROOT, "tests", "packages", "imports")
MISSING_LOCK = os.path.join(ROOT, "tests", "packages", "missing-lock", "app")


def serve(compiler, directory, calls):
    """The replies to `calls` ((tool, arguments) pairs), by position, from a server started in `directory`."""
    messages = [{"jsonrpc": "2.0", "id": 0, "method": "initialize",
                 "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                            "clientInfo": {"name": "check", "version": "1"}}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"}]
    for k, (tool, arguments) in enumerate(calls):
        messages.append({"jsonrpc": "2.0", "id": k + 1, "method": "tools/call",
                         "params": {"name": tool, "arguments": arguments}})
    run = subprocess.run([compiler, "mcp"], input="".join(json.dumps(m) + "\n" for m in messages),
                         capture_output=True, text=True, timeout=300, cwd=directory)
    answers = {}
    for line in run.stdout.splitlines():
        reply = json.loads(line)
        answers[reply.get("id")] = reply
    return [answers.get(k + 1, {}) for k in range(len(calls))]


def content(reply):
    """A tool result's JSON, and whether it is an error; (None, None) for a protocol error or no reply."""
    result = reply.get("result")
    if result is None:
        return None, None
    return json.loads(result["content"][0]["text"]), result.get("isError", False)


def places(document, prefix=""):
    return sorted((d["code"], prefix + d["file"], d["start"]["line"], d["start"]["column"])
                  for d in document["diagnostics"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    app = os.path.join(IMPORTS, "app")
    problems = []

    command = subprocess.run([compiler, "check", "--offline", "--diagnostics-format=json"], cwd=app,
                             capture_output=True, text=True, timeout=300)
    expected = json.loads(command.stdout)
    main_path = os.path.join(app, "src", "main.cee")
    with open(main_path, encoding="utf-8") as f:
        main_text = f.read()
    lines = main_text.count("\n")
    wrong = main_text + "fn mcpWrong() -> i32 => true;\n"
    fixable = main_text + "fn mcpBump(x: borrow mut i32) { x += 1; }\nfn mcpUse() { let n = 1; mcpBump(n); }\n"

    replies = serve(compiler, app, [
        ("ceesafe_check", {"package": "."}),
        ("ceesafe_check", {"paths": ["src/main.cee", "src/sub/helper.cee"]}),
        ("ceesafe_check", {"package": ".", "files": [{"path": "src/main.cee", "text": wrong}]}),
        ("ceesafe_fix", {"package": ".", "files": [{"path": "src/main.cee", "text": fixable}]}),
        ("ceesafe_check", {"package": ".", "paths": ["src/main.cee"]}),
        ("ceesafe_check", {"package": ".", "profile": "hosted"}),
        ("ceesafe_check", {"package": ".", "files": [{"path": "src/absent.cee", "text": ""}]}),
        ("ceesafe_symbol", {"package": ".", "path": "src/main.cee", "line": 11, "column": 20}),
    ])

    checked, failed = content(replies[0])
    if checked is None or failed:
        problems.append(f"agreement: no diagnostics document ({replies[0]})")
    elif places(checked) != places(expected):
        problems.append(f"agreement: the tool reports {places(checked)}, the command {places(expected)}")

    alone, _ = content(replies[1])
    if alone is None or not any(d["code"] in ("N0200", "N0310") for d in alone["diagnostics"]):
        problems.append("agreement: 'paths' alone was expected not to find the dependencies (N0200, or N0310 at an "
                        "import); the fixture no longer shows the difference")

    overlaid, failed = content(replies[2])
    if overlaid is None or failed:
        problems.append(f"overlay: no diagnostics document ({replies[2]})")
    elif ("N0205", "src/main.cee", lines + 1) not in [(c, f, l) for c, f, l, _ in places(overlaid)]:
        problems.append(f"overlay: no N0205 at src/main.cee:{lines + 1}, the line the call added: {places(overlaid)}")
    with open(main_path, encoding="utf-8") as f:
        if f.read() != main_text:
            problems.append("overlay: the file on disk changed")

    fixed, failed = content(replies[3])
    if fixed is None or failed:
        problems.append(f"fix: no result ({replies[3]})")
    else:
        names = [f["path"] for f in fixed["files"]]
        if names != ["src/main.cee"]:
            problems.append(f"fix: expected the one changed file, src/main.cee, got {names}")
        elif "var n = 1; mcpBump(mut n);" not in fixed["files"][0]["text"]:
            problems.append("fix: N0249's fix (var, mut) was not applied to the text given")
        if fixed["applied"] < 1:
            problems.append("fix: no fix counted")

    for k, what in ((4, "'package' with 'paths'"), (5, "'package' with 'profile'")):
        if replies[k].get("error", {}).get("code") != -32602:
            problems.append(f"refusals: {what} is not InvalidParams: {replies[k]}")
    for k, what in ((6, "a 'files' entry that is no source"),):
        _, failed = content(replies[k])
        if not failed:
            problems.append(f"refusals: {what} is not a tool error: {replies[k]}")

    symbol, failed = content(replies[7])
    if symbol is None or failed or "square" not in symbol.get("hover", ""):
        problems.append(f"symbol: no hover naming 'square' at src/main.cee:11:20 ({replies[7]})")

    above, failed = content(serve(compiler, IMPORTS, [("ceesafe_check", {"package": "app"})])[0])
    if above is None or failed:
        problems.append("names: no diagnostics document from the directory above")
    elif places(above) != places(expected, "app/"):
        problems.append(f"names: from the directory above, {places(above)}, expected {places(expected, 'app/')}")

    _, failed = content(serve(compiler, MISSING_LOCK, [("ceesafe_check", {"package": "."})])[0])
    if not failed:
        problems.append("refusals: a package without its lock is not a tool error")
    with tempfile.TemporaryDirectory() as empty:
        _, failed = content(serve(compiler, empty, [("ceesafe_check", {"package": "."})])[0])
        if not failed:
            problems.append("refusals: a directory without a manifest is not a tool error")

    for p in problems:
        print("FAIL", p)
    print(f"mcp_package_check: 12 checks: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
