#!/usr/bin/env python3
"""The oracle of tests/support/run-pass/json-tree.cee: Python's json module on the documents of that file.

It prints the test's `// expect-stdout:` lines for the documents (one a line, in the raw literal of `documents()`):
whether each is JSON, its tree written back, the tree with only the members the test keeps, and the root's member
`a`. The last line, the deep array, is arithmetic: Python's parser is recursive and cannot read it.

usage: python3 tools/oracles/json_tree.py
"""
import json
import re
from pathlib import Path

TEST = Path(__file__).resolve().parents[2] / "tests" / "support" / "run-pass" / "json-tree.cee"
KEEP = {"id", "kind", "inner", "name"}


class Number(str):
    """A number as it is written."""


class Pairs(list):
    """An object's members, in order, duplicates and all."""


def refuse(name):
    raise ValueError(name)      # NaN, Infinity: not JSON


def parse(text: str):
    return json.loads(text, object_pairs_hook=Pairs, parse_int=Number, parse_float=Number, parse_constant=refuse)


def dump(value, keep=None) -> str:
    if isinstance(value, Pairs):
        return "{" + ",".join(f"{k}:{dump(v, keep)}" for k, v in value if keep is None or k in keep) + "}"
    if isinstance(value, list):
        return "[" + ",".join(dump(v, keep) for v in value) + "]"
    if isinstance(value, Number):
        return str(value)
    if isinstance(value, str):
        try:
            return '"' + value.encode("utf-8").hex() + '"'
        except UnicodeEncodeError:      # a lone surrogate: not a string the reader decodes
            return '"?"'
    return {True: "true", False: "false", None: "null"}[value]


def main():
    source = TEST.read_text(encoding="utf-8")
    body = re.search(r'fn documents\(\) -> StringView => """\n(.*?)\n    """;', source, re.S).group(1)
    for index, line in enumerate(body.split("\n")):
        document = line[4:] if line.startswith("    ") else line
        try:
            value = parse(document)
        except ValueError:
            print(f"// expect-stdout: {index}: invalid")
            continue
        shown = f"{dump(value)} | kept {dump(value, KEEP)}"
        if isinstance(value, Pairs) and any(k == "a" for k, _ in value):
            shown += f" | a {dump([v for k, v in value if k == 'a'][-1])}"
        print(f"// expect-stdout: {index}: {shown}")
    print("// expect-stdout: deep: 200000 arrays, each in the one before")


if __name__ == "__main__":
    main()
