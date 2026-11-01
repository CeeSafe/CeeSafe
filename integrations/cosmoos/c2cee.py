#!/usr/bin/env python3
"""
c2cee.py -- porting CosmoOS's kernel C to CeeSafe (spec/tooling.md §9): `analyse` reports what a C file's port needs, from
clang's own front end (-Xclang -ast-dump=json); `port` drafts the port into a directory outside the tree; `verify` runs
a port's checks in a CosmoOS tree; `selftest` checks the analysis and the draft against a fixture. It never edits the
tree.

    python3 integrations/cosmoos/c2cee.py analyse kernel/scheduler/semaphore.c --header kernel/include/kernel/semaphore.h \
        [--cosmoos ~/dev/CosmoOS] [--arch x86_64|aarch64] [--json]
    python3 integrations/cosmoos/c2cee.py port kernel/ipc/pipe.c --header kernel/include/kernel/pipe.h --out <dir> \
        [--cosmoos ~/dev/CosmoOS] [--arch x86_64|aarch64]
    python3 integrations/cosmoos/c2cee.py verify [--cosmoos <tree>] [--arch x86_64|aarch64|both] [--no-boot] [--max-load 8]
    python3 integrations/cosmoos/c2cee.py selftest [--bless]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

CEESAFE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))   # c2cee_port
FIXTURE = Path(__file__).resolve().parent / "c2cee"

# Macros whose expansion in a function body means something for the port (spec/tooling.md §9.1).
MACROS = {
    "wait_event": "wait_floor_until (wait.h): a wait entry is C state no CeeSafe local may hold",
    "wait_event_timeout": "wait_floor_until_timeout (wait.h)",
    "wait_event_killable": "a floor helper (no CeeSafe helper yet)",
    "wait_event_killable_timeout": "a floor helper (no CeeSafe helper yet)",
    "list_for_each_entry": "a loop over lib/kernel's List, with offsetOf for the entry",
    "list_for_each_entry_safe": "a loop over lib/kernel's List that reads the next node first",
    "might_sleep": "wait_floor_check_sleep, with callerAddress()",
    "KASSERT": "a floor (panic's message)",
    "WARN": "a floor (a formatted warning)",
    "WARN_ON": "a floor (a formatted warning)",
    "container_of": "offsetOf<T>(field) and raw-pointer arithmetic, or lib/kernel's containerOf",
}

# C types and their CeeSafe spelling (type-system.md §28.3), after typedefs are resolved by clang.
SCALARS = {
    "_Bool": "bool", "bool": "bool", "void": "void",
    "char": "CChar", "signed char": "i8", "unsigned char": "u8",
    "short": "i16", "unsigned short": "u16", "int": "i32", "unsigned int": "u32",
    "long": "i64", "unsigned long": "usize", "long long": "i64", "unsigned long long": "u64",
}
NAMED = {
    "int8_t": "i8", "uint8_t": "u8", "int16_t": "i16", "uint16_t": "u16", "int32_t": "i32", "uint32_t": "u32",
    "int64_t": "i64", "uint64_t": "u64", "size_t": "usize", "uintptr_t": "usize", "intptr_t": "isize",
    "ptrdiff_t": "isize", "arch_irq_state_t": "usize", "unsigned": "u32",
}


def ceesafe_keywords() -> set[str]:
    """CeeSafe's keywords, from the lexer's own table (src/syntax/token.cee, keywordText)."""
    text = (CEESAFE / "src" / "syntax" / "token.cee").read_text()
    return set(re.findall(r'TokenKind\.\w+Keyword => "([a-z_][A-Za-z_]*)",', text))


def ceesafe_type(c: str) -> str:
    """A C type's CeeSafe spelling, as a port would declare it; `?` marks what needs a decision."""
    c = re.sub(r"\s+", " ", c.replace("struct ", "").strip())
    if m := re.fullmatch(r"(.*?)\s*\(\*\)\((.*)\)", c):
        params = ", ".join(ceesafe_type(p) for p in split_params(m.group(2)))
        result = ceesafe_type(m.group(1))
        return f'extern "C" fn({params})' + ("" if result == "void" else f" -> {result}")
    if m := re.fullmatch(r"(.*)\[(\d+)\]", c):
        return f"[{ceesafe_type(m.group(1))}; {m.group(2)}]"
    if c.endswith("*"):
        inner = c[:-1].strip()
        const = inner.startswith("const ")
        inner = inner[len("const "):] if const else inner
        target = "void" if inner == "void" else ceesafe_type(inner)
        return ("*" if const else "*mut ") + target
    c = c.replace("const ", "").replace("volatile ", "")
    return SCALARS.get(c) or NAMED.get(c) or c


def split_params(text: str) -> list[str]:
    if text.strip() in ("", "void"):
        return []
    depth, out, cur = 0, [], ""
    for ch in text:
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    return [p.strip() for p in out + [cur]]


# ---- clang ---------------------------------------------------------------------------------------------------------

def clang_ast(path: Path, includes: list[Path], defines: list[str], target: str | None) -> dict:
    args = ["clang", "-std=c11", "-ffreestanding", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-w"]
    if target:
        args += [f"--target={target}"]
    args += [f"-D{d}" for d in defines] + [f"-I{i}" for i in includes] + [str(path)]
    run = subprocess.run(args, capture_output=True, text=True)
    if run.returncode != 0:
        sys.exit(f"c2cee: clang failed on {path}:\n{run.stderr[:4000]}")
    return json.loads(run.stdout)


class Locations:
    """Clang's JSON gives a location's file only when it changes; this follows the dump order to know every node's."""

    def __init__(self):
        self.file = None

    def update(self, node: dict):
        for loc in self._locs(node):
            if "file" in loc:
                self.file = loc["file"]

    @staticmethod
    def _locs(node: dict):
        for key in ("loc",):
            loc = node.get(key)
            if isinstance(loc, dict):
                yield from Locations._parts(loc)
        rng = node.get("range")
        if isinstance(rng, dict):
            for end in ("begin", "end"):
                if isinstance(rng.get(end), dict):
                    yield from Locations._parts(rng[end])

    @staticmethod
    def _parts(loc: dict):
        if "spellingLoc" in loc or "expansionLoc" in loc:
            for part in ("spellingLoc", "expansionLoc"):
                if isinstance(loc.get(part), dict):
                    yield loc[part]
        else:
            yield loc


def walk(node: dict, visit, locations: Locations):
    locations.update(node)
    visit(node, locations.file)
    for child in node.get("inner", []) or []:
        walk(child, visit, locations)


# ---- the include graph ---------------------------------------------------------------------------------------------

def includes_of(header: Path, search: list[Path]) -> list[Path]:
    out = []
    for m in re.finditer(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', header.read_text(errors="replace"), re.M):
        for base in [header.parent] + search:
            candidate = (base / m.group(1)).resolve()
            if candidate.is_file():
                out.append(candidate)
                break
    return out


def reaches(start: Path, goal: Path, search: list[Path], memo: dict) -> bool:
    """Does `start` include `goal`, directly or not?"""
    if start.resolve() == goal:
        return False
    seen, stack = set(), [start.resolve()]
    while stack:
        h = stack.pop()
        if h in seen:
            continue
        seen.add(h)
        if h == goal:
            return True
        stack.extend(memo.setdefault(h, includes_of(h, search)))
    return False


# ---- analyse -------------------------------------------------------------------------------------------------------

def analyse(source: Path, header: Path | None, includes: list[Path], target: str | None, root: Path,
            keep: dict | None = None) -> dict:
    source = source.resolve()
    raw = source.read_bytes()   # clang's offsets count bytes, not characters
    text = raw.decode(errors="replace")
    asts = {cfg: clang_ast(source, includes, [f"CONFIG_DEBUG={1 if cfg == 'debug' else 0}"], target)
            for cfg in ("debug", "release")}
    rel = lambda p: os.path.relpath(p, root) if p else "?"

    # Every function declaration, per build: where, and whether static inline or variadic.
    decls: dict[str, dict] = {}
    for cfg, ast in asts.items():
        locations = Locations()

        def visit(n, file, cfg=cfg):
            if n.get("kind") == "FunctionDecl" and "name" in n:
                d = decls.setdefault(n["name"], {"type": n["type"]["qualType"], "file": file, "kinds": {}})
                inline = n.get("storageClass") == "static"   # a static function in a header is an inline one
                kind = "variadic" if n.get("variadic") else "inline" if inline else "function"
                if d["kinds"].get(cfg) != "inline":
                    d["kinds"][cfg] = kind
                if file and Path(file).resolve() != source and d["file"] in (None, str(source)):
                    d["file"] = file

        walk(ast, visit, locations)

    keywords = ceesafe_keywords()
    functions, atomics_fields = [], {}
    ast = asts["debug"]
    locations = Locations()
    for top in ast.get("inner", []):
        locations.update(top)
        if top.get("kind") != "FunctionDecl" or not any(c.get("kind") == "CompoundStmt" for c in top.get("inner", [])):
            continue
        if locations.file and Path(locations.file).resolve() != source:
            continue   # a header's inline function
        info = {"name": top["name"], "c": top["type"]["qualType"], "static": top.get("storageClass") == "static",
                "calls": {}, "pointer_calls": [], "builtins": set(), "atomics": [], "macros": set(), "locals": []}
        params = [p for p in top.get("inner", []) if p.get("kind") == "ParmVarDecl"]
        ret = top["type"]["qualType"].split("(")[0].strip()
        info["ceesafe"] = (f"fn {top['name']}(" + ", ".join(f"{p.get('name', '_')}: {ceesafe_type(p['type'].get('desugaredQualType', p['type']['qualType']))}"
                                                        for p in params) + ")"
                        + ("" if ret == "void" else f" -> {ceesafe_type(ret)}"))

        def scan(n, file, info=info):
            kind = n.get("kind")
            if kind == "CallExpr":
                callee = n.get("inner", [{}])[0]
                ref = find(callee, lambda x: x.get("kind") == "DeclRefExpr")
                if ref and ref.get("referencedDecl", {}).get("kind") == "FunctionDecl":
                    name = ref["referencedDecl"]["name"]
                    if name in ("__builtin_expect", "__builtin_unreachable"):
                        pass   # likely/unlikely hints: nothing to port
                    elif name.startswith(("__builtin", "__atomic", "__sync", "__c11_atomic")):
                        info["builtins"].add(name)
                    else:
                        info["calls"][name] = True
                else:
                    member = find(callee, lambda x: x.get("kind") == "MemberExpr")
                    info["pointer_calls"].append(member.get("name", "?") if member else "?")
            elif kind == "AtomicExpr":
                info["builtins"].add(n.get("name", "__atomic"))
                member = find(n, lambda x: x.get("kind") == "MemberExpr")
                if member:
                    field_type = member.get("type", {}).get("qualType", "")
                    info["atomics"].append({"op": n.get("name"), "field": member.get("name"), "type": field_type})
                    atomics_fields.setdefault(member.get("name"), set()).add(field_type)
            elif kind == "VarDecl" and n.get("type", {}).get("qualType", "").startswith("struct ") \
                    and not n["type"]["qualType"].endswith("*"):
                info["locals"].append({"name": n.get("name"), "type": n["type"]["qualType"]})
            exp = (n.get("loc") or {}).get("expansionLoc") or (n.get("range", {}).get("begin") or {}).get("expansionLoc")
            if isinstance(exp, dict) and "offset" in exp and "tokLen" in exp and not exp.get("isMacroArgExpansion"):
                token = raw[exp["offset"]:exp["offset"] + exp["tokLen"]].decode(errors="replace")
                if re.fullmatch(r"[A-Za-z_]\w*", token):
                    info["macros"].add(token)

        for child in top.get("inner", []):
            if child.get("kind") == "CompoundStmt":
                walk(child, scan, Locations())
        functions.append(info)

    exported = set(re.findall(r"EXPORT_SYMBOL\(\s*(\w+)\s*\)", text))
    search = includes
    goal = header.resolve() if header else None
    memo: dict = {}

    def header_note(file):
        if not file or not goal:
            return None
        h = Path(file).resolve()
        if h == goal:
            return "the port's own header: its functions become the port's exports"
        if reaches(h, goal, search, memo):
            return "includes the port's header: not a @header (declare without one, or through a floor)"
        return None

    own = {f["name"] for f in functions}
    report_functions, callees, headers = [], {}, {}
    for f in functions:
        calls = []
        for name in sorted(f["calls"]):
            d = decls.get(name, {"file": None, "kinds": {}, "type": "?"})
            if name in own:
                calls.append({"name": name, "header": rel(source), "notes": ["the file's own function: a CeeSafe function of the port"]})
                continue
            kinds = d["kinds"]
            notes = []
            if kinds.get("debug") != kinds.get("release"):
                notes.append(f"{kinds.get('debug')} in debug, {kinds.get('release')} in release: declare it through its header")
            if "variadic" in kinds.values():
                notes.append("variadic: a floor")
            elif "inline" in kinds.values() and kinds.get("debug") == kinds.get("release"):
                notes.append("static inline: callable through its header")
            hnote = header_note(d["file"])
            if hnote:
                notes.append(hnote)
            calls.append({"name": name, "header": rel(d["file"]), "notes": notes})
            callees[name] = calls[-1]
            if d["file"] and Path(d["file"]).resolve() != source:
                headers.setdefault(rel(d["file"]), hnote)
        macros = sorted(m for m in f["macros"] if m in MACROS or m == "EXPORT_SYMBOL")
        builtins = sorted(f["builtins"])
        report_functions.append({
            "name": f["name"], "c": f["c"], "ceesafe": f["ceesafe"], "static": f["static"], "exported": f["name"] in exported,
            "calls": calls,
            "pointer_calls": [{"member": m, "note": "a call through a function pointer: a floor if the member is an imported struct's (§31.1)"}
                              for m in f["pointer_calls"]],
            "builtins": [{"name": b, "note": builtin_note(b)} for b in builtins],
            "atomics": f["atomics"],
            "macros": [{"name": m, "note": MACROS.get(m, "")} for m in macros],
            "c_state_locals": [{"name": l["name"], "type": l["type"],
                                "note": "a local of struct type: if it holds C state, a floor helper (§33.1)"} for l in f["locals"]],
        })

    structs = []
    if header:
        structs = analyse_structs(header.resolve(), asts["debug"], keywords, atomics_fields, root, source)

    if keep is not None:
        keep.update(asts=asts, decls=decls, keywords=keywords)
    return {
        "file": rel(source), "header": rel(header.resolve()) if header else None,
        "functions": report_functions,
        "structs": structs,
        "headers": [{"header": h, "note": n} for h, n in sorted(headers.items())],
        "floor": sorted({f"{f['name']}: {why}" for f in report_functions for why in floor_reasons(f)}),
    }


def builtin_note(name: str) -> str:
    if name == "__builtin_return_address":
        return "callerAddress() in an @export function (ADR 0030)"
    if name == "__atomic_thread_fence":
        return "a floor: CeeSafe has no fence"
    if name.startswith("__atomic"):
        return "an atomic view of the field (lib/core's AtomicU32/U64/USize; a bool field needs a u32)"
    return ""


def floor_reasons(f: dict):
    for c in f["calls"]:
        if any(n.startswith("variadic") for n in c["notes"]):
            yield f"calls variadic {c['name']}"
    for p in f["pointer_calls"]:
        yield f"calls through {p['member']}"
    for b in f["builtins"]:
        if b["name"] == "__atomic_thread_fence":
            yield "a fence"
    for m in f["macros"]:
        if m["name"] in ("KASSERT", "WARN", "WARN_ON", "wait_event", "wait_event_timeout", "wait_event_killable",
                         "wait_event_killable_timeout", "EXPORT_SYMBOL"):
            yield f"uses {m['name']}"
    for l in f["c_state_locals"]:
        yield f"holds a {l['type']} local"


def find(node: dict, pred):
    if pred(node):
        return node
    for child in node.get("inner", []) or []:
        if (hit := find(child, pred)):
            return hit
    return None


def analyse_structs(header: Path, ast: dict, keywords: set, atomics_fields: dict, root: Path, source: Path) -> list:
    out, locations = [], Locations()
    for top in ast.get("inner", []):
        locations.update(top)
        if top.get("kind") != "RecordDecl" or not top.get("completeDefinition") or not top.get("name"):
            continue
        if not locations.file or Path(locations.file).resolve() != header:
            continue
        fields = []
        for fd in top.get("inner", []):
            if fd.get("kind") != "FieldDecl":
                continue
            ctype = fd["type"]["qualType"]
            notes = []
            if fd["name"] in keywords:
                notes.append(f"'{fd['name']}' is a CeeSafe keyword: rename the C member (no @cname on @repr(C) fields)")
            if ctype.startswith("const ") and ctype.endswith("*"):
                notes.append("a const pointer: the generated member has no const; a C initialiser storing a literal needs a cast")
            used = atomics_fields.get(fd["name"])
            if used:
                if ctype in ("bool", "_Bool"):
                    notes.append("used atomically and a bool: make it a u32 (lib/core's AtomicBool is four bytes)")
                else:
                    notes.append("used atomically: an atomic view of the field")
            macros = sorted(set(m for m, body in initialiser_macros(header) if re.search(rf"\.{re.escape(fd['name'])}\b", body)))
            if macros:
                notes.append("set by the header's initialiser macro" + ("s " if len(macros) > 1 else " ") + ", ".join(macros))
            users = [u for u in member_users(root, header, top["name"], fd["name"]) if (root / u).resolve() != source]
            if users:
                notes.append("named in " + ", ".join(users[:6]) + (" and more" if len(users) > 6 else ""))
            desugared = fd["type"].get("desugaredQualType", ctype)
            # A typedef's name is kept (a @ctype), except a function pointer's, which CeeSafe spells out.
            fields.append({"name": fd["name"], "c": ctype, "ceesafe": ceesafe_type(desugared if "(*)" in desugared else ctype),
                           "notes": notes})
        out.append({"name": top["name"], "fields": fields})
    return out


def initialiser_macros(header: Path) -> list[tuple[str, str]]:
    """The header's function-like macros, with their bodies (continuation lines joined)."""
    text = re.sub(r"\\\n", " ", header.read_text(errors="replace"))
    return re.findall(r"^\s*#\s*define\s+(\w+)\([^)]*\)(.*)$", text, re.M)


def member_users(root: Path, header: Path, struct: str, member: str) -> list[str]:
    """Files other than the port's that name `->member` or `.member` and either name `struct <struct>` or include the
    header (a heuristic: other structs share member names, so review the list)."""
    if not (root / ".git").exists():
        return []
    # POSIX extended regexes (git grep -E) have no \b: the end of the name is spelled out.
    run = subprocess.run(["git", "grep", "-lE", rf"(->|\.){re.escape(member)}([^A-Za-z0-9_]|$)", "--", "*.c", "*.h"], cwd=root,
                         capture_output=True, text=True)
    base = header.name
    users = []
    for f in run.stdout.split():
        p = root / f
        if p.resolve() == header:
            continue
        try:
            body = p.read_text(errors="replace")
            if re.search(rf"\bstruct\s+{re.escape(struct)}\b", body) or \
                    re.search(rf'#\s*include\s*[<"][^>"]*{re.escape(base)}[>"]', body):
                users.append(f)
        except OSError:
            pass
    return sorted(users)


def print_report(r: dict):
    print(f"c2cee analyse: {r['file']}" + (f" (types from {r['header']})" if r["header"] else ""))
    for f in r["functions"]:
        flags = [w for w, on in (("static", f["static"]), ("exported", f["exported"])) if on]
        print(f"\n{f['name']}{' [' + ', '.join(flags) + ']' if flags else ''}\n    C:    {f['c']}\n    CeeSafe: {f['ceesafe']}")
        for c in f["calls"]:
            print(f"    calls {c['name']} ({c['header']})" + (": " + "; ".join(c["notes"]) if c["notes"] else ""))
        for p in f["pointer_calls"]:
            print(f"    calls through {p['member']}: {p['note']}")
        for b in f["builtins"]:
            print(f"    builtin {b['name']}: {b['note']}")
        for a in f["atomics"]:
            print(f"    atomic {a['op']} on {a['field']} ({a['type']})")
        for m in f["macros"]:
            print(f"    macro {m['name']}" + (f": {m['note']}" if m["note"] else ""))
        for l in f["c_state_locals"]:
            print(f"    local {l['name']}: {l['type']}: {l['note']}")
    for s in r["structs"]:
        print(f"\nstruct {s['name']}")
        for fd in s["fields"]:
            print(f"    {fd['name']}: {fd['ceesafe']}   ({fd['c']})" + ("".join(f"\n        - {n}" for n in fd["notes"])))
    if r["headers"]:
        print("\n@header candidates")
        for h in r["headers"]:
            print(f"    {h['header']}" + (f": {h['note']}" if h["note"] else ""))
    if r["floor"]:
        print("\nfloor (C that stays)")
        for line in r["floor"]:
            print(f"    {line}")


# ---- verify --------------------------------------------------------------------------------------------------------

def verify(tree: Path, archs: list[str], boot: bool, max_load: float, ceesafec: str) -> int:
    # A port lands in CosmoOS as a CeeSafe source, which its build compiles with the pinned ceesafec (CosmoOS's
    # scripts/cee-gen.py, build/cee.mk): these are the tree's own checks, run with that compiler.
    if not (tree / "scripts" / "cee-gen.py").is_file():
        sys.exit(f"c2cee verify: {tree} has no scripts/cee-gen.py: a CosmoOS tree from before its move to CeeSafe "
                 "(it built with the reference compiler); verify a tree at or after that change")
    env = dict(os.environ, CEESAFEC=ceesafec)
    make = "gmake" if subprocess.run(["which", "gmake"], capture_output=True).returncode == 0 else "make"
    results = []

    peak = [0.0]   # the highest one-minute load average seen during the last step

    def step(name: str, args: list[str], show=None) -> bool:
        t0 = time.monotonic()
        peak[0] = 0.0
        done = threading.Event()

        def sample():
            while hasattr(os, "getloadavg") and not done.wait(5):
                peak[0] = max(peak[0], os.getloadavg()[0])

        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        run = subprocess.run(args, cwd=tree, env=env, capture_output=True, text=True)
        done.set()
        sampler.join()
        out = run.stdout + run.stderr
        ok = run.returncode == 0
        extra = show(out) if show else ""
        line = f"{'PASS' if ok else 'FAIL'}  {name} ({time.monotonic() - t0:.0f} s){extra}"
        if not ok:
            tail = [l for l in out.splitlines() if re.search(r"error|FAIL|failed", l)][:6]
            line += "".join(f"\n        {l}" for l in tail)
        print(line, flush=True)
        results.append((name, ok))
        return ok

    def digests(out: str) -> str:
        found = re.findall(r"(\w[\w-]* trace): .*?digest (0x[0-9a-f]+) \(recorded (0x[0-9a-f]+)\)", out)
        return "".join(f"; {n} {d}{'' if d == r else ' != ' + r}" for n, d, r in found)

    def boot_summary(out: str) -> str:
        m = re.search(r"boot-test: (PASS|FAIL)[^\n]*", out)
        fails = re.findall(r"SELFTEST: (\S+)\s+\.\.\. FAIL[^\n]*", out)
        return (f"; {m.group(0)}" if m else "") + "".join(f"\n        FAIL {f}" for f in fails)

    def quiet():
        if max_load <= 0 or not hasattr(os, "getloadavg"):
            return
        while os.getloadavg()[0] >= max_load:
            time.sleep(30)

    def loaded() -> bool:
        """The host was busy at some point during the last step (a spike can be over by the time it fails)."""
        return max_load > 0 and peak[0] >= max_load

    step("cee-check", [make, "cee-check"])
    step("host-test", [make, "host-test"], digests)
    step("check-docs", [make, "check-docs"])
    for arch in archs:
        for build in ("debug", "release"):
            step(f"{arch} {build} build", [make, f"ARCH={arch}", f"BUILD={build}", "-j8"])
        step(f"{arch} analyze", [make, f"ARCH={arch}", "analyze"])
        if arch == archs[0]:
            # The fuzzers' shims define some of a port's functions over its types (tests/fuzz/shim_*.c).
            step("fuzz build", [make, f"ARCH={arch}", "fuzz-build", "-j8"])
        if boot:
            for build in ("debug", "release"):
                quiet()
                args = [make, f"ARCH={arch}", f"BUILD={build}", "test"]
                if not step(f"{arch} {build} boot", args, boot_summary) and loaded():
                    # A boot that failed while the host was busy is run once more on a quiet host. Both lines are
                    # printed; the second decides, and a failure that is not the load's fails it too.
                    print(f"      load average up to {peak[0]:.1f} during the boot: booting once more", flush=True)
                    results.pop()
                    quiet()
                    step(f"{arch} {build} boot (again, quiet host)", args, boot_summary)
    failed = [n for n, ok in results if not ok]
    print(f"\nc2cee verify: {len(results) - len(failed)} of {len(results)} passed" + (f"; first failure: {failed[0]}" if failed else ""))
    return 1 if failed else 0


# ---- port (stage 2) -----------------------------------------------------------------------------------------------

def port(source: Path, header: Path | None, includes: list[Path], target: str | None, root: Path, out: Path):
    """Drafts the port into `out` (spec/tooling.md §9.4); returns the files written and their TODO counts."""
    import c2cee_port
    keep: dict = {}
    report = analyse(source, header, includes, target, root, keep)
    draft = c2cee_port.Port(sys.modules[__name__], source.resolve(), header.resolve() if header else None, keep["asts"],
                             report, keep["keywords"], keep["decls"], root).draft()
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for name, text in draft.items():
        (out / name).write_text(text)
        written.append((str(out / name), text.count("TODO(c2cee)")))
    return written


# ---- main ----------------------------------------------------------------------------------------------------------

def cosmo_includes(tree: Path, arch: str, source: Path) -> list[Path]:
    """The kernel's own include path (CosmoOS's build/toolchain.mk, KERNEL_CFLAGS): the headers its build generates from
    CeeSafe (out/cee/kernel/include, CEE_CFLAGS; `make cee` writes them), then the tree's."""
    return [tree / "out" / "cee" / "kernel" / "include", tree / "kernel" / "include", tree / "drivers" / "include",
            tree / "boot" / "protocol", tree / "kernel" / "arch" / arch / "include", source.parent.resolve()]


def selftest(bless: bool) -> int:
    """The fixture's analysis (§9.3) and its drafted port (§9.4), against their reviewed snapshots."""
    import difflib
    import tempfile
    report = analyse(FIXTURE / "fixture.c", FIXTURE / "fixture.h", [FIXTURE], None, FIXTURE)
    results = {"fixture.expected.json": json.dumps(report, indent=2, sort_keys=True) + "\n"}
    with tempfile.TemporaryDirectory() as tmp:
        port(FIXTURE / "fixture.c", FIXTURE / "fixture.h", [FIXTURE], None, FIXTURE, Path(tmp))
        results["fixture.draft.cee"] = (Path(tmp) / "fixture.cee").read_text()
        results["fixture_floor.draft.c"] = (Path(tmp) / "fixture_floor.c").read_text()
    failed = 0
    for name, got in results.items():
        expected = FIXTURE / name
        if bless:
            expected.write_text(got)
            print(f"c2cee selftest: wrote {expected.relative_to(CEESAFE)}")
        elif not expected.exists() or expected.read_text() != got:
            sys.stdout.writelines(difflib.unified_diff(expected.read_text().splitlines(True) if expected.exists() else [],
                                                       got.splitlines(True), f"expected {name}", f"got {name}"))
            failed += 1
    if bless:
        return 0
    print("c2cee selftest: " + ("PASS" if not failed else f"FAIL in {failed} file(s) (run with --bless after reviewing)"))
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Porting CosmoOS C to CeeSafe (spec/tooling.md §9).")
    sub = parser.add_subparsers(dest="command", required=True)
    a = sub.add_parser("analyse", help="report what a C file's port needs")
    a.add_argument("file")
    a.add_argument("--header", help="the header that defines the types the port takes over")
    a.add_argument("--cosmoos", default=str(Path.home() / "dev" / "CosmoOS"))
    a.add_argument("--arch", choices=["x86_64", "aarch64"], default="x86_64")
    a.add_argument("--json", action="store_true")
    v = sub.add_parser("verify", help="run a port's checks in a CosmoOS tree")
    v.add_argument("--cosmoos", default=".")
    v.add_argument("--arch", choices=["x86_64", "aarch64", "both"], default="both")
    v.add_argument("--no-boot", action="store_true")
    v.add_argument("--max-load", type=float, default=8.0)
    v.add_argument("--ceesafec", default=os.environ.get("CEESAFEC") or shutil.which("ceesafec")
                   or str(Path.home() / ".ceesafe" / "bin" / "ceesafec"),
                   help="the ceesafec CosmoOS's Make runs: the version it pins (python3 scripts/cee-gen.py --pinned)")
    pt = sub.add_parser("port", help="draft a C file's CeeSafe port into a directory (stage 2)")
    pt.add_argument("file")
    pt.add_argument("--header", help="the header whose structs the port takes over")
    pt.add_argument("--out", required=True, help="the directory to write the draft into (never the tree)")
    pt.add_argument("--cosmoos", default=str(Path.home() / "dev" / "CosmoOS"))
    pt.add_argument("--arch", choices=["x86_64", "aarch64"], default="x86_64")
    s = sub.add_parser("selftest", help="check the analysis against integrations/cosmoos/c2cee/fixture.c")
    s.add_argument("--bless", action="store_true")
    args = parser.parse_args()

    if args.command == "port":
        tree = Path(args.cosmoos).expanduser().resolve()
        source = Path(args.file)
        source = source if source.is_absolute() else (tree / source if (tree / source).exists() else source.resolve())
        header = None
        if args.header:
            header = Path(args.header)
            header = header if header.is_absolute() else (tree / header if (tree / header).exists() else header.resolve())
        target = {"x86_64": "x86_64-unknown-none-elf", "aarch64": "aarch64-unknown-none-elf"}[args.arch]
        written = port(source, header, cosmo_includes(tree, args.arch, source), target, tree, Path(args.out))
        for name, todos in written:
            print(f"c2cee port: wrote {name}" + (f" ({todos} TODO(c2cee))" if todos else ""))
        return 0
    if args.command == "analyse":
        tree = Path(args.cosmoos).expanduser().resolve()
        source = Path(args.file)
        source = source if source.is_absolute() else (tree / source if (tree / source).exists() else source.resolve())
        header = None
        if args.header:
            header = Path(args.header)
            header = header if header.is_absolute() else (tree / header if (tree / header).exists() else header.resolve())
        target = {"x86_64": "x86_64-unknown-none-elf", "aarch64": "aarch64-unknown-none-elf"}[args.arch]
        report = analyse(source, header, cosmo_includes(tree, args.arch, source), target, tree)
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print_report(report)
        return 0
    if args.command == "verify":
        archs = ["x86_64", "aarch64"] if args.arch == "both" else [args.arch]
        return verify(Path(args.cosmoos).expanduser().resolve(), archs, not args.no_boot, args.max_load,
                      str(Path(args.ceesafec).expanduser()))
    return selftest(args.bless)


if __name__ == "__main__":
    sys.exit(main())
