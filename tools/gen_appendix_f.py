#!/usr/bin/env python3
"""Writes book/src/appendix-f-diagnostics.md: a row for every diagnostic code of the compiler's catalog.

It reads
- the codes: the arms of summaryOf in src/diagnostics/catalog.cee (tools/gen_catalog.py writes it from spec/, and
  `ceesafec explain` prints it, so no compiler is run);
- a row's "Shown in": the chapters' sections whose header names the code (`diagnostics: [N0249]`, book/src/ch*.md);
- book/appendices/appendix-f.intro.md, written by hand: the page's header and its opening, up to the first table;
- book/appendices/appendix-f.lines.txt, written by hand: `N0249<tab>what went wrong, and the usual fix`, one line for
  each code that is given.
The groups, their sentences, the warnings and the codes that are not given are below.

usage: python3 tools/gen_appendix_f.py            (writes the page; review the diff)
       python3 tools/gen_appendix_f.py --check    (writes nothing; exit 1 when the committed page would change)

tools/book_check.py makes the same comparison (tools/book_appendix_check.py), so a new code, or a chapter header
that names another code, without a new page fails the contract suite.
"""
import glob
import os
import re
import sys

import book_appendix_gen
from book_appendix_gen import need, read

PAGE = "book/src/appendix-f-diagnostics.md"
SCRIPT = "tools/gen_appendix_f.py"
CATALOG = "src/diagnostics/catalog.cee"
INTRO = book_appendix_gen.INPUTS + "/appendix-f.intro.md"
LINES = book_appendix_gen.INPUTS + "/appendix-f.lines.txt"
SOURCES = f"{INTRO}, appendix-f.lines.txt, {CATALOG} and the chapters' `diagnostics:` headers"

SITE = "https://ceesafe.github.io/CeeSafe/reference/diagnostics#"
WARNINGS = {"N0400", "N0401", "N0402", "N0403", "N0404"}  # the codes src/ reports with diagnostics.warning(…)
NOT_GIVEN = {
    "N0002": "Unassigned. No release has given it.",
    "N0223": "Unassigned. No release has given it.",
    "N0258": "Retired. It was given for a payload that is not Copy bound by a `match` on a value. Such a `match` now moves the payload out and consumes the value.",
}
# (heading, the range of numbers, the specification's anchor, the sentences under the heading)
GROUPS = [
    ("Lexer (N0001 to N0011)", range(1, 100), "lexer-n0001–n0099",
     "The lexer cuts a file into tokens - names, numbers, literals, operators. It finishes a file before the parser "
     "starts, so these come first among a file's diagnostics."),
    ("Parser (N0100 to N0110)", range(100, 200), "parser-n0100–n0199",
     "The parser reports one diagnostic for each broken statement or item, and goes on at the next one. The rules "
     "after this group are checked only when every file parses."),
    ("Names, types and flow (N0200 to N0236)", range(200, 240), "names-types-and-flow-n0200–n0299",
     "The type checker's diagnostics - names, types, constants, assignments, and the paths through a function."),
    ("Borrows (N0240 to N0249)", range(240, 250), "references-and-borrows-n0240–n0249-lifetimes-md",
     "The borrow checker's diagnostics. It runs only on a program that type-checks, and its errors carry notes that "
     "say where the borrow was taken and where it is used later. The messages say \"reference\" for a borrow."),
    ("Moves, destruction, `match` and arrays (N0250 to N0259)", range(250, 260),
     "moves-and-destruction-n0250–n0259-type-system-md-§14",
     "The move checker also runs only on a program that type-checks. A move of a value that is borrowed is N0240."),
    ("Results, the heap, generics and interfaces (N0260 to N0268)", range(260, 270),
     "results-and-the-heap-n0260–n0269-type-system-md-§17–§18",
     "`Result`, optionals and allocators, and what generic code and `impl` blocks must satisfy."),
    ("Regions (N0270 to N0274)", range(270, 280), "region-parameters-n0270–n0279-type-system-md-§25-m6x",
     "A region parameter says what a struct, an enum or a function's result borrows from."),
    ("Unsafe code and raw pointers (N0280 to N0283)", range(280, 290),
     "unsafe-and-raw-pointers-n0280–n0289-type-system-md-§27-unsafe-md-m8a",
     "What may be written only in `unsafe`, and what raw pointers may point to."),
    ("C, profiles, effects and closures' kinds (N0290 to N0299)", range(290, 300),
     "c-interoperability-and-profiles-n0290–n0299-type-system-md-§28-m8b-profiles-md-§4-m8c",
     "Attributes, the C boundary, the freestanding profile, effects, `private`, and functions as values."),
    ("Build (N0300)", range(300, 310), "build-n0300–n0399", "Reported by `ceesafec build`, not by `ceesafec check`."),
    ("Names across packages (N0310 to N0315)", range(310, 320), "build-n0300–n0399",
     "Names that one package uses from another."),
    ("Packages (N0320 to N0329, N0335)", list(range(320, 330)) + [335], "build-n0300–n0399",
     "The manifest `ceesafe.toml`, the lock file `ceesafe.lock`, dependencies, versions and publishing. These are "
     "reported at a line of the manifest or the lock file, not of a source file."),
    ("Later rules (N0330 to N0364)", [n for n in range(330, 400) if n != 335], "build-n0300–n0399",
     "The codes of the features that came after the groups above were numbered - `dyn`, owning closures, threads, "
     "statics, async, tests, cells, const parameters, patterns, `debugAssert`, embedded files and the package "
     "cache. The specification lists them in one table under \"Build\"."),
    ("Warnings (N0400 to N0404)", range(400, 500), "warnings-n0400–n0499",
     "A warning is printed as `warning[N0400]`. It never stops `check`, `build`, `run` or `test`, and never changes "
     "the exit status, unless the command has `--deny-warnings`, which makes each warning an error "
     "([9.2](./ch09-02-result), [14.1](./ch14-01-build-settings)). Warnings are given for your own package's files "
     "only, never for a dependency's."),
]


def catalog_codes(root):
    """The codes of the compiler, from the arms `    249 => "…",` of summaryOf."""
    text = read(root, CATALOG)
    start = text.find("fn summaryOf(")
    stop = text.find("\n};", start)
    need(start >= 0 and stop >= 0, f"{CATALOG} has no summaryOf")
    codes = ["N%04d" % int(number) for number in re.findall(r"^    (\d+) =>", text[start:stop], re.M)]
    need(len(codes) >= 100, f"summaryOf ({CATALOG}) has {len(codes)} arms that could be read, and there are at least 100")
    need(len(set(codes)) == len(codes), f"summaryOf ({CATALOG}) has a code twice")
    return set(codes)


def lines_of(root):
    """{code: its line}, from the hand-written lines."""
    lines = {}
    for at, row in enumerate(read(root, LINES).strip("\n").split("\n"), 1):
        here = f"{LINES}, line {at}"
        cells = row.split("\t")
        need(len(cells) == 2 and re.fullmatch(r"N\d{4}", cells[0]), f"{here}: is not `N0000<tab>text`")
        code, text = cells
        need(code not in lines, f"{here}: {code} has a line already")
        need("|" not in text, f"{here}: a `|` would end the table's cell")
        need(not re.search(r"<[A-Za-z]|\{\{", re.sub(r"`[^`]*`", "", text)),
             f"{here}: `<tag` or `{{{{` outside a code span would be read as HTML or as a template")
        need(text.count("`") % 2 == 0, f"{here}: a code span is not closed")
        lines[code] = text
    return lines


def shown_in(root):
    """{code: the sections whose header names it, in the book's order: (number, page)}."""
    shown = {}
    for path in sorted(glob.glob(os.path.join(root, "book", "src", "ch[0-9][0-9]-[0-9][0-9]-*.md"))):
        page_id = os.path.basename(path)[:-3]
        page = read(root, f"book/src/{page_id}.md")
        head = re.match(r"^---\n(.*?)\n---\n", page, re.S)
        named = re.search(r"^diagnostics:\s*\[(.*?)\]\s*$", head.group(1), re.M) if head else None
        if not named:
            continue
        chapter, part = int(page_id[2:4]), int(page_id[5:7])
        number = f"{chapter}.{part}" if part else f"{chapter}"
        for code in sorted(set(re.findall(r"N\d{4}", named.group(1)))):
            shown.setdefault(code, []).append(((chapter, part), number, page_id))
    return {code: [(number, page_id) for _, number, page_id in sorted(found)] for code, found in shown.items()}


def anchor(slug):
    return SITE + slug.replace("–", "%E2%80%93").replace("§", "%C2%A7")


def names(codes):
    return " ".join(sorted(codes))


def render(root):
    """The page's text, without the note that it is generated."""
    catalog, lines, shown = catalog_codes(root), lines_of(root), shown_in(root)
    given = catalog - set(NOT_GIVEN)
    need(set(NOT_GIVEN) <= catalog, f"{CATALOG} no longer has {names(set(NOT_GIVEN) - catalog)}, which NOT_GIVEN"
                                    f" ({SCRIPT}) lists as unassigned or retired")
    need(given <= set(lines), f"{LINES} has no line for {names(given - set(lines))}, which {CATALOG} has: write one"
                              f" (or list the code in NOT_GIVEN, {SCRIPT})")
    need(set(lines) <= given, f"{LINES} has a line for {names(set(lines) - given)}, which {CATALOG} does not have"
                              " as a code that is given")
    need(set(shown) <= given, f"a chapter's header names {names(set(shown) - given)}, which {CATALOG} does not have"
                              " as a code that is given")
    intro = read(root, INTRO)
    out = [intro.rstrip("\n"), ""]
    placed = set()
    for title, numbers, slug, about in GROUPS:
        codes = sorted(c for c in given if int(c[1:]) in numbers)
        need(codes, f"the group '{title}' ({SCRIPT}) has no code in {CATALOG}")
        out += [f"## {title}", "", about, "", "| Code | Kind | What went wrong, and the usual fix | Shown in |",
                "|---|---|---|---|"]
        for code in codes:
            need(code not in placed, f"{code} is in two groups of GROUPS ({SCRIPT})")
            placed.add(code)
            kind = "warning" if code in WARNINGS else "error"
            where = ", ".join(f"[{number}](./{page_id})" for number, page_id in shown.get(code, []))
            if not where:
                # The specification's table of warnings begins with N0350, which is an error.
                where = f"[the specification]({anchor('warnings-n0400–n0499' if code == 'N0350' else slug)})"
            out.append(f"| {code} | {kind} | {lines[code]} | {where} |")
        out.append("")
    need(placed == given, f"no group of GROUPS ({SCRIPT}) holds {names(given - placed)}")
    out += ["## Codes that are not given", "",
            "The catalog has three codes that release 0.42.0 never reports. They are listed so that every code "
            "`ceesafec explain` knows is on this page. A code is never used again for another meaning.", "",
            "| Code | Status |", "|---|---|"]
    for code, status in sorted(NOT_GIVEN.items()):
        out.append(f"| {code} | {status} |")
    out.append("")
    page = "\n".join(out)
    errors, warnings = len(given - WARNINGS), len(given & WARNINGS)
    need(f"{errors} errors and {warnings} warnings" in intro,
         f"{INTRO} does not say '{errors} errors and {warnings} warnings', which is what the tables have")
    return page


if __name__ == "__main__":
    sys.exit(book_appendix_gen.run(sys.modules[__name__], __doc__))
