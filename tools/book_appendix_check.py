#!/usr/bin/env python3
"""The book's appendices say what the compiler's own lists say (Decision 0011 §6).

One function for each appendix; each returns its problems, in the form tools/book_check.py prints
(`book/src/<file>.md: …`). Every comparison is made in both directions and names the item that is missing or extra.
A list read from the compiler's source that comes back empty, or shorter than it can be, is a problem too
("not checked: …"): a check that compares a page with nothing must not pass.

  A  appendix-a-keywords       reserved words = keywordText (src/syntax/token.cee); contextual words = the parser's
                               nameIs(…, "X") less isPrimitiveTypeName, whose names the page also gives; attributes =
                               the lists the checker gives knownAttributes, with @from, and isAttributeWord's names
  B  appendix-b-operators      the spellings = tokenText, with `>>` and `>>=`; the assignment operators =
                               peekAssignOp; the precedence levels = peekBinaryOp and spec/grammar.md §2.1
  C  appendix-c-built-in-types the members = the ones the compiler lists for an unknown member (one `check`), the
                               number methods of src/check/io.cee, builtinName's functions, the structs of spec/api/
                               behind the built-in types, the tables of guide/builtins.md; the integers, the floats,
                               Box and the built-in functions hold nothing else, and no other part a member that
                               none of those lists, nor the specification's text, has
  D  appendix-d-derives        the derives = deriveRequest (src/check/derives.cee), in three tables; the attributes =
                               spec/type-system.md §28.1, each a literal of the compiler; the codes it quotes exist
  E  appendix-e-ceesafec       the commands = phaseOf (src/main.cee); the options = the option literals of the files
                               that read the command line, and of `--help` and `test --help`
  F  appendix-f-diagnostics    the codes = src/diagnostics/catalog.cee (which `explain` prints: three calls confirm
                               it); a row's links = the pages whose header names the code; a row with none links to
                               the specification; the count of errors and warnings the page states
  H  appendix-h-grammar        the productions = spec/grammar.md's blocks and spec/type-system.md §15.2's, line for
                               line; the precedence table = spec/grammar.md §2.1's
  I  appendix-i-for-ai-models  the MCP tools = toolName (src/tooling/mcp_server.cee); the commands and options of
                               "The tools a model has" are the compiler's
  J  appendix-j-versions       the rows = `git tag -l 'v*'` with their dates, newest first (left out where there are
                               no tags), and the newest is the compiler's version; an ADR "released in ceesafec X"
                               is in X's row; an ADR a row names is a file of spec/decisions/
  glossary                     the rows in order under their letter, none twice; a cross-reference has a row

usage (alone): python3 tools/book_appendix_check.py --ceesafec out/ceesafec [--root DIR]
"""
import argparse
import difflib
import glob
import json
import os
import re
import subprocess
import sys
import tempfile


class Shape(Exception):
    """A source or a page no longer has the shape a check reads: the check cannot be made."""


# ---------------------------------------------------------------------------------------------------- helpers

def read(root, path):
    try:
        with open(os.path.join(root, *path.split("/")), encoding="utf-8") as f:
            return f.read()
    except OSError:
        raise Shape(f"{path} cannot be read")


def between(text, start, end, path):
    """The text from `start` up to the next `end`."""
    at = text.find(start)
    if at < 0:
        raise Shape(f"{path} has no '{start}'")
    stop = text.find(end, at + len(start))
    if stop < 0:
        raise Shape(f"{path} has no '{end}' after '{start}'")
    return text[at:stop]


def at_least(items, least, what):
    """`items`, or Shape when there are fewer than the source can have."""
    if len(items) < least:
        raise Shape(f"{what}: found {len(items)}, and there are at least {least}"
                    " (the source's form has changed: correct tools/book_appendix_check.py)")
    return items


def section(page, heading):
    """A page's part from `## heading` to the next `## `."""
    start = page.find("\n## " + heading + "\n")
    if start < 0:
        raise Shape(f"the page has no part '## {heading}'")
    end = page.find("\n## ", start + 1)
    return page[start:end if end != -1 else len(page)]


def tables(text):
    """The tables of a text: each a list of rows after the header's two, each row its cells (`\\|` is a `|`)."""
    found, rows = [], []
    for line in text.split("\n") + [""]:
        if line.startswith("|"):
            rows.append([cell.strip().replace("\\|", "|") for cell in re.split(r"(?<!\\)\|", line)[1:-1]])
        elif rows:
            found.append(rows[2:])
            rows = []
    return found


def first_table(page, heading):
    found = tables(section(page, heading))
    if not found or not found[0]:
        raise Shape(f"the part '## {heading}' has no table with rows")
    return found[0]


def spans(cell):
    return re.findall(r"`([^`]+)`", cell)


def first_spans(rows, heading):
    """The first code span of each row's first cell."""
    out = []
    for row in rows:
        found = spans(row[0]) if row else []
        if not found:
            raise Shape(f"a row of '## {heading}' does not start with a code span: {' | '.join(row)[:60]}")
        out.append(found[0])
    return out


def both_ways(where, place, on_page, source_name, in_source, quote="'"):
    """The problems of two lists that must hold the same items."""
    problems = []
    page, source = set(on_page), set(in_source)
    for item in sorted(source - page):
        problems.append(f"{where}: {place} lacks {quote}{item}{quote}, which {source_name} has")
    for item in sorted(page - source):
        problems.append(f"{where}: {place} has {quote}{item}{quote}, which {source_name} does not have")
    for item in sorted({i for i in on_page if list(on_page).count(i) > 1}):
        problems.append(f"{where}: {place} has {quote}{item}{quote} twice")
    return problems


def catalog(root):
    """The diagnostic codes of the compiler: {"N0249": summary}, from summaryOf of src/diagnostics/catalog.cee."""
    path = "src/diagnostics/catalog.cee"
    body = between(read(root, path), "fn summaryOf(", "\n};", path)
    found = {}
    for number, summary in re.findall(r'^    (\d+) =>\s*"((?:[^"\\]|\\.)*)"', body, re.M):
        found["N%04d" % int(number)] = summary
    arms = len(re.findall(r"^    \d+ =>", body, re.M))
    if arms != len(found):
        raise Shape(f"{path}: summaryOf has {arms} arms and {len(found)} could be read")
    at_least(found, 100, f"the codes of summaryOf ({path})")
    return found


def commands_of(root):
    """The commands ceesafec runs: the names phaseOf knows (src/main.cee)."""
    path = "src/main.cee"
    body = between(read(root, path), "fn phaseOf(", "\n}\n", path)
    return at_least(set(re.findall(r'command == "([a-z-]+)"', body)), 10, f"the commands of phaseOf ({path})")


OPTION = r"--?[A-Za-z][A-Za-z0-9-]*"
OPTION_SOURCES = ("src/main.cee", "src/formatting/fmt_command.cee", "src/documentation/doc_generator.cee",
                  "src/packages/commands.cee")


def source_options(root):
    """Every string literal that is an option, in the files that read the command line."""
    found = set()
    for path in OPTION_SOURCES:
        here = {o.rstrip("=") for o in re.findall(r'"(' + OPTION + r'=?)"', read(root, path))}
        if not here:
            raise Shape(f"{path} has no option literal")
        found |= here
    return at_least(found, 30, "the option literals of the command line's sources")


def help_options(ceesafec):
    """Every option `ceesafec --help` and `ceesafec test --help` print at the start of a line."""
    found = set()
    for arguments in (["--help"], ["test", "--help"]):
        result = subprocess.run([ceesafec] + arguments, capture_output=True, text=True)
        here = set()
        for line in result.stdout.split("\n"):
            if re.match(r"\s+-", line):
                head = re.split(r"\s{2,}", line.strip())[0]
                here.update(re.findall(r"(?<![\w-])(" + OPTION + r")", head))
        if result.returncode != 0 or not here:
            raise Shape(f"`ceesafec {' '.join(arguments)}` prints no option")
        found |= here
    return at_least(found, 20, "the options of `ceesafec --help`")


def guarded(where, check):
    """A check's problems; a Shape is a problem of its own, so that no check passes by comparing nothing."""
    try:
        return check()
    except Shape as shape:
        return [f"{where}: not checked: {shape}"]


# ---------------------------------------------------------------------------------------------------- A

def appendix_a(root):
    where = "book/src/appendix-a-keywords.md"
    return guarded(where, lambda: _appendix_a(root, where))


def _appendix_a(root, where):
    page = read(root, where)
    token = read(root, "src/syntax/token.cee")
    parser = read(root, "src/syntax/parser.cee")
    problems = []

    keywords = re.findall(r'TokenKind\.\w+Keyword => "(\w+)"',
                          between(token, "fn keywordText(", "\n};", "src/syntax/token.cee"))
    at_least(keywords, 30, "the keywords of keywordText (src/syntax/token.cee)")
    problems += both_ways(where, "'Reserved words'", first_spans(first_table(page, "Reserved words"), "Reserved words"),
                          "keywordText (src/syntax/token.cee)", keywords)

    primitive = between(parser, "fn isPrimitiveTypeName(", ";\n", "src/syntax/parser.cee")
    primitives = at_least(set(re.findall(r'nameIs\(\s*\w+,\s*"(\w+)"', primitive)), 14,
                          "the names of isPrimitiveTypeName (src/syntax/parser.cee)")
    calls = len(re.findall(r"\bnameIs\(", parser)) - len(re.findall(r"fn nameIs\(", parser))
    literals = re.findall(r'\bnameIs\([^";]*"(\w+)"', parser)
    if calls != len(literals):
        raise Shape(f"src/syntax/parser.cee calls nameIs {calls} times, {len(literals)} of them with a literal")
    names = at_least(set(literals) - primitives, 3, "the contextual words of the parser's nameIs (src/syntax/parser.cee)")
    contextual = section(page, "Contextual words")
    problems += both_ways(where, "'Contextual words'",
                          first_spans(first_table(page, "Contextual words"), "Contextual words"),
                          "the parser's nameIs (src/syntax/parser.cee)", names)
    for name in sorted(primitives):
        if f"`{name}`" not in contextual:
            problems.append(f"{where}: 'Contextual words' does not name the primitive type '{name}' of"
                            " isPrimitiveTypeName (src/syntax/parser.cee)")

    if "from" not in names:
        raise Shape("the parser no longer reads '@from' with nameIs (src/syntax/parser.cee)")
    accepted = {"from"}
    lists = 0
    for path in sorted(glob.glob(os.path.join(root, "src", "check", "*.cee"))):
        text = read(root, "src/check/" + os.path.basename(path))
        short = "src/check/" + os.path.basename(path)
        for call in re.finditer(r"(?<!fn )\bknownAttributes\(", text):
            arguments = text[call.end():text.index(";", call.end())]
            # knownAttributes(c, attributes, place, names, mut known): `names` is a literal or the local `words`
            literal = re.match(r'\s*\w+,\s*[\w.]+,\s*"[^"]*",\s*"([^"]*)"', arguments)
            if literal:
                accepted.update(literal.group(1).split())
            elif re.match(r'\s*\w+,\s*[\w.]+,\s*"[^"]*",\s*words\b', arguments):
                before = re.findall(r'let words =\s*"([^"]*)"', text[:call.start()])
                if not before:
                    raise Shape(f"{short}: a knownAttributes call's `words` has no `let words = \"…\"` before it")
                accepted.update(before[-1].split())
            else:
                raise Shape(f"{short}: a knownAttributes call's names are neither a literal nor `words`")
            lists += 1
    at_least(range(lists), 8, "the knownAttributes calls of src/check/*.cee")
    at_least(accepted, 15, "the attributes the checker accepts (knownAttributes, src/check/*.cee)")
    listed = [name.lstrip("@") for name in first_spans(first_table(page, "Attributes"), "Attributes")]
    problems += both_ways(where, "'Attributes'", listed, "the checker's knownAttributes lists (src/check/*.cee)",
                          accepted, quote="@")
    word = between(read(root, "src/check/effects.cee"), "fn isAttributeWord(", ";\n", "src/check/effects.cee")
    words = at_least(set(re.findall(r'"(\w+)"', word)), 10, "the names of isAttributeWord (src/check/effects.cee)")
    for name in sorted(words - set(listed)):
        problems.append(f"{where}: 'Attributes' lacks @{name}, which isAttributeWord (src/check/effects.cee) has")
    return problems


# ---------------------------------------------------------------------------------------------------- B

def appendix_b(root):
    where = "book/src/appendix-b-operators.md"
    return guarded(where, lambda: _appendix_b(root, where))


def _appendix_b(root, where):
    page = read(root, where)
    parser = read(root, "src/syntax/parser.cee")
    grammar = read(root, "spec/grammar.md")
    problems = []

    token_text = between(read(root, "src/syntax/token.cee"), "fn tokenText(", "\n};", "src/syntax/token.cee")
    spelling_of = {kind: text.replace('\\"', '"')
                   for kind, text in re.findall(r'TokenKind\.(\w+) => "((?:[^"\\]|\\.)+)",', token_text)}
    arms = len(re.findall(r"^    TokenKind\.\w+ =>", token_text, re.M))
    if arms != len(spelling_of):
        raise Shape(f"src/syntax/token.cee: tokenText has {arms} arms and {len(spelling_of)} could be read")
    # The literal kinds and Identifier print their own names: they are not spellings.
    spellings = [s for s in spelling_of.values() if not re.match(r"^[A-Z]\w+$", s)]
    at_least(spellings, 40, "the spellings of tokenText (src/syntax/token.cee)")
    on_page = []
    for heading in ("Operators", "Assignment operators", "Punctuation and other symbols"):
        for row in first_table(page, heading):
            on_page.extend(spans(row[0]))
    # `>>` and `>>=` are no tokens: the parser forms them from adjacent `>` and `>`/`>=` (grammar.md §1.7).
    if "ShiftRight" not in parser:
        raise Shape("src/syntax/parser.cee no longer forms '>>' (ShiftRight)")
    problems += both_ways(where, "the first columns of 'Operators', 'Assignment operators' and 'Punctuation and other"
                          " symbols'", set(on_page), "tokenText (src/syntax/token.cee)", spellings + [">>", ">>="], "`")

    assign = between(parser, "fn peekAssignOp(", "\n    }\n", "src/syntax/parser.cee")
    kinds = at_least(re.findall(r"TokenKind\.(\w+) => AssignOp", assign), 8,
                     "the tokens of peekAssignOp (src/syntax/parser.cee)")
    if "AssignOp.ShiftRight" not in assign:
        raise Shape("peekAssignOp (src/syntax/parser.cee) no longer forms '>>='")
    for kind in kinds:
        if kind not in spelling_of:
            raise Shape(f"peekAssignOp's TokenKind.{kind} has no spelling in tokenText")
    problems += both_ways(where, "'Assignment operators'",
                          first_spans(first_table(page, "Assignment operators"), "Assignment operators"),
                          "peekAssignOp (src/syntax/parser.cee)", [spelling_of[k] for k in kinds] + [">>="], "`")

    binary = between(parser, "fn peekBinaryOp(", "\n    }\n", "src/syntax/parser.cee")
    comparison = re.search(r"fn comparisonPrecedence\(\) -> u32 => (\d+);", parser)
    shift = re.search(r"BinaryOp\.ShiftRight, precedence: (\d+)", binary)
    if not comparison or not shift or "BinaryOp.Greater, comparisonPrecedence()" not in binary:
        raise Shape("src/syntax/parser.cee: comparisonPrecedence() or peekBinaryOp's '>' and '>>' are not as they were")
    comparison = int(comparison.group(1))
    levels = {}
    for kind, level in re.findall(r"TokenKind\.(\w+) => binaryAhead\(BinaryOp\.\w+, (\d+|comparisonPrecedence\(\))\)",
                                  binary):
        if kind not in spelling_of:
            raise Shape(f"peekBinaryOp's TokenKind.{kind} has no spelling in tokenText")
        levels[spelling_of[kind]] = int(level) if level.isdigit() else comparison
    if len(levels) != len(re.findall(r"^            TokenKind\.\w+ =>", binary, re.M)):
        raise Shape("src/syntax/parser.cee: an arm of peekBinaryOp could not be read")
    at_least(levels, 15, "the binary operators of peekBinaryOp (src/syntax/parser.cee)")
    levels[">>"] = int(shift.group(1))
    levels[">"] = comparison
    prefix_level = max(levels.values()) + 1  # above every binary operator: `-`, `*` and `!` there are the prefix forms
    page_levels, page_rows = {}, {}
    for row in first_table(page, "Precedence"):
        if not row[0].isdigit() or len(row) < 2:
            raise Shape(f"a row of '## Precedence' does not start with a level: {' | '.join(row)[:60]}")
        page_rows[int(row[0])] = set(spans(row[1]))
        for op in spans(row[1]):
            if op in levels and int(row[0]) < prefix_level:
                if op in page_levels:
                    problems.append(f"{where}: 'Precedence' has `{op}` at levels {page_levels[op]} and {row[0]}")
                page_levels[op] = int(row[0])
    for op in sorted(levels):
        if op not in page_levels:
            problems.append(f"{where}: 'Precedence' lacks the binary operator `{op}`, which peekBinaryOp"
                            f" (src/syntax/parser.cee) has at level {levels[op]}")
        elif page_levels[op] != levels[op]:
            problems.append(f"{where}: 'Precedence' has `{op}` at level {page_levels[op]}, and peekBinaryOp"
                            f" (src/syntax/parser.cee) at level {levels[op]}")
    binary_rows = {op for level, ops in page_rows.items() if level < prefix_level for op in ops}
    for op in sorted(binary_rows - set(levels) - {"is"}):  # `is` is parsed apart, at the comparisons' level
        problems.append(f"{where}: 'Precedence' has `{op}` among the binary operators, which peekBinaryOp"
                        " (src/syntax/parser.cee) does not have")

    spec = between(grammar, "### 2.1 Expressions and precedence", "\n```", "spec/grammar.md")
    found = tables(spec)
    if not found:
        raise Shape("spec/grammar.md §2.1 has no precedence table")
    spec_rows = {}
    for row in found[0]:
        if not row[0].isdigit():
            raise Shape("spec/grammar.md §2.1: a row of the precedence table does not start with a level")
        spec_rows[int(row[0])] = {word for span in spans(row[1]) for word in span.split()}
    at_least(spec_rows, 12, "the precedence levels of spec/grammar.md §2.1")
    postfix = max(spec_rows)  # the postfix forms are examples (`f(x)`, `a[i]`), not spellings: Appendix H compares them
    for level in sorted(set(spec_rows) | set(page_rows)):
        if level == postfix:
            continue
        mine = {word for span in page_rows.get(level, set()) for word in span.split()}
        theirs = spec_rows.get(level, set())
        for op in sorted(theirs - mine):
            problems.append(f"{where}: 'Precedence', level {level}, lacks `{op}`, which spec/grammar.md §2.1 has there")
        for op in sorted(mine - theirs):
            problems.append(f"{where}: 'Precedence', level {level}, has `{op}`, which spec/grammar.md §2.1 does not"
                            " have there")
    return problems


# ---------------------------------------------------------------------------------------------------- C

def api_entries(root, path):
    """The entries of a page `ceesafec doc` wrote (spec/api/core.md): (type, member or None, signature)."""
    parts = re.split(r"^(##+ .*)$", read(root, path), flags=re.M)
    out, current = [], None
    for i in range(1, len(parts), 2):
        heading, body = parts[i], parts[i + 1]
        block = re.search(r"```ceesafe\n(.*?)```", body, re.S)
        signature = " ".join(block.group(1).split()) if block else ""
        if heading.startswith("## "):
            current = heading[3:].split()[-1]
            out.append((current, None, signature))
        elif heading.startswith("### "):
            name = heading[4:].strip()
            out.append((current, name.split(".", 1)[1] if "." in name else name, signature))
    return at_least(out, 50, f"the entries of {path}")


def members_of(cell):
    """The members a table's first cell names: `v.push(x: T)`, `Vector<T>.create()`, `v.length`, `IoError.NotFound`."""
    found = set()
    for code in spans(cell):
        found.update(re.findall(r"(?:^|[.\s])([A-Za-z][A-Za-z0-9]*)(?:<[^>]*>)?\(", code))
        field = re.fullmatch(r"[a-z]\.([a-z][A-Za-z]*)", code)
        if field:
            found.add(field.group(1))
        found.update(re.findall(r"\b(?:IoError|AllocError)\.([A-Z][A-Za-z]*)", code))
    return found - {"fn"}  # `fn(T) -> bool` is a parameter's type


# What the compiler says of an unknown member, and the parts of the page that must hold what it lists.
C_PROBES = [
    ("a vector's methods", "var v = Vector<i32>.create(); v.zzz();", ["`Vector<T>`", "Slices: `Slice<T>` and `SliceMut<T>`"]),
    ("Vector's functions", "var v = Vector<i32>.zzz();", ["`Vector<T>`"]),
    ("a string's methods", "var s = String.create(); s.zzz();", ["`String`", "`StringView`"]),
    ("String's functions", "var s = String.zzz();", ["`String`"]),
    ("StringView's functions", "let s = StringView.zzz();", ["`StringView`"]),
    ("a slice's methods", "let a = [1, 2]; a[..].zzz();", ["Slices: `Slice<T>` and `SliceMut<T>`"]),
    ("an optional's methods", "let o: i32? = null; o.zzz();", ["Optionals: `T?`"]),
    ("a result's methods", "let r: Result<i32, AllocError> = Ok(1); r.zzz();", ["`Result<T, E>`"]),
    ("Box's functions", "let b = Box.zzz(1);", ["`Box<T>`"]),
    ("a float type's functions", "let x = f64.zzz(1);", ["Floats"]),
]
C_WHOLE = ["TextSearch", "ByteSearch", "ResultMethods", "OptionalResults", "SliceSplitting", "SliceAlgorithms",
           "VectorEditing", "VectorCopying", "StringCopying", "VectorRaw", "VectorForeign", "HashMap", "OrderedMap"]
C_FAMILIES = [("NumberParsing", r"parse[UI]", ["parseInt", "parseIntRadix"]), ("NumberParsing", r"parseF", ["parseFloat"]),
              ("VectorBytes", r"push[UI]\d+BE", ["pushBE"]), ("VectorBytes", r"push[UI]\d+LE", ["pushLE"]),
              ("ByteOrder", r"read[UI]\d+BE", ["readBE"]), ("ByteOrder", r"read[UI]\d+LE", ["readLE"]),
              ("ByteOrder", r"write[UI]\d+BE", ["writeBE"]), ("ByteOrder", r"write[UI]\d+LE", ["writeLE"])]
C_GUIDE = [("`Console`", ["`Console`"]), ("`Process` and program arguments", ["`Process`"]),
           ("`StringView` and `String`", None), ("`Vector`, arrays and slices", None),
           ("Integers, floats and conversions", None)]
C_WHOLE_PARTS = ["Integers", "Floats", "`Box<T>`", "Built-in functions"]  # the compiler's lists are all these parts hold
C_ELSEWHERE ={"flush", "over"}  # guide/builtins.md has Console.flush in File's table; FixedBufferAllocator.over is no type of this page


def probe_members(ceesafec):
    """What the compiler lists for an unknown member of each built-in type: one `ceesafec check` of one file."""
    lines, source = {}, []
    for k, (what, body, _) in enumerate(C_PROBES):
        source += [f"fn probe{k}() {{", body, "}"]
        lines[len(source) - 1] = what
    source += ["fn main() {", "}"]
    with tempfile.TemporaryDirectory() as directory:
        with open(os.path.join(directory, "main.cee"), "w", encoding="utf-8") as f:
            f.write("\n".join(source) + "\n")
        result = subprocess.run([ceesafec, "check", "--diagnostics-format=json", "main.cee"], cwd=directory,
                                capture_output=True, text=True)
    try:
        document = json.loads(result.stdout)
    except ValueError:
        raise Shape("`ceesafec check --diagnostics-format=json` of the probe program printed no JSON document")
    listed = {}
    for diagnostic in document.get("diagnostics", []):
        what = lines.get(diagnostic.get("start", {}).get("line"))
        message = diagnostic.get("message", "")
        if what is None or "'zzz'" not in message:
            continue
        names = [re.sub(r"\(.*", "", quoted).split(".")[-1] for quoted in re.findall(r"'([^']+)'", message.split("'zzz'", 1)[1])
                 if " " not in quoted.split("(")[0]]
        # 'match', '?' and '!' in a message are syntax, not members
        listed[what] = [n for n in names if re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", n) and n != "match"]
    for what, _, _ in C_PROBES:
        if not listed.get(what):
            raise Shape(f"the compiler lists no member for {what} (an unknown member 'zzz' in a probe program)")
    return listed


def appendix_c(root, ceesafec):
    where = "book/src/appendix-c-built-in-types.md"
    return guarded(where, lambda: _appendix_c(root, ceesafec, where))


def _appendix_c(root, ceesafec, where):
    page = read(root, where)
    by_part, heading = {}, None
    for line in page.split("\n"):
        if line.startswith("## "):
            heading = line[3:].strip()
        elif line.startswith("| ") and heading and not re.match(r"\| (Member|Form|Function|Type|Types) \|", line):
            by_part.setdefault(heading, set()).update(members_of(line.split(" | ")[0][2:]))
    in_page = set().union(*by_part.values()) if by_part else set()
    at_least(in_page, 50, "the members the page's tables name")
    problems, sources = [], set()
    whole = {part: set() for part in C_WHOLE_PARTS}
    origin = {}

    def need(names, source, parts=None, all_of_it=False):
        """`names` are in the page (in `parts` of it); `all_of_it`: with the part's other such lists, they are all
        the part holds."""
        if not names:
            raise Shape(f"{source} gives no member")
        have = in_page
        if parts:
            for part in parts:
                if part not in by_part:
                    raise Shape(f"the page has no table under '## {part}'")
            have = set().union(*(by_part[part] for part in parts))
        place = " or ".join(f"'{part}'" for part in parts) if parts else "the page"
        for name in names:
            sources.add(name)
            if name not in have:
                problems.append(f"{where}: {place} lacks the member '{name}', which {source} has")
        if all_of_it:
            whole[parts[0]].update(names)
            origin.setdefault(parts[0], []).append(source)

    # 1. The compiler's own lists.
    listed = probe_members(ceesafec)
    for what, _, parts in C_PROBES:
        need(listed[what], f"the compiler's message for {what}", parts, all_of_it=parts[0] in C_WHOLE_PARTS)
    # 2. The number methods: the names the checker resolves on an integer and on a float. (The lists of
    # src/check/suggest.cee, for "did you mean", are copies of the first and the third.)
    numbers = read(root, "src/check/io.cee")
    for function, part, least in (("integerMethod", "Integers", 10), ("integerBitMethod", "Integers", 5),
                                  ("floatMethod", "Floats", 5)):
        body = between(numbers, f"fn {function}(", "\n}\n", "src/check/io.cee")
        names = at_least(re.findall(r'sameText\(name, "([A-Za-z]+)"\)', body), least,
                         f"the names of {function} (src/check/io.cee)")
        need(names, f"{function} (src/check/io.cee)", [part], all_of_it=True)
    # 3. The built-in functions.
    builtin = between(read(root, "src/check/declare.cee"), "fn builtinName(", "\n};", "src/check/declare.cee")
    names = re.findall(r'=> "([A-Za-z]+)"', builtin)
    at_least(names, 30, "the names of builtinName (src/check/declare.cee)")
    need(at_least([n for n in names if n[0].islower()], 10, "the functions of builtinName (src/check/declare.cee)"),
         "builtinName (src/check/declare.cee)", ["Built-in functions"], all_of_it=True)
    # 4. The library structs that hold the built-in types' methods.
    core = api_entries(root, "spec/api/core.md")
    std = api_entries(root, "spec/api/std.md")
    for struct in C_WHOLE:
        members = [m for s, m, _ in core + std if s == struct and m and not m.endswith("Failing")]
        need(members, f"{struct} (spec/api)")
    formatted = [signature for struct, member, signature in core if struct == "AppendFormatted" and member is None]
    need([n for signature in formatted for n in re.findall(r"fn ([A-Za-z]+)\(", signature)],
         "AppendFormatted (spec/api/core.md)")
    for struct, pattern, methods in C_FAMILIES:
        if not any(s == struct and m and re.match(pattern, m) for s, m, _ in core):
            raise Shape(f"{struct} has no member {pattern} in spec/api/core.md: the page's {methods} have lost their source")
        need(methods, f"{struct} (spec/api/core.md)")
    # 5. The tables of the guide's page of built-in types.
    guide = read(root, "guide/builtins.md")
    for heading, parts in C_GUIDE:
        body = section("\n" + guide, heading)
        names = []
        for line in body.split("\n"):
            if line.startswith("| ") and not re.match(r"\| (Member|Function|Operation) ", line) and "enum E" not in line:
                cell = line.split(" | ")[0]
                names += [n for code in spans(cell)
                          for n in re.findall(r"(?:^|[.\s])([A-Za-z][A-Za-z0-9]*)(?:<[^>]*>)?\(", code)]
        need([n for n in names if n not in C_ELSEWHERE], f"guide/builtins.md, '{heading}'", parts)

    # Nothing invented. Where the compiler's lists are the whole of a part, the part has nothing else.
    for part in C_WHOLE_PARTS:
        if not whole[part]:
            raise Shape(f"no list of the compiler was compared with the page's part '## {part}'")
        for name in sorted(by_part.get(part, set()) - whole[part]):
            problems.append(f"{where}: '{part}' has the member '{name}', which {' and '.join(origin[part])}"
                            f" {'do' if len(origin[part]) > 1 else 'does'} not have")
    # Elsewhere, a member the page names is in one of the lists above, or is a member in the specification's text.
    reference = read(root, "spec/type-system.md") + guide + read(root, "spec/api/core.md") + read(root, "spec/api/std.md")
    for name in sorted(in_page - sources):
        if not re.search(r"(\.|`|\b)" + name + r"(<[^>\n]*>)?(\(|`|\b)", reference):
            problems.append(f"{where}: names the member '{name}', which no list of the compiler, spec/type-system.md,"
                            " spec/api/ or guide/builtins.md has")
    return problems


# ---------------------------------------------------------------------------------------------------- D

def appendix_d(root):
    where = "book/src/appendix-d-derives.md"
    return guarded(where, lambda: _appendix_d(root, where))


def _appendix_d(root, where):
    page = read(root, where)
    problems = []
    request = between(read(root, "src/check/derives.cee"), "fn deriveRequest(", "\nstruct ", "src/check/derives.cee")
    names = at_least(set(re.findall(r'sameText\(\s*text,\s*"([A-Za-z]+)"', request)), 3,
                     "the derives of deriveRequest (src/check/derives.cee)")
    source = "deriveRequest (src/check/derives.cee)"

    listed = [name.strip() for row in first_table(page, "The derives")
              for span in spans(row[0])[:1] for name in re.findall(r"@derive\(([A-Za-z, ]+)\)", span)
              for name in name.split(",")]
    problems += both_ways(where, "'The derives'", set(listed), source, names)
    written = first_spans(first_table(page, "What each derive writes"), "What each derive writes")
    problems += both_ways(where, "'What each derive writes'", written, source, names)
    header = re.search(r"^\| A field or payload of type \|(.*)\|$", section(page, "What a field's type must have"), re.M)
    if not header:
        raise Shape("'## What a field's type must have' has no table that starts '| A field or payload of type |'")
    problems += both_ways(where, "the columns of 'What a field's type must have'", spans(header.group(1)), source, names)

    spec = read(root, "spec/type-system.md")
    table = between(spec, "### 28.1 Attributes", "\n### 28.2", "spec/type-system.md")
    spec_names = set()
    for row in re.findall(r"^\| (`@.*?) \|", table, re.M):
        spec_names.update(re.findall(r"`@([A-Za-z]+)", row))
    at_least(spec_names, 10, "the attributes of spec/type-system.md §28.1")
    for name in ("threadSafe", "interiorMutable"):  # §38.3 and §41.1 give these two, outside §28.1's table
        if "@" + name not in spec:
            raise Shape(f"spec/type-system.md no longer has @{name}")
        spec_names.add(name)
    page_names = {"derive"}
    for row in first_table(page, "The other attributes"):
        page_names.update(re.findall(r"`@([A-Za-z]+)", row[0]))
    problems += both_ways(where, "'The other attributes' (with @derive)", page_names,
                          "spec/type-system.md §28.1 (with §38.3 and §41.1)", spec_names, quote="@")
    compiler = ""
    for directory in ("check", "syntax"):
        for path in sorted(glob.glob(os.path.join(root, "src", directory, "*.cee"))):
            compiler += read(root, f"src/{directory}/{os.path.basename(path)}")
    if len(compiler) < 100000:
        raise Shape("src/check/ and src/syntax/ could not be read")
    for name in sorted(page_names):
        if f'"{name}"' not in compiler:
            problems.append(f"{where}: 'The other attributes' has @{name}, which is no string literal of src/check/"
                            " or src/syntax/")
    codes = catalog(root)
    for code in sorted(set(re.findall(r"\bN[0-9]{4}\b", page))):
        if codes.get(code, "*(unassigned)*") == "*(unassigned)*":
            problems.append(f"{where}: names {code}, which is no code of src/diagnostics/catalog.cee")
    return problems


# ---------------------------------------------------------------------------------------------------- E

def appendix_e(root, ceesafec):
    where = "book/src/appendix-e-ceesafec.md"
    return guarded(where, lambda: _appendix_e(root, ceesafec, where))


def _appendix_e(root, ceesafec, where):
    page = read(root, where)
    problems = []
    commands = commands_of(root)
    headed = set()
    for heading in re.findall(r"^## (.*)$", page, re.M):
        headed.update(re.findall(r"`ceesafec ([a-z-]+)`", heading))
    problems += both_ways(where, "the headings '## `ceesafec <command>`'", headed, "phaseOf (src/main.cee)", commands)
    in_table = re.findall(r"^\| `ceesafec ([a-z-]+)", section(page, "The command line"), re.M)
    problems += both_ways(where, "the table of 'The command line'", in_table, "phaseOf (src/main.cee)", commands)

    in_source, in_help = source_options(root), help_options(ceesafec)
    on_page = set(re.findall(r"^\| `(" + OPTION + r")` \|", page, re.M))
    for option in sorted((in_source | in_help) - on_page):
        origin = "the command line's sources" if option in in_source else "`ceesafec --help`"
        problems.append(f"{where}: no table has a row for `{option}`, which {origin} "
                        f"{'have' if option in in_source else 'has'}")
    for option in sorted(on_page - in_source - in_help):
        problems.append(f"{where}: has a row for `{option}`, which neither the command line's sources"
                        f" ({', '.join(OPTION_SOURCES)}) nor `ceesafec --help` has")
    return problems


# ---------------------------------------------------------------------------------------------------- F

def header_diagnostics(root, written):
    """{code: the ids of the written pages whose header names it}."""
    named = {}
    for file in written:
        try:
            text = read(root, f"book/src/{file}.md")
        except Shape:
            continue
        header = re.match(r"---\n(.*?)\n---\n", text, re.S)
        line = re.search(r"^diagnostics:\s*\[(.*?)\]\s*$", header.group(1), re.M) if header else None
        for code in re.findall(r"N\d{4}", line.group(1)) if line else []:
            named.setdefault(code, set()).add(file)
    return named


def written_ids(root):
    return sorted(os.path.basename(path)[:-3] for path in glob.glob(os.path.join(root, "book", "src", "*.md")))


SPECIFICATION = "https://ceesafe.github.io/CeeSafe/reference/diagnostics#"


def appendix_f(root, ceesafec, written=None):
    where = "book/src/appendix-f-diagnostics.md"
    return guarded(where, lambda: _appendix_f(root, ceesafec, written, where))


def _appendix_f(root, ceesafec, written, where):
    page = read(root, where)
    problems = []
    codes = catalog(root)
    unused = {code for code, summary in codes.items() if summary.startswith("*")}  # "*(unassigned)*", "*Retired in …"
    # `ceesafec explain` prints the catalog (the same summaryOf): its first and last codes are explained, and a number
    # the catalog does not have is not. One process for each of the 162 codes would say no more, and take a minute.
    absent = next("N%04d" % n for n in range(1, 10000) if "N%04d" % n not in codes)
    for code, known in ((min(codes), True), (max(codes), True), (absent, False)):
        result = subprocess.run([ceesafec, "explain", code], capture_output=True, text=True)
        if (result.returncode == 0 and result.stdout.startswith(code + ":")) != known:
            problems.append(f"{where}: `ceesafec explain {code}` {'does not explain' if known else 'explains'} a code"
                            f" that src/diagnostics/catalog.cee {'has' if known else 'does not have'}: the catalog is"
                            " not what this compiler explains")

    headers = header_diagnostics(root, [f for f in (written or written_ids(root)) if f != "appendix-f-diagnostics"])
    if not headers:
        raise Shape("no page of the book names a diagnostic in its header")
    rows, kinds = [], {}
    for line in page.split("\n"):
        row = re.match(r"^\| (N\d{4}) \| (.*) \|$", line)
        if not row:
            continue
        code, cells = row.group(1), row.group(2).split(" | ")
        rows.append(code)
        if len(cells) == 1:
            if code not in unused and code in codes:
                problems.append(f"{where}: {code} has a row without a kind and a section, and"
                                " src/diagnostics/catalog.cee has it as a code in use")
            continue
        if len(cells) != 3:
            problems.append(f"{where}: {code}'s row has {len(cells) + 1} cells, not Code, Kind, What went wrong, Shown in")
            continue
        kind, meaning, shown = cells
        kinds[code] = kind
        if kind not in ("error", "warning"):
            problems.append(f"{where}: {code}'s kind is '{kind}', not error or warning")
        if code in unused:
            problems.append(f"{where}: {code} has a row as a code in use, and src/diagnostics/catalog.cee has it"
                            " unassigned or retired")
        if len(meaning) < 40:
            problems.append(f"{where}: {code}'s row does not say what went wrong and the usual fix")
        links = set(re.findall(r"\]\(\./([A-Za-z0-9_-]+)(?:#[^)]*)?\)", shown))
        naming = headers.get(code, set())
        # A section of a chapter that shows the code must be linked; an appendix that names it may be.
        for file in sorted({f for f in naming if f.startswith("ch")} - links):
            problems.append(f"{where}: {code}'s row does not link to './{file}', whose header names {code}")
        for file in sorted(links - naming):
            problems.append(f"{where}: {code}'s row links to './{file}', whose header does not name {code}")
        if not links and SPECIFICATION not in shown:
            problems.append(f"{where}: {code}'s row links to no section and not to the specification ({SPECIFICATION}…)")
    problems += both_ways(where, "the tables", rows, "summaryOf (src/diagnostics/catalog.cee)", codes, quote="")
    stated = re.search(r"(\d+) errors and (\d+) warnings", page)
    if not stated:
        problems.append(f"{where}: does not state how many codes there are ('N errors and M warnings')")
    else:
        errors = sum(1 for kind in kinds.values() if kind == "error")
        warnings = sum(1 for kind in kinds.values() if kind == "warning")
        if (int(stated.group(1)), int(stated.group(2))) != (errors, warnings):
            problems.append(f"{where}: says '{stated.group(0)}', and its tables have {errors} errors and {warnings}"
                            " warnings")
    return problems


# ---------------------------------------------------------------------------------------------------- H

def fenced(lines, opening):
    """The fenced blocks that `opening` opens, each its lines that are not blank."""
    out, i = [], 0
    while i < len(lines):
        fence = lines[i].strip()
        if not fence.startswith("```"):
            i += 1
            continue
        end = i + 1
        while end < len(lines) and lines[end].strip() != "```":
            end += 1
        if fence == opening:
            out.append([line for line in lines[i + 1:end] if line.strip()])
        i = end + 1
    return out


def level_table(lines, path):
    first = next((i for i, line in enumerate(lines) if line.startswith("| Level |")), None)
    if first is None:
        raise Shape(f"{path} has no table that starts '| Level |'")
    last = first
    while last < len(lines) and lines[last].startswith("|"):
        last += 1
    return at_least(lines[first:last], 10, f"the rows of the precedence table of {path}")


def differences(where, what, mine, theirs, source):
    """The lines only one of two sequences has, at most six of them, as problems."""
    problems = []
    for line in difflib.ndiff(theirs, mine):
        if line.startswith("- "):
            problems.append(f"{where}: {what} lacks the line '{line[2:].strip()}' of {source}")
        elif line.startswith("+ "):
            problems.append(f"{where}: {what} has the line '{line[2:].strip()}', which {source} does not have there")
    if len(problems) > 6:
        problems = problems[:6] + [f"{where}: {what}: {len(problems) - 6} more lines differ from {source}"]
    return problems


def appendix_h(root):
    where = "book/src/appendix-h-grammar.md"
    return guarded(where, lambda: _appendix_h(root, where))


def _appendix_h(root, where):
    page = read(root, where).split("\n")
    grammar = read(root, "spec/grammar.md").split("\n")
    types = read(root, "spec/type-system.md").split("\n")
    mine = fenced(page, "```text")
    if len(mine) < 2:
        raise Shape("the page has fewer than two `text` blocks")
    theirs = at_least([line for block in fenced(grammar, "```") for line in block], 100,
                      "the lines of spec/grammar.md's productions (its unlabelled fenced blocks)")
    start = next((i for i, line in enumerate(types) if line.startswith("### 15.2")), None)
    if start is None:
        raise Shape("spec/type-system.md has no '### 15.2'")
    stop = next((i for i in range(start + 1, len(types)) if types[i].startswith("#")), len(types))
    patterns = fenced(types[start:stop], "```text")
    if not patterns:
        raise Shape("spec/type-system.md §15.2 has no `text` block")
    at_least(patterns[0], 10, "the lines of the pattern grammar of spec/type-system.md §15.2")
    problems = differences(where, "the productions", [line for block in mine[:-1] for line in block], theirs,
                           "spec/grammar.md")
    problems += differences(where, "the patterns (the last `text` block)", mine[-1], patterns[0],
                            "spec/type-system.md §15.2")
    problems += differences(where, "the precedence table", level_table(page, where), level_table(grammar, "spec/grammar.md"),
                            "spec/grammar.md §2.1")
    return problems


# ---------------------------------------------------------------------------------------------------- I

def appendix_i(root):
    where = "book/src/appendix-i-for-ai-models.md"
    return guarded(where, lambda: _appendix_i(root, where))


def _appendix_i(root, where):
    page = read(root, where)
    problems = []
    server = read(root, "src/tooling/mcp_server.cee")
    tools = re.findall(r'=> "(ceesafe_[a-z_]+)"', between(server, "fn toolName(", "\n};", "src/tooling/mcp_server.cee"))
    count = re.search(r"fn mcpToolCount\(\) -> usize => (\d+);", server)
    if not count or int(count.group(1)) != len(tools) or len(tools) < 5:
        raise Shape(f"src/tooling/mcp_server.cee: toolName gives {len(tools)} tools and mcpToolCount() is"
                    f" {count.group(1) if count else 'not found'}")
    rows = re.findall(r"^\| `(ceesafe_[a-z_]+)` \|", page, re.M)
    header = re.search(r"^\| MCP tool \|", page, re.M)
    if not header:
        raise Shape("the page has no table that starts '| MCP tool |'")
    problems += both_ways(where, "the table of MCP tools", rows, "toolName (src/tooling/mcp_server.cee)", tools, "`")

    commands, options = commands_of(root), source_options(root)
    for row in first_table(page, "The tools a model has"):
        for span in spans(row[0]):
            for command in re.findall(r"^ceesafec ([a-z-]+)", span):
                if command not in commands:
                    problems.append(f"{where}: 'The tools a model has' has `ceesafec {command}`, which is no command"
                                    " of phaseOf (src/main.cee)")
            for option in re.findall(r"(?<![\w-])(" + OPTION + r")", span):
                if option not in options:
                    problems.append(f"{where}: 'The tools a model has' has `{option}`, which is no option literal of"
                                    " the command line's sources")
    return problems


# ---------------------------------------------------------------------------------------------------- J

def adrs_named(text):
    """The ADR numbers a text names: 'ADR 0086', 'ADRs 0105 to 0122' (or '0066-0068'), '(0105, 0107)'."""
    found = set()
    for a, b in re.findall(r"ADRs ([0-9]{4})(?: to |[-–])([0-9]{4})", text):
        found.update(range(int(a), int(b) + 1))
    for n in re.findall(r"ADR ([0-9]{4})", text):
        found.add(int(n))
    for group in re.findall(r"\(([0-9]{4}(?:, [0-9]{4})*)\)", text):
        found.update(int(n) for n in group.split(", "))
    return found


def release_tags(root):
    """The release tags, newest first: (version, date, the tagged commit's subject). Empty where git has none."""
    try:
        result = subprocess.run(
            ["git", "for-each-ref", "--sort=-v:refname",
             "--format=%(refname:short)%09%(creatordate:short)%09%(subject)%09%(*subject)", "refs/tags/v*"],
            cwd=root, capture_output=True, text=True)
    except OSError:
        return []
    if result.returncode != 0:
        return []
    tags = []
    for line in result.stdout.split("\n"):
        fields = line.split("\t")
        if len(fields) == 4 and re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", fields[0]):
            tags.append((fields[0][1:], fields[1], fields[3] or fields[2]))
    return tags


def compiler_version(root):
    path = "src/backend/c_program.cee"
    found = re.search(r'fn ceesafeVersion\(\) -> StringView => "([0-9]+\.[0-9]+\.[0-9]+)[^"]*";', read(root, path))
    if not found:
        raise Shape(f"{path} has no ceesafeVersion() to read the version from")
    return found.group(1)


def appendix_j(root, tags=None):
    """`tags` is release_tags(root) unless given; an empty list leaves the comparison with the tags out."""
    where = "book/src/appendix-j-versions.md"
    return guarded(where, lambda: _appendix_j(root, release_tags(root) if tags is None else tags, where))


def _appendix_j(root, tags, where):
    page = read(root, where)
    problems = []
    rows = re.findall(r"^\| ([0-9]+\.[0-9]+\.[0-9]+) \| ([0-9-]{10}) \| (.*?) \| (.*?) \|$", page, re.M)
    at_least(rows, 1, "the page's rows '| release | date | added | sections |'")
    releases = [version for version, _, _, _ in rows]
    key = lambda version: tuple(int(part) for part in version.split("."))
    for version in sorted({v for v in releases if releases.count(v) > 1}):
        problems.append(f"{where}: release {version} has two rows")
    for a, b in zip(releases, releases[1:]):
        if key(a) < key(b):
            problems.append(f"{where}: release {b} comes after {a}: the rows are newest first")
    current = compiler_version(root)
    if releases[0] != current:
        problems.append(f"{where}: its newest row is {releases[0]}, and the compiler is {current}"
                        " (ceesafeVersion, src/backend/c_program.cee)")
    if tags:
        dates = {version: date for version, date, _ in tags}
        # A release is written before it is tagged: the compiler's own version may have a row and no tag yet.
        for version in sorted(set(dates) - set(releases), key=key):
            problems.append(f"{where}: has no row for release {version}, which has the tag v{version}")
        for version in sorted(set(releases) - set(dates) - {current}, key=key):
            problems.append(f"{where}: has a row for release {version}, which has no tag v{version}")
        for version, date, _, _ in rows:
            if version in dates and dates[version] != date:
                problems.append(f"{where}: release {version} has the date {date}, and its tag v{version} {dates[version]}")

    by_release = {}
    for version, _, text, _ in rows:
        by_release.setdefault(version, set()).update(adrs_named(text))
    files = {}
    for path in glob.glob(os.path.join(root, "spec", "decisions", "[0-9][0-9][0-9][0-9]-*.md")):
        files[int(os.path.basename(path)[:4])] = "spec/decisions/" + os.path.basename(path)
    at_least(files, 50, "the ADRs of spec/decisions/")
    said = 0
    for number, path in sorted(files.items()):
        for release in sorted(set(re.findall(r"released in\s+ceesafec\s+([0-9]+\.[0-9]+\.[0-9]+)", read(root, path)[:1500]))):
            said += 1
            if release not in by_release:
                problems.append(f"{where}: has no row for release {release}, in which {path} says it was released")
            elif number not in by_release[release]:
                problems.append(f"{where}: the row of {release} does not name ADR {number:04d}, which says it was"
                                f" released in ceesafec {release} ({path})")
    at_least(range(said), 10, "the ADRs whose status says 'released in ceesafec X' (spec/decisions/)")
    # 'ADRs 0105 to 0122' may span a number that was never given: its two ends are files, and so is every ADR named alone.
    for version, _, text, _ in rows:
        spanned = set()
        for a, b in re.findall(r"ADRs ([0-9]{4})(?: to |[-–])([0-9]{4})", text):
            spanned.update(range(int(a) + 1, int(b)))
        alone = adrs_named(re.sub(r"ADRs ([0-9]{4})(?: to |[-–])([0-9]{4})", r"ADR \1, ADR \2", text))
        for n in sorted((by_release[version] - spanned | alone) - set(files)):
            problems.append(f"{where}: the row of {version} names ADR {n:04d}, which is no file of spec/decisions/")
    for version, _, subject in tags:
        if subject.startswith("release:"):
            for n in sorted(adrs_named(subject) & set(files)):
                if version in by_release and n not in by_release[version]:
                    problems.append(f"{where}: the row of {version} does not name ADR {n:04d}, which the release's"
                                    f" commit names ('{subject[:70]}')")
    return problems


# ---------------------------------------------------------------------------------------------------- glossary

def glossary(root):
    where = "book/src/glossary.md"
    return guarded(where, lambda: _glossary(root, where))


def _glossary(root, where):
    page = read(root, where)
    problems = []
    clean = lambda text: re.sub(r"[^a-z0-9 ]", "", text.lower()).strip()
    terms, letter = [], ""
    for line in page.split("\n"):
        heading = re.match(r"## ([A-Z])$", line)
        if heading:
            if letter and heading.group(1) <= letter:
                problems.append(f"{where}: the heading '{heading.group(1)}' comes after '{letter}'")
            letter = heading.group(1)
            continue
        row = re.match(r"\| (.+?) \| (.+) \| (.+?) \|$", line)
        if not row or row.group(1) == "Term" or set(row.group(1)) <= set("-: "):
            continue
        term, key = row.group(1), clean(row.group(1))
        if not key:
            problems.append(f"{where}: the term '{term}' has no letter or digit to order it by")
            continue
        if key[0].upper() != letter:
            problems.append(f"{where}: '{term}' is under the heading '{letter}'")
        terms.append((key, term, row.group(2)))
    at_least(terms, 50, "the page's rows '| term | meaning | section |'")
    keys = [key for key, _, _ in terms]
    for (a, first), (b, second) in zip([(k, t) for k, t, _ in terms], [(k, t) for k, t, _ in terms][1:]):
        if a > b:
            problems.append(f"{where}: '{first}' comes before '{second}': the rows are in alphabetical order")
    for key, term, _ in terms:
        if keys.count(key) > 1 and keys.index(key) == [t for _, t, _ in terms].index(term):
            problems.append(f"{where}: '{term}' has two rows")
    plain = set(keys)
    for _, term, _ in terms:
        for part in re.sub(r"\s*\(.*\)$", "", term).split(", "):
            plain.add(clean(part))
    for _, term, meaning in terms:
        for target in re.findall(r"(?<![`*\w])\*([A-Za-z`][^*]*)\*", meaning):
            if clean(target) not in plain and clean(target).rstrip("s") not in plain:
                problems.append(f"{where}: '{term}' points to *{target}*, which has no row")
    return problems


# ---------------------------------------------------------------------------------------------------- all

APPENDICES = [
    ("appendix-a-keywords", "A", lambda root, cee, written: appendix_a(root)),
    ("appendix-b-operators", "B", lambda root, cee, written: appendix_b(root)),
    ("appendix-c-built-in-types", "C", lambda root, cee, written: appendix_c(root, cee)),
    ("appendix-d-derives", "D", lambda root, cee, written: appendix_d(root)),
    ("appendix-e-ceesafec", "E", lambda root, cee, written: appendix_e(root, cee)),
    ("appendix-f-diagnostics", "F", lambda root, cee, written: appendix_f(root, cee, written)),
    ("appendix-h-grammar", "H", lambda root, cee, written: appendix_h(root)),
    ("appendix-i-for-ai-models", "I", lambda root, cee, written: appendix_i(root)),
    ("appendix-j-versions", "J", lambda root, cee, written: appendix_j(root)),
    ("glossary", "glossary", lambda root, cee, written: glossary(root)),
]


def check(root, ceesafec, written=None):
    """The problems of the appendices that are written, and what was checked, for book_check's last line:
    "appendices A B C D E F H I J and the glossary against the compiler"."""
    written = list(written) if written is not None else written_ids(root)
    problems, checked = [], []
    for file, name, run in APPENDICES:
        if file not in written or not os.path.exists(os.path.join(root, "book", "src", file + ".md")):
            continue  # a planned page
        checked.append(name)
        problems += run(root, ceesafec, written)
    letters = [name for name in checked if name != "glossary"]
    said = []
    if letters:
        said.append(("appendices " if len(letters) > 1 else "appendix ") + " ".join(letters))
    if "glossary" in checked:
        said.append("the glossary")
    summary = (" and ".join(said) + " against the compiler") if said else "no appendix written"
    if "J" in checked and not release_tags(root):
        summary += " (no git tags here: J's releases not compared with them)"
    return problems, summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ceesafec", required=True)
    parser.add_argument("--root", default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
    args = parser.parse_args()
    problems, summary = check(os.path.abspath(args.root), os.path.abspath(args.ceesafec))
    for problem in problems:
        print("FAIL " + problem)
    print(f"book_appendix_check: {summary}; {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
