#!/usr/bin/env python3
"""cbind: an `extern "C"` block for a C header, written from clang's AST (ADR 0095).

usage: python3 tools/cbind.py --header kernel/handle.h -I kernel/include [-D NAME[=VALUE] ...] [--target TRIPLE]
           [--config NAME="clang flags" ...] [--only NAME,NAME ...] [--with FILE.cee ...] [--leave NAME,NAME ...] [--no-fmt]
           -o handle.cee
       python3 tools/cbind.py --header kernel/handle.h --header kernel/socket.h ... --out-dir kernel/bind

Several headers are written as one file each. The files compile as one unit, where a name is declared once, so a
type is declared by the header that defines it and is left out of the others; a type none of them defines stays
opaque in the first that mentions it.

The header is named as C code includes it and found through the -I directories. Nothing is compiled or run: clang
parses a file that includes the header, so a header for a target the host cannot execute is read like any other.

What is written (ADR 0095 §2.2): the header's own functions, structs, enums, typedefs of structs and of function
pointers, object-like macros that are integer constant expressions or string literals, and `extern` data. A
declaration that CeeSafe cannot spell is left out, or for a struct made opaque, with a comment that says why: the tool
never guesses. With several --config, only what is identical in all of them is written, and the rest is listed.

The output is formatted by ceesafec ($CEESAFEC, else `out/ceesafec` of this repository, else `ceesafec` on PATH), so
that it is the same text wherever it is made; --no-fmt writes the tool's own layout.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CEESAFE = Path(__file__).resolve().parents[1]

# C types and their CeeSafe spelling (type-system.md §28.3), as integrations/cosmoos/c2cee.py maps them.
SCALARS = {
    "_Bool": "bool", "bool": "bool", "void": "void",
    "char": "CChar", "signed char": "i8", "unsigned char": "u8",
    "short": "i16", "unsigned short": "u16", "int": "i32", "unsigned int": "u32",
    "long": "i64", "unsigned long": "usize", "long long": "i64", "unsigned long long": "u64",
    "float": "f32", "double": "f64",
}
NAMED = {
    "int8_t": "i8", "uint8_t": "u8", "int16_t": "i16", "uint16_t": "u16", "int32_t": "i32", "uint32_t": "u32",
    "int64_t": "i64", "uint64_t": "u64", "size_t": "usize", "uintptr_t": "usize", "intptr_t": "isize",
    "ptrdiff_t": "isize", "ssize_t": "isize", "unsigned": "u32",
}
ATOMICS = {"u8": "AtomicU8", "u32": "AtomicU32", "u64": "AtomicU64", "usize": "AtomicUSize"}
INTEGERS = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "isize", "usize"}

# CeeSafe's keywords (src/syntax/token.cee, keywordText); tools/cbind_check.py compares this with the lexer's table.
KEYWORDS = {
    "as", "async", "await", "borrow", "break", "class", "const", "continue", "defer", "drop", "else", "enum", "extern",
    "false", "fn", "for", "if", "impl", "import", "in", "interface", "internal", "let", "loop", "macro", "match", "move",
    "mut", "namespace", "null", "own", "private", "public", "return", "static", "struct", "this", "true", "type",
    "unsafe", "using", "var", "where", "while", "yield",
}
IDENTIFIER = re.compile(r"[A-Za-z_]\w*\Z")


def fail(message: str):
    sys.exit(f"cbind: {message}")


def safe_name(name: str) -> str:
    return name + "_" if name in KEYWORDS else name


def reserved(name: str) -> bool:
    """Does the C standard reserve the name: `__…`, or `_` and an uppercase letter (type-system.md §28.2; the rule of
    src/check/ffi.cee, checkCName)? ceesafec refuses such a C type's name (N0292), so the type has no CeeSafe spelling:
    it is not declared, and neither is what needs it. `struct __va_list_tag`, a `va_list` of x86-64, is one."""
    return name.startswith("__") or (len(name) > 1 and name[0] == "_" and "A" <= name[1] <= "Z")


RESERVED = "a type whose name C reserves: CeeSafe has no name for it (N0292)"

def top_const(c: str) -> bool:
    """Is the object itself `const`: `const int`, `char *const`, `const char *const [4]`, not `const char *`?"""
    c = re.sub(r"\s+", " ", c).strip()
    c = re.sub(r" ?(\[\d*\])+$", "", c)
    if "*" in c:
        return "const" in c[c.rindex("*"):]
    return c.startswith("const ") or c.endswith(" const")


def split_params(text: str) -> list[str]:
    if text.strip() in ("", "void"):
        return []
    depth, out, cur = 0, [], ""
    for ch in text:
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        depth += ch in "(["
        depth -= ch in ")]"
        cur += ch
    return [p.strip() for p in out + [cur]]


# ---- clang ---------------------------------------------------------------------------------------------------------

class Locations:
    """Clang's JSON gives a location's file only when it changes; this follows the dump order to know every node's."""

    def __init__(self):
        self.file = None

    def at(self, loc):
        if not isinstance(loc, dict):
            return
        parts = [loc[p] for p in ("spellingLoc", "expansionLoc") if isinstance(loc.get(p), dict)] or [loc]
        for part in parts:
            if "file" in part:
                self.file = part["file"]

    def enter(self, node: dict) -> str | None:
        """The file of the node's own location, with the dump's state moved past the node's range."""
        self.at(node.get("loc"))
        own = self.file
        rng = node.get("range")
        if isinstance(rng, dict):
            self.at(rng.get("begin"))
            self.at(rng.get("end"))
        return own


class Clang:
    def __init__(self, clang: str, flags: list[str]):
        self.base = [clang, "-std=c11", "-ffreestanding", "-fsyntax-only", "-w", "-ferror-limit=0",
                     "-fno-caret-diagnostics"] + flags

    def run(self, extra: list[str], path: Path) -> subprocess.CompletedProcess:
        return subprocess.run(self.base + extra + [str(path)], capture_output=True, text=True)


# ---- one configuration's reading of the header ----------------------------------------------------------------------

class Reading:
    """What one configuration's clang says the header declares: `entries`, by (group, name), each the lines of its
    CeeSafe declaration; `notes`, by name, why a declaration is not written; `mentions`, the types an entry names."""

    def __init__(self, header: str, path: Path, clang: Clang, known: set[str], windows: bool):
        self.header, self.path, self.clang, self.known, self.windows = header, path, clang, known, windows
        self.entries: dict[tuple[str, str], list[str]] = {}
        self.mentions: dict[tuple[str, str], set[str]] = {}
        self.notes: dict[str, str] = {}
        self.records: dict[str, dict] = {}     # a struct's CeeSafe name -> {fields, here, ctype, union}
        self.enums: dict[str, dict] = {}       # an enum's CeeSafe name -> {constants, here, ctype, underlying}
        self.typedefs: dict[str, str] = {}     # a typedef that is another type's name -> that type, as C spells it
        self.used: set[str] = set()
        self.read()

    # -- the C side

    def read(self):
        macros = self.macros()
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "cbind_probe.c"
            lines = [f'#include "{self.header}"']
            for name in macros:
                lines.append(f"extern __typeof__(({name})) cee_t_{name};")
                lines.append(f"enum {{ cee_v_{name} = ({name}) }};")
            probe.write_text("\n".join(lines) + "\n")
            # A macro that is not an expression fails its first line, one that is not an integer constant its second:
            # each alone, since each has a line of its own. Those lines are removed and the rest is read.
            check = self.clang.run([], probe)
            bad = {int(m.group(1)) for m in re.finditer(r"cbind_probe\.c:(\d+):\d+: error:", check.stderr)}
            if 1 in bad or (check.returncode != 0 and not bad):
                fail(f"clang cannot read {self.header}:\n{check.stderr[:4000]}")
            kept = [line if i + 1 not in bad else "" for i, line in enumerate(lines)]
            probe.write_text("\n".join(kept) + "\n")
            dump = self.clang.run(["-Xclang", "-ast-dump=json"], probe)
            if dump.returncode != 0:
                fail(f"clang cannot read {self.header}:\n{dump.stderr[:4000]}")
            ast = json.loads(dump.stdout)
        self.scan(ast)
        self.declare(macros)

    def macros(self) -> list[str]:
        """The header's own object-like macros, in order (`clang -E -dD` keeps each #define where it is written)."""
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "cbind_macros.c"
            source.write_text(f'#include "{self.header}"\n')
            run = self.clang.run(["-E", "-dD"], source)
        if run.returncode != 0:
            fail(f"clang cannot preprocess {self.header}:\n{run.stderr[:4000]}")
        here, names = False, []
        for line in run.stdout.splitlines():
            if m := re.match(r'# \d+ "(.*)"', line):
                here = self.same_file(m.group(1))
            elif here and (m := re.match(r"#define (\w+)(.?)(.*)", line)):
                name, after, body = m.groups()
                if after != "(" and body.strip() and not name.startswith("__") and name not in names:
                    names.append(name)
            elif here and (m := re.match(r"#undef (\w+)", line)) and m.group(1) in names:
                names.remove(m.group(1))
        return names

    def same_file(self, name: str | None) -> bool:
        if not name or name.startswith("<"):
            return False
        try:
            return Path(name).resolve() == self.path
        except OSError:
            return False

    def scan(self, ast: dict):
        """Every declaration at file scope, and the tags declared inside structs, with the file each is written in."""
        self.functions, self.variables, self.probes, self.order = {}, {}, {}, []
        self.fnptr_typedefs: dict[str, bool] = {}
        self.by_id: dict[str, dict] = {}
        locations = Locations()

        def skip(node):                       # keeps the dump's file state right without reading the subtree
            locations.enter(node)
            for child in node.get("inner", []) or []:
                skip(child)

        def tag(node, here):
            self.by_id[node.get("id", "")] = node
            node["cbind_here"] = here
            kind = node["kind"]
            if kind == "RecordDecl":
                for child in node.get("inner", []) or []:
                    inner_here = self.same_file(locations.enter(child))
                    if child.get("kind") in ("RecordDecl", "EnumDecl"):
                        tag(child, inner_here)
                    else:
                        for grand in child.get("inner", []) or []:
                            skip(grand)
                if node.get("name") and (node.get("completeDefinition") or node["name"] not in self.records):
                    self.note_record(node["name"], node, here, ctype=False)
            else:
                for child in node.get("inner", []) or []:
                    skip(child)
                if node.get("name"):
                    self.note_enum(node["name"], node, here, ctype=False)
                elif here:
                    self.order.append(("anonymous-enum", node))
                else:
                    for constant in self.constants_of(node):
                        if constant["name"].startswith("cee_v_"):
                            self.probes[constant["name"][len("cee_v_"):] + " value"] = constant

        for node in ast.get("inner", []) or []:
            here = self.same_file(locations.enter(node))
            kind = node.get("kind")
            if kind in ("RecordDecl", "EnumDecl"):
                tag(node, here)
                continue
            for child in node.get("inner", []) or []:
                skip(child)
            if node.get("isImplicit"):
                continue
            name = node.get("name", "")
            if kind == "TypedefDecl":
                self.note_typedef(name, node, here)
            elif kind == "FunctionDecl" and here and name not in self.functions:
                self.functions[name] = node
            elif kind == "VarDecl":
                if name.startswith("cee_t_"):
                    self.probes[name[len("cee_t_"):]] = node
                elif here and node.get("storageClass") == "extern" and name not in self.variables:
                    self.variables[name] = node

    def note_record(self, name: str, node: dict, here: bool, ctype: bool):
        self.records[name] = {"node": node, "here": here and bool(node.get("completeDefinition")), "ctype": ctype,
                              "union": node.get("tagUsed") == "union", "declared": here}

    def note_enum(self, name: str, node: dict, here: bool, ctype: bool):
        if name in self.enums and not node.get("inner"):
            return
        self.enums[name] = {"node": node, "here": here, "ctype": ctype}

    def note_typedef(self, name: str, node: dict, here: bool):
        spelled = node.get("type", {}).get("qualType", "")
        target = self.tag_of(node)
        if target is not None and not target.get("name"):
            # `typedef struct { … } name;`: the tag has no name of its own, so the typedef's is the type's.
            if target["kind"] == "RecordDecl":
                self.note_record(name, target, here, ctype=True)
            else:
                self.note_enum(name, target, here, ctype=True)
                self.order = [(k, n) for k, n in self.order if n is not target]
            return
        self.typedefs[name] = spelled
        if here and re.fullmatch(r".*\(\*\)\(.*\)", spelled):
            self.fnptr_typedefs[name] = True

    def tag_of(self, typedef: dict) -> dict | None:
        """The struct, union or enum a typedef names directly (not through a pointer), if it names one."""
        node = (typedef.get("inner") or [None])[0]
        while isinstance(node, dict):
            if node.get("kind") in ("RecordType", "EnumType"):
                return self.by_id.get(node.get("decl", {}).get("id", ""))
            if node.get("kind") != "ElaboratedType":
                return None
            owned = node.get("ownedTagDecl")
            if isinstance(owned, dict) and owned.get("id") in self.by_id:
                return self.by_id[owned["id"]]
            node = (node.get("inner") or [None])[0]
        return None

    # -- types

    def type_of(self, c: str, depth: int = 0, member: bool = False) -> str | None:
        """A C type's CeeSafe spelling, or None when CeeSafe has none; the names it mentions go to `self.used`. A struct's
        member (`member`) that is a `char` or an array of them is `u8`: §31.1 has no `CChar` member."""
        c = re.sub(r"\s+", " ", c).strip()
        if depth > 16 or not c or re.search(r"\bvolatile\b", c):       # CeeSafe has no volatile type
            return None
        if m := re.fullmatch(r"(.*?) ?\(\*(?: ?const)?\)\((.*)\)", c):
            return self.function_type(m.group(1), m.group(2), depth)
        if m := re.fullmatch(r"(.*?) ?\(\*(?: ?const)?\[(\d+)\]\)\((.*)\)", c):
            element = self.function_type(m.group(1), m.group(3), depth)
            return f"[{element}; {m.group(2)}]" if element else None
        if m := re.fullmatch(r"([^\[\(]*?) ?\[(\d+)\](.*)", c):
            element = self.type_of(m.group(1) + m.group(3), depth + 1, member)
            return f"[{element}; {m.group(2)}]" if element and element != "void" else None
        if m := re.fullmatch(r"(.*)\*(?: ?(?:const|restrict|volatile|__restrict))*", c):
            inner = m.group(1).strip()
            if "*" in inner:        # a pointer to a pointer: only a `const` after the last `*` is the pointee's
                const = bool(re.search(r"\* ?const$", inner))
                inner = re.sub(r"(\*) ?const$", r"\1", inner).strip()
            else:
                const = inner.startswith("const ") or inner.endswith(" const")
                inner = re.sub(r"^const | const$", "", inner).strip()
            if m2 := re.fullmatch(r"(.*?) ?\((.*)\)", inner):       # a pointer to a function type's typedef, spelled out
                return self.function_type(m2.group(1), m2.group(2), depth)
            if inner in self.typedefs and "(*)" in self.resolve(inner):
                # A pointer to a function pointer, such as a table of them: the typedef's name, an opaque C type.
                if reserved(inner):
                    return None
                self.used.add(inner)
                return ("*" if const else "*mut ") + inner if inner not in KEYWORDS else None
            target = self.type_of(inner, depth + 1)
            if target is None:
                return None
            if target.startswith('extern "C" fn') and inner in self.typedefs and "(*)" not in self.resolve(inner):
                return target                                         # `fn_t *`, fn_t a function type
            return ("*" if const else "*mut ") + target
        if m := re.fullmatch(r"_Atomic\((.*)\)|_Atomic (.*)", c):
            return ATOMICS.get(self.type_of(m.group(1) or m.group(2), depth + 1) or "")
        c = re.sub(r"\b(const|volatile) ", "", c)
        c = re.sub(r" (const|volatile)$", "", c)
        if c in SCALARS or c in NAMED:
            t = SCALARS.get(c) or NAMED[c]
            return "u8" if member and t == "CChar" else t
        if m := re.fullmatch(r"(struct|union|enum) ([A-Za-z_]\w*)", c):
            if m.group(1) == "union" or reserved(m.group(2)):
                return None
            self.used.add(m.group(2))
            return safe_name(m.group(2)) if m.group(2) not in KEYWORDS else None
        if not IDENTIFIER.match(c):
            return None
        if c in self.records or c in self.enums:        # a typedef that is the type's only name
            if reserved(c):
                return None
            self.used.add(c)
            return c
        if c in self.typedefs:
            return self.type_of(self.typedefs[c], depth + 1)
        return None

    def resolve(self, name: str) -> str:
        seen = set()
        while name in self.typedefs and name not in seen:
            seen.add(name)
            name = self.typedefs[name]
        return name

    def function_type(self, result: str, params: str, depth: int) -> str | None:
        out = []
        for p in split_params(params):
            if p == "...":
                return None
            t = self.type_of(p, depth + 1)
            if t is None or not self.by_value_ok(t):
                return None
            out.append(t)
        r = self.type_of(result, depth + 1)
        if r is None or not self.by_value_ok(r):
            return None
        return f'extern "C" fn({", ".join(out)})' + ("" if r == "void" else f" -> {r}")

    def by_value_ok(self, t: str) -> bool:
        """May a parameter or a result have this type? An imported struct's size is C's alone (N0294)."""
        return t not in self.records and not t.startswith("[") and not t.startswith("Atomic")

    def field_ok(self, t: str) -> bool:
        """type-system.md §31.1: what a member of an imported struct may be."""
        if t.startswith("["):
            t = t[1:t.rindex(";")]
        return t not in ("void", "f32", "f64") and not t.startswith("[")

    # -- declarations

    def constants_of(self, enum: dict) -> list[dict]:
        return [c for c in enum.get("inner", []) or [] if c.get("kind") == "EnumConstantDecl"]

    def values_of(self, enum: dict) -> list[tuple[str, int]] | None:
        out, value = [], -1
        for constant in self.constants_of(enum):
            found = self.value_of(constant)
            if found is None and constant.get("inner"):
                return None
            value = found if found is not None else value + 1
            out.append((constant["name"], value))
        return out

    def value_of(self, node: dict) -> int | None:
        for child in node.get("inner", []) or []:
            if child.get("kind") == "ConstantExpr" and "value" in child:
                return int(child["value"])
            found = self.value_of(child) if child.get("kind") in ("ImplicitCastExpr", "ParenExpr") else None
            if found is not None:
                return found
        return None

    def underlying(self, enum: dict, values: list[tuple[str, int]]) -> str:
        fixed = enum.get("fixedUnderlyingType", {}).get("qualType")
        if fixed and (t := self.type_of(fixed)) in INTEGERS:
            return t
        low = min([v for _, v in values] + [0])
        high = max([v for _, v in values] + [0])
        if low < -(1 << 31) or high > (1 << 32) - 1 or (low < 0 and high > (1 << 31) - 1):
            return "i64" if low < 0 else "u64"
        return "i32" if low < 0 or self.windows else "u32"

    def add(self, group: str, name: str, lines: list[str], used: set[str]):
        self.entries[(group, name)] = lines
        self.mentions[(group, name)] = set(used)

    def declare(self, macros: list[str]):
        taken: dict[str, str] = {}

        def claim(name: str, what: str) -> bool:
            if name in taken and taken[name] != what:
                self.notes[name] = f"{what}: the name is already {taken[name]}'s"
                return False
            taken[name] = what
            return True

        # Types of this header, in the order they are written.
        for name, record in self.records.items():
            if not record["here"]:
                continue
            if reserved(name):
                self.notes[name] = RESERVED
                continue
            if record["union"]:
                if record["ctype"] and claim(name, "a union"):
                    self.add("type", name, ["// a union: opaque", f'@ctype("{name}")', f"struct {name};"], set())
                else:
                    self.notes.setdefault(name, "a union with a tag: CeeSafe has no name for `union " + name + "`")
                continue
            if name in KEYWORDS:
                self.notes[name] = "a struct whose name is a CeeSafe keyword"
                continue
            if not claim(name, "a struct"):
                continue
            self.used = set()
            fields, opaque = [], None
            for child in record["node"].get("inner", []) or []:
                if child.get("kind") != "FieldDecl":
                    continue
                field = child.get("name")
                if not field:
                    opaque = "an anonymous struct or union member"
                elif child.get("isBitfield"):
                    opaque = f"the bit-field `{field}`"
                else:
                    c = child.get("type", {}).get("qualType", "")
                    t = self.type_of(c, member=True)
                    if t is None or not self.field_ok(t):
                        opaque = f"the member `{field}`, a `{c}`"
                    elif field in KEYWORDS:
                        fields += [f'    @cname("{field}")', f"    {field}_: {t};"]
                    else:
                        fields.append(f"    {field}: {t};")
                if opaque:
                    break
            head = [f'@ctype("{name}")'] if record["ctype"] else []
            if opaque:
                self.add("type", name, [f"// opaque for {opaque}"] + head + [f"struct {name};"], set())
            elif not fields:
                self.add("type", name, head + [f"struct {name};"], set())
            else:
                self.add("type", name, head + [f"struct {name} {{"] + fields + ["}"], self.used - {name})
        for name, enum in self.enums.items():
            if not enum["here"] or not self.constants_of(enum["node"]):
                continue
            if reserved(name):
                self.notes[name] = RESERVED
                continue
            values = self.values_of(enum["node"])
            if values is None or name in KEYWORDS or not claim(name, "an enum"):
                self.notes.setdefault(name, "an enum whose values clang does not give")
                continue
            head = [f'@ctype("{name}")'] if enum["ctype"] else []
            lines = head + [f"enum {name}: {self.underlying(enum['node'], values)};"]
            for constant, value in values:
                if constant in KEYWORDS or not claim(constant, "an enumerator"):
                    continue
                lines.append(f"const {constant}: {name} = {value};")
            self.add("type", name, lines, set())
        for name in self.fnptr_typedefs:
            if reserved(name):
                self.notes[name] = RESERVED
            elif name not in KEYWORDS and claim(name, "a typedef"):
                self.add("type", name, [f'@ctype("{name}")', f"struct {name};"], set())

        # Constants: anonymous enums' enumerators, then the macros.
        for kind, node in self.order:
            values = self.values_of(node) or []
            for constant, value in values:
                if constant.startswith("cee_v_") or constant in KEYWORDS or not claim(constant, "an enumerator"):
                    continue
                fits = "i32" if -(1 << 31) <= value < (1 << 31) else ("u32" if 0 <= value < (1 << 32) else
                                                                      ("i64" if value < 0 else "u64"))
                self.add("constant", constant, [f"const {constant}: {fits} = {value};"], set())
        for name in macros:
            typed, valued = self.probes.get(name), self.probes.get(name + " value")
            if typed is None:
                self.notes[name] = "a macro that is not an expression"
                continue
            if name in KEYWORDS or not claim(name, "a macro"):
                continue
            c = typed["type"].get("desugaredQualType") or typed["type"].get("qualType", "")
            self.used = set()
            if re.fullmatch(r"(const )?char ?\[\d+\]", c):
                self.add("constant", name, [f"const {name}: *CChar;"], set())
                continue
            t = self.type_of(c)
            value = self.value_of(valued) if valued is not None else None
            if value is None or t is None or (t not in INTEGERS and t not in self.enums):
                self.notes[name] = "a macro that is not an integer constant or a string literal"
                continue
            self.add("constant", name, [f"const {name}: {t} = {value};"], self.used)

        # Data and functions.
        for name, node in self.variables.items():
            c = node.get("type", {}).get("qualType", "")
            self.used = set()
            t = self.type_of(c)
            if t is None or t == "void":
                self.notes[name] = f"data of a type CeeSafe cannot spell, `{c}`"
                continue
            if name in KEYWORDS or not claim(name, "data"):
                continue
            constant = top_const(c) or t.startswith("Atomic")
            self.add("data", name, [f"static {'' if constant else 'mut '}{name}: {t};"], self.used)
        for name, node in self.functions.items():
            c = node.get("type", {}).get("qualType", "")
            self.used = set()
            m = re.fullmatch(r"(.*?) ?\((.*)\)", c)
            result = self.type_of(m.group(1)) if m else None
            if result is None or not self.by_value_ok(result):
                self.notes[name] = f"a function whose result CeeSafe cannot take, `{c}`"
                continue
            params, bad = [], None
            for i, p in enumerate(x for x in node.get("inner", []) or [] if x.get("kind") == "ParmVarDecl"):
                pc = p.get("type", {}).get("qualType", "")
                t = self.type_of(pc)
                if t is None or not self.by_value_ok(t):
                    bad = pc
                    break
                params.append(f"{safe_name(p.get('name') or f'p{i}')}: {t}")
            if bad:
                self.notes[name] = f"a function with a parameter CeeSafe cannot take, `{bad}`"
                continue
            if not claim(name, "a function"):
                continue
            if node.get("variadic"):
                params.append("...")
            head = [f'@cname("{name}")'] if name in KEYWORDS else []
            tail = "" if result == "void" else f" -> {result}"
            self.add("function", name, head + [f"fn {safe_name(name)}({', '.join(params)}){tail};"], self.used)

        # Types of other headers that the entries mention: opaque here, unless a --with file declares them.
        mentioned = set().union(*self.mentions.values()) if self.mentions else set()
        for name in sorted(mentioned):
            if ("type", name) in self.entries or name in self.known:
                continue
            if name in self.enums:
                enum = self.enums[name]
                values = self.values_of(enum["node"]) or []
                head = [f'@ctype("{name}")'] if enum["ctype"] else []
                self.add("foreign", name, head + [f"enum {name}: {self.underlying(enum['node'], values)};"], set())
            else:
                record = self.records.get(name, {"ctype": name in self.typedefs})
                head = [f'@ctype("{name}")'] if record["ctype"] else []
                self.add("foreign", name, head + [f"struct {name};"], set())


# ---- the output -----------------------------------------------------------------------------------------------------

GROUPS = ("foreign", "type", "constant", "data", "function")


def names_of(path: Path) -> set[str]:
    """The C names a CeeSafe file declares (--with): they are left to it."""
    text = path.read_text()
    names = set(re.findall(r"^\s*(?:public )?(?:struct|enum|const|static(?: mut)?|(?:unsafe )?fn) (\w+)", text, re.M))
    return names | set(re.findall(r'@cname\("(\w+)"\)', text))


def narrow(reading: Reading, only: set[str]) -> set[tuple[str, str]]:
    keep = {key for key in reading.entries if key[1] in only}
    grew = True
    while grew:
        grew = False
        for key in list(keep):
            for name in reading.mentions.get(key, ()):
                for group in ("type", "foreign"):
                    if (group, name) in reading.entries and (group, name) not in keep:
                        keep.add((group, name))
                        grew = True
    return keep


class Binding:
    """One header's declarations: those every configuration gives alike (`same`), and the rest by name (`differs`)."""

    def __init__(self, header: str, readings: dict[str, Reading], only: set[str] | None):
        self.header, self.names = header, list(readings)
        self.first = readings[self.names[0]]
        keep = narrow(self.first, only) if only else None
        self.same, self.differs, self.opaque, every = [], [], {}, []
        for reading in readings.values():
            for key in reading.entries:
                if key not in every:
                    every.append(key)
        for key in every:
            if keep is not None and key not in keep:
                continue
            texts = [r.entries.get(key) for r in readings.values()]
            if all(t == texts[0] for t in texts):
                self.same.append(key)
            else:
                absent = [n for n, t in zip(self.names, texts) if t is None]
                self.differs.append(f"{key[1]} ({'absent in ' + ', '.join(absent) if absent else 'not the same in each'})")
                lines = self.common(key[1], texts) if key[0] == "type" and not absent else None
                if lines:       # what mentions the type still can: its name is declared, its members are not
                    self.opaque[key] = lines
                    self.same.append(key)
        self.notes = {n: why for n, why in self.first.notes.items() if not only or n in only}

    @staticmethod
    def common(name: str, texts: list[list[str]]) -> list[str] | None:
        """A type whose members or values differ by configuration, as much of it as every configuration agrees on: a
        struct opaque, an enum its integer."""
        heads = []
        for lines in texts:
            at = next((i for i, line in enumerate(lines) if line.startswith((f"struct {name}", f"enum {name}:"))), None)
            if at is None:
                return None
            decl = f"struct {name};" if lines[at].startswith("struct") else lines[at]
            heads.append([line for line in lines[:at] if line.startswith("@")] + [decl])
        if any(h != heads[0] for h in heads):
            return None
        return ["// not the same in every configuration: only its name is declared"] + heads[0]

    def render(self) -> str:
        out = [f"// GENERATED by tools/cbind.py (CeeSafe ADR 0095) from {self.header}: do not edit, regenerate it."]
        if len(self.names) > 1:
            out.append(f"// What is declared here is the same in: {', '.join(self.names)}.")
        out += ["", f'@header("{self.header}")', 'extern "C" {']
        body = []
        for group in GROUPS:
            keys = [k for k in self.same if k[0] == group]
            if group == "foreign":
                keys.sort(key=lambda k: k[1])
            for key in keys:
                body += ["    " + line for line in self.opaque.get(key) or self.first.entries[key]]
            if keys:
                body.append("")
        while body and not body[-1]:
            body.pop()
        out += body + ["}"]
        written = {name for _, name in self.same}
        notes = [(n, why) for n, why in self.notes.items() if n not in written]
        if notes:
            out += ["", "// Not declared:"] + [f"//   {name}: {why}" for name, why in notes]
        if self.differs:
            out += ["", "// Not the same in every configuration, so not declared: write these by hand, with @cfg (and --leave",
                    "// a type that is, so that its name is declared once)."]
            out += [f"//   {d}" for d in self.differs]
        return "\n".join(out) + "\n"


def settle(bindings: list[Binding], known: set[str]):
    """One declaration of each name among the bindings, which compile as one unit (N0201): a type belongs to the header
    that defines it, and anything else that several declare to the first of them. A name in `known` (--with, --leave)
    is declared elsewhere, and by none of them."""
    for binding in bindings:
        for key in binding.same:
            if key[1] in known and key[0] != "foreign":
                binding.notes[key[1]] = "declared elsewhere (--with or --leave)"
        binding.same = [key for key in binding.same if key[1] not in known]
    owner: dict[str, tuple[Binding, str]] = {}
    for wanted in ("type", "foreign", "constant", "data", "function"):     # a definition, then an opaque mention
        for binding in bindings:
            for group, name in binding.same:
                if group == wanted:
                    owner.setdefault(name, (binding, group))
    for binding in bindings:
        kept = []
        for key in binding.same:
            by, group = owner[key[1]]
            if by is binding and group == key[0]:
                kept.append(key)
            elif key[0] != "foreign":
                what = "this header" if by is binding else by.header
                kind = "type" if group == "foreign" else group
                binding.notes[f"{key[1]} ({key[0]})"] = f"the name is a {kind}'s, declared for {what}"
        binding.same = kept


def find_header(header: str, includes: list[str]) -> Path:
    for directory in includes:
        candidate = Path(directory) / header
        if candidate.is_file():
            return candidate.resolve()
    if Path(header).is_file():
        return Path(header).resolve()
    fail(f"{header} is not under any -I directory")


def formatter() -> str | None:
    for candidate in (os.environ.get("CEESAFEC"), str(CEESAFE / "out" / "ceesafec"), shutil.which("ceesafec")):
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="an extern \"C\" block for a C header (ADR 0095)")
    parser.add_argument("--header", action="append", required=True,
                        help="the header, as C code includes it; repeat it for several, written to --out-dir as one\n                        file each, which compile together")
    parser.add_argument("-I", dest="includes", action="append", default=[], help="an include directory")
    parser.add_argument("-D", dest="defines", action="append", default=[], help="a macro, NAME or NAME=VALUE")
    parser.add_argument("--target", help="clang's target triple")
    parser.add_argument("--config", action="append", default=[], metavar="NAME=FLAGS",
                        help="a configuration: its name and its clang flags; repeat it, and only what is the same in all is written")
    parser.add_argument("--only", action="append", default=[], help="names to write, with the types they mention")
    parser.add_argument("--with", dest="others", action="append", default=[], metavar="FILE",
                        help="a generated file compiled with this one: the types it declares are left to it")
    parser.add_argument("--leave", action="append", default=[], metavar="NAME,NAME",
                        help="names declared elsewhere, such as by lib/kernel (ADR 0081): they are not written")
    parser.add_argument("--clang", default=os.environ.get("CLANG", "clang"))
    parser.add_argument("--no-fmt", action="store_true", help="do not format the output with ceesafec")
    parser.add_argument("-o", dest="output", help="the file to write, for one header")
    parser.add_argument("--out-dir", help="the directory to write <header's name>.cee into, for several headers")
    args = parser.parse_args()

    if bool(args.output) == bool(args.out_dir) or (len(args.header) > 1 and not args.out_dir):
        fail("give -o FILE for one header, or --out-dir DIRECTORY")
    outputs = {}
    for header in args.header:
        output = Path(args.output) if args.output else Path(args.out_dir) / (Path(header).stem + ".cee")
        if output in outputs.values():
            fail(f"two headers would be written to {output}")
        outputs[header] = output
    common = [f"-I{i}" for i in args.includes] + [f"-D{d}" for d in args.defines]
    if args.target:
        common.append(f"--target={args.target}")
    configs: dict[str, list[str]] = {}
    for config in args.config:
        name, _, flags = config.partition("=")
        if not name or name in configs:
            fail(f"--config '{config}': a name, once, then '=' and the flags")
        configs[name] = shlex.split(flags)
    if not configs:
        configs["default"] = []
    known = set()
    for other in args.others:
        known |= names_of(Path(other))
    known |= {n for group in args.leave for n in group.split(",") if n}
    only = {n for group in args.only for n in group.split(",") if n} or None
    bindings = []
    for header in args.header:
        path = find_header(header, args.includes)
        readings = {}
        for name, flags in configs.items():
            every = common + flags
            windows = any("windows" in f or "msvc" in f for f in every if f.startswith("--target"))
            readings[name] = Reading(header, path, Clang(args.clang, every), known, windows)
        bindings.append(Binding(header, readings, only))
    if only:
        written = {name for b in bindings for _, name in b.same}
        if only - written:
            fail(f"--only names what is not declared, or not written: {', '.join(sorted(only - written))}")
    settle(bindings, known)
    exe = None if args.no_fmt else formatter()
    if exe is None and not args.no_fmt:
        fail("no ceesafec to format the output with: set CEESAFEC, or pass --no-fmt")
    for binding in bindings:
        output = outputs[binding.header]
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(binding.render())
        if exe:
            run = subprocess.run([exe, "fmt", str(output)], capture_output=True, text=True)
            if run.returncode != 0:
                fail(f"ceesafec fmt refused the output ({output}):\n{run.stdout}{run.stderr}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
