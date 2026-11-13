#!/usr/bin/env python3
"""Writes book/src/appendix-h-grammar.md: the template, with the productions copied from the specification.

The template is book/appendices/appendix-h.template.md: the page's own text, written by hand, with a `@@NAME@@` line
where a group of productions goes. The productions are the unlabelled fenced blocks of spec/grammar.md, in order, the
rows of the precedence table of its §2.1, and the first `text` block of spec/type-system.md §15.2 (patterns). Nothing
in a block is edited: the big block of grammar.md §2 is cut at its blank lines, and that is all.

usage: python3 tools/gen_appendix_h.py            (writes the page; review the diff)
       python3 tools/gen_appendix_h.py --check    (writes nothing; exit 1 when the committed page would change)

tools/book_check.py makes the same comparison (tools/book_appendix_check.py), so a change to the grammar without a
new page fails the contract suite.
"""
import re
import sys

import book_appendix_gen
from book_appendix_gen import need, read

PAGE = "book/src/appendix-h-grammar.md"
SCRIPT = "tools/gen_appendix_h.py"
TEMPLATE = book_appendix_gen.INPUTS + "/appendix-h.template.md"
SOURCES = f"{TEMPLATE}, spec/grammar.md and spec/type-system.md §15.2"


def unlabelled_blocks(lines):
    """The contents of the fenced blocks that open with a bare ``` (the productions), in order."""
    blocks, i = [], 0
    while i < len(lines):
        fence = lines[i].strip()
        if not fence.startswith("```"):
            i += 1
            continue
        end = i + 1
        while end < len(lines) and lines[end].strip() != "```":
            end += 1
        need(end < len(lines), "spec/grammar.md has a fenced block that is not closed")
        if fence == "```":
            blocks.append(lines[i + 1:end])
        i = end + 1
    return blocks


def chunks(block):
    """A block cut at its blank lines."""
    out, current = [], []
    for line in block:
        if line.strip():
            current.append(line)
        elif current:
            out.append(current)
            current = []
    if current:
        out.append(current)
    return out


def after(lines, heading, path):
    start = next((i for i, line in enumerate(lines) if line.startswith(heading)), None)
    need(start is not None, f"{path} has no '{heading}'")
    return start


def precedence_rows(lines):
    start = after(lines, "### 2.1", "spec/grammar.md")
    first = next((i for i in range(start, len(lines)) if lines[i].startswith("|")), None)
    need(first is not None, "spec/grammar.md §2.1 has no table")
    last = first
    while last < len(lines) and lines[last].startswith("|"):
        last += 1
    return lines[first:last]


def pattern_block(lines):
    start = after(lines, "### 15.2", "spec/type-system.md")
    first = next((i for i in range(start, len(lines)) if lines[i].strip() == "```text"), None)
    need(first is not None, "spec/type-system.md §15.2 has no `text` block")
    last = first + 1
    while last < len(lines) and lines[last].strip() != "```":
        last += 1
    need(last < len(lines), "spec/type-system.md §15.2's `text` block is not closed")
    return lines[first + 1:last]


def fenced(block):
    return "```text\n" + "\n".join(block) + "\n```"


def render(root):
    """The page's text, without the note that it is generated."""
    grammar = read(root, "spec/grammar.md").replace("\r\n", "\n").split("\n")
    types = read(root, "spec/type-system.md").replace("\r\n", "\n").split("\n")
    blocks = unlabelled_blocks(grammar)
    need(len(blocks) == 7, f"spec/grammar.md has {len(blocks)} unlabelled fenced blocks, and the template places 7")
    syntactic = chunks(blocks[5])
    need(len(syntactic) == 7, f"the block of spec/grammar.md §2 has {len(syntactic)} groups between its blank lines,"
                              " and the template places 7")
    need(syntactic[0][0].startswith("CompilationUnit") and syntactic[5][0].startswith("Type =")
         and syntactic[6][0].startswith("Block =") and blocks[6][0].startswith("Primary ="),
         "the groups of spec/grammar.md §2 no longer start with CompilationUnit, …, Type, Block, and §2.1's with Primary")
    parts = {
        "IDENTIFIERS": fenced(blocks[0]),
        "KEYWORDS": fenced(blocks[1]),
        "INTEGERS": fenced(blocks[2]),
        "FLOATS": fenced(blocks[3]),
        "PUNCTUATION": fenced(blocks[4]),
        "UNIT": fenced(syntactic[0]),
        "ITEMS": fenced(syntactic[1]),
        "FUNCTIONS": fenced(syntactic[2]),
        "STRUCTS": fenced(syntactic[3]),
        "CONSTANTS": fenced(syntactic[4]),
        "TYPES": fenced(syntactic[5]),
        "STATEMENTS": fenced(syntactic[6]),
        "PRECEDENCE": "\n".join(precedence_rows(grammar)),
        "EXPRESSIONS": fenced(blocks[6]),
        "PATTERNS": fenced(pattern_block(types)),
    }
    page = read(root, TEMPLATE)
    for name, text in parts.items():
        marker = "@@" + name + "@@"
        need(page.count(marker) == 1, f"{TEMPLATE} has {marker} {page.count(marker)} times, not once")
        page = page.replace(marker, text)
    left = re.search(r"@@[A-Z]+@@", page)
    need(not left, f"{TEMPLATE} has {left.group(0) if left else ''}, which is no group of productions")
    return page


if __name__ == "__main__":
    sys.exit(book_appendix_gen.run(sys.modules[__name__], __doc__))
