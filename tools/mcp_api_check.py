#!/usr/bin/env python3
"""ceesafe_api over the MCP server (spec/tooling.md §6.3): the tool is listed, a type, a qualified member and a bare
member name find their entries in lib/core, lib/std and the built-in types page, and a name that does not exist gets
suggestions that include the real one. The expectations are names the documents themselves state (spec/api/*.md,
guide/builtins.md), not output of the code under test.

usage: python3 tools/mcp_api_check.py --compiler out/ceesafec
"""
import argparse
import json
import subprocess
import sys

# (query, a document and heading that must be among the results), or (query, None, a name the suggestions must hold).
CASES = [
    ("TextWriter", "spec/api/core.md", "## struct TextWriter"),
    ("TextWriter.decimal", "spec/api/core.md", "### TextWriter.decimal"),
    ("decimal", "spec/api/core.md", "### TextWriter.decimal"),
    ("Timer.now", "spec/api/core.md", "### Timer.now"),
    ("AsyncTcpListener.accept", "spec/api/std.md", "### AsyncTcpListener.accept"),
    ("Environment.variable", "spec/api/std.md", "### Environment.variable"),
    ("Process.argument", "guide/builtins.md", "## `Process` and program arguments"),
    ("Vector.push", "guide/builtins.md", "## `Vector`, arrays and slices"),
    ("StringView.fromUtf8", "guide/builtins.md", "## `StringView` and `String`"),
    ("Console", "guide/builtins.md", "## `Console`"),
    ("unsigned", None, "TextWriter.decimal"),       # by what the entry's text says
    ("TextWriter.unsigned", None, "TextWriter.signed"),
    ("Environment.args", None, "Environment.variable"),  # the type's real members
    ("Vector.pushBack", None, "Vector.push"),
    ("Strng", None, "String"),                     # one edit away
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    messages = [
        {"jsonrpc": "2.0", "id": 0, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "check", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
    ]
    for k, (query, _, _) in enumerate(CASES):
        messages.append({"jsonrpc": "2.0", "id": k + 2, "method": "tools/call",
                         "params": {"name": "ceesafe_api", "arguments": {"name": query}}})
    run = subprocess.run([args.compiler, "mcp"], input="".join(json.dumps(m) + "\n" for m in messages),
                         capture_output=True, text=True, timeout=120)
    answers = {}
    for line in run.stdout.splitlines():
        reply = json.loads(line)
        answers[reply.get("id")] = reply
    problems = []
    tools = [t["name"] for t in answers.get(1, {}).get("result", {}).get("tools", [])]
    if "ceesafe_api" not in tools:
        problems.append(f"tools/list does not list ceesafe_api: {tools}")
    for k, (query, document, expected) in enumerate(CASES):
        reply = answers.get(k + 2)
        if reply is None or "result" not in reply:
            problems.append(f"{query}: no result ({reply})")
            continue
        result = reply["result"].get("structuredContent") or json.loads(reply["result"]["content"][0]["text"])
        if document is None:
            if result["results"] or expected not in result["suggestions"]:
                problems.append(f"{query}: expected no entries and the suggestion {expected!r}, got "
                                f"{len(result['results'])} entries and {result['suggestions']}")
        elif not any(r["document"] == document and r["heading"] == expected for r in result["results"]):
            problems.append(f"{query}: no entry {document} {expected!r} among "
                            f"{[(r['document'], r['heading']) for r in result['results']]}")
    for p in problems:
        print("FAIL", p)
    print(f"mcp_api_check: {len(CASES)} lookups: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
