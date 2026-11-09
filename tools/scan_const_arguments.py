#!/usr/bin/env python3
"""ADR 0083 §2.1's release scan: find `Identifier '<' … '>'` that 0.26.0 reads as generic arguments and 0.25.0 did
not, because the tokens between hold `true` or `false` (the only tokens the lookahead gains), and the `>` is followed
by a token of the lookahead's follow set. Mirrors Parser.isGenericArgumentListAhead (src/syntax/parser.cee).

usage: python3 -I tools/scan_const_arguments.py ROOT...   (.cee files, and ```ceesafe blocks of .md files)
"""
import os
import re
import sys

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lcomment>//[^\n]*)
  | (?P<bcomment>/\*.*?\*/)
  | (?P<raw>\"\"\".*?\"\"\")
  | (?P<string>c?"(?:\\.|[^"\\\n])*")
  | (?P<char>'(?:\\.|[^'\\\n])+')
  | (?P<number>0[xXbBoO][0-9a-fA-F_]+[a-z0-9]*|[0-9][0-9_]*(?:\.[0-9][0-9_]*)?(?:[eE][+-]?[0-9]+)?[a-z0-9]*)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>->|=>|\?\?|<=|>=|==|!=|&&|\|\||<<|>>|\.\.=|\.\.|[-+*/%<>=!&|^~?:;,.(){}\[\]@#])
  | (?P<other>.)
""", re.S | re.X)

ALLOWED_IDENTS = {"true", "false", "borrow", "mut"}


def tokens(text):
    out = []
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ("ws", "lcomment", "bcomment"):
            continue
        value = m.group(0)
        line = text.count("\n", 0, m.start()) + 1
        if kind == "ident":
            if value in ("true", "false"):
                kind = "bool"
            elif value == "fn":
                kind = "fn"
        out.append((kind, value, line))
    return out


def ahead(toks, i):
    """At toks[i] == '<' after an identifier: (generic, sawBool) by the lookahead's rules, bools allowed."""
    depth = 0
    in_fn = False
    saw_dyn = False
    saw_bool = False
    k = i
    while k < len(toks):
        kind, value, _ = toks[k]
        if kind == "ident" and value == "dyn":
            saw_dyn = True
        function_part = value in ("(", ")", "->")
        if kind == "fn" or value == "@":
            in_fn = True
        elif function_part and in_fn:
            pass
        elif value in ("<", "["):
            depth += 1
        elif value in (">", "]"):
            depth -= 1
            if depth == 0:
                if value != ">":
                    return False, saw_bool
                nxt = toks[k + 1][1] if k + 1 < len(toks) else ""
                return nxt in ("(", ".", "{", ")", ",", ";", "]"), saw_bool
        elif value == ">>":
            return False, saw_bool  # the lexer's `>>` ends no list here (as in the parser)
        else:
            if kind == "bool":
                saw_bool = True
                allowed = True
            elif kind == "ident":
                allowed = True
            elif kind == "number":
                allowed = True
            elif value in (".", ",", ";", "*", "?", "??"):
                allowed = True
            elif value == "+":
                allowed = saw_dyn
            else:
                allowed = False
            if not allowed:
                return False, saw_bool
        k += 1
    return False, saw_bool


def scan_text(path, text, base_line, hits):
    toks = tokens(text)
    for i in range(1, len(toks)):
        if toks[i][1] != "<" or toks[i - 1][0] != "ident":
            continue
        generic, saw_bool = ahead(toks, i)
        if generic and saw_bool:
            hits.append((path, base_line + toks[i][2] - 1, " ".join(t[1] for t in toks[i - 1:i + 12])))


def main():
    hits = []
    files = 0
    for root in sys.argv[1:]:
        for directory, dirs, names in os.walk(root):
            dirs[:] = [d for d in dirs if d not in (".git", "out", "node_modules", "upstream", "target")]
            for name in names:
                path = os.path.join(directory, name)
                if name.endswith(".cee"):
                    files += 1
                    with open(path, encoding="utf-8", errors="replace") as f:
                        scan_text(path, f.read(), 1, hits)
                elif name.endswith(".md"):
                    with open(path, encoding="utf-8", errors="replace") as f:
                        text = f.read()
                    for m in re.finditer(r"```(?:ceesafe|cee)\n(.*?)```", text, re.S):
                        files += 1
                        scan_text(path, m.group(1), text.count("\n", 0, m.start(1)) + 1, hits)
    print(f"scanned {files} files or blocks; {len(hits)} hits")
    for path, line, snippet in hits:
        print(f"{path}:{line}: {snippet}")


main()
