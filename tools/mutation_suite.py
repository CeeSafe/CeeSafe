#!/usr/bin/env python3
"""The M2 mutation suite against a ceesafec.

Seed programs that compile are mutated structurally: adjacent statements swapped, statements moved or removed, scopes
added or removed, moves duplicated. Each mutant's expected diagnostics, as (code, line) pairs, come from an oracle: a
separate model of the rules in ownership.md and lifetimes.md for straight-line code, sharing no code with either
compiler. The compiler must report exactly those diagnostics.

The mutants are fixed by their seeds: .NET's seeded System.Random (the legacy subtractive generator that `new
Random(seed)` uses) is reproduced here, with the seeds 1000 + seed and its draws in a fixed order.

usage: python3 tools/mutation_suite.py --compiler out/ceesafec [--compiler ANOTHER_CEESAFEC] [--jobs N]
"""
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MUTANTS_PER_SEED = 120
INT_MAX = 2147483647


class DotNetRandom:
    """System.Random(seed): the Net5CompatSeedImpl generator (Knuth's subtractive method)."""

    def __init__(self, seed):
        self.seeds = [0] * 56
        subtraction = INT_MAX if seed == -2147483648 else abs(seed)
        mj = 161803398 - subtraction
        self.seeds[55] = mj
        mk = 1
        ii = 0
        for i in range(1, 55):
            ii += 21
            if ii >= 55:
                ii -= 55
            self.seeds[ii] = mk
            mk = mj - mk
            if mk < 0:
                mk += INT_MAX
            mj = self.seeds[ii]
        for _ in range(1, 5):
            for i in range(1, 56):
                n = i + 30
                if n >= 55:
                    n -= 55
                self.seeds[i] -= self.seeds[1 + n]
                if self.seeds[i] < 0:
                    self.seeds[i] += INT_MAX
        self.inext = 0
        self.inextp = 21

    def _sample(self):
        inext = self.inext + 1
        if inext >= 56:
            inext = 1
        inextp = self.inextp + 1
        if inextp >= 56:
            inextp = 1
        value = self.seeds[inext] - self.seeds[inextp]
        if value == INT_MAX:
            value -= 1
        if value < 0:
            value += INT_MAX
        self.seeds[inext] = value
        self.inext = inext
        self.inextp = inextp
        return value

    def next(self, max_value):
        return int(self._sample() * (1.0 / INT_MAX) * max_value)


# ---- Statements: instances are identified by identity, not by value ----

class Stmt:
    def __init__(self, kind, name=None, reference=None, mutable=False, body=None):
        self.kind = kind  # Declare, Consume, Peek, ReadField, WriteField, Assign, Borrow, Use, Block
        self.name = name
        self.reference = reference
        self.mutable = mutable
        self.body = body


def D(x): return Stmt("Declare", name=x)
def Consume(x): return Stmt("Consume", name=x)
def Peek(x): return Stmt("Peek", name=x)
def ReadField(x): return Stmt("ReadField", name=x)
def WriteField(x): return Stmt("WriteField", name=x)
def Assign(x): return Stmt("Assign", name=x)
def Borrow(r, x, m): return Stmt("Borrow", name=x, reference=r, mutable=m)
def Use(r, m): return Stmt("Use", reference=r, mutable=m)
def B(*body): return Stmt("Block", body=tuple(body))


SEEDS = [
    (D("a"), D("b"), Borrow("ra", "a", False), Use("ra", False), Peek("b"), Consume("a"), Assign("a"), ReadField("a"),
     Borrow("mb", "b", True), Use("mb", True), WriteField("b"), Consume("b"),
     B(D("c"), Borrow("rc", "c", False), Use("rc", False), Consume("c"))),
    (D("x"), Borrow("r1", "x", False), Borrow("r2", "x", False), Use("r1", False), ReadField("x"), Use("r2", False),
     Borrow("m", "x", True), Use("m", True), Use("m", True), Assign("x"), ReadField("x"), Consume("x")),
    (D("p"), B(Borrow("rp", "p", True), Use("rp", True)), Peek("p"),
     B(D("q"), Consume("q"), Assign("q"), Borrow("rq", "q", False), Use("rq", False)), Consume("p")),
    (D("s"), D("t"), Consume("s"), Assign("s"), WriteField("t"), Borrow("rt", "t", False), ReadField("t"),
     Use("rt", False), Consume("t"), Consume("s")),
    (D("u"), Borrow("mu", "u", True), Use("mu", True), WriteField("u"), Peek("u"), Borrow("su", "u", False),
     ReadField("u"), Use("su", False), Assign("u"), B(Borrow("mu2", "u", True), Use("mu2", True)), Consume("u")),
]

PRELUDE = """struct Res { v: i32; drop { } }
fn consume(r: Res) { }
fn peek(r: borrow Res) { }
fn look(r: borrow Res) { }
fn poke(r: borrow mut Res) { r.v += 1; }
fn main() {
    var total = 0;"""


def render(program):
    """The program text, and the source line of every statement (by identity)."""
    out = [PRELUDE + "\n"]
    line = out[0].count("\n") + 1
    lines = {}

    def emit(body, depth):
        nonlocal line
        for stmt in body:
            indent = " " * (4 * depth)
            if stmt.kind == "Block":
                out.append(indent + "{\n")
                line += 1
                emit(stmt.body, depth + 1)
                out.append(indent + "}\n")
                line += 1
                continue
            lines[id(stmt)] = line
            line += 1
            text = {
                "Declare": lambda s: f"var {s.name} = Res {{ v: 1 }};",
                "Consume": lambda s: f"consume({s.name});",
                "Peek": lambda s: f"peek({s.name});",
                "ReadField": lambda s: f"total += {s.name}.v;",
                "WriteField": lambda s: f"{s.name}.v = 7;",
                "Assign": lambda s: f"{s.name} = Res {{ v: 2 }};",
                "Borrow": lambda s: f"let {s.reference} = borrow {'mut ' if s.mutable else ''}{s.name};",
                "Use": lambda s: f"{'poke' if s.mutable else 'look'}({s.reference});",
            }[stmt.kind](stmt)
            out.append(indent + text + "\n")

    emit(program, 1)
    out.append("    Console.writeLine(total);\n}\n")
    return "".join(out), lines


# ---- Mutations ----

def lists(program):
    """Every statement list (the top level and each block body, in pre-order), with a way to replace it."""
    result = []

    def visit(lst, replace):
        result.append((lst, replace))
        for i, stmt in enumerate(lst):
            if stmt.kind == "Block":
                def inner(body, i=i, lst=lst, replace=replace):
                    return replace(lst[:i] + (B(*body),) + lst[i + 1:])
                visit(stmt.body, inner)

    visit(program, lambda lst: lst)
    return result


def mutate(program, random):
    all_lists = lists(program)
    for _ in range(50):
        lst, replace = all_lists[random.next(len(all_lists))]
        case = random.next(6)
        n = len(lst)
        if case == 0 and n >= 2:
            i = random.next(n - 1)
            return replace(lst[:i] + (lst[i + 1], lst[i]) + lst[i + 2:]), f"swap {i},{i + 1}"
        if case == 1 and n >= 2:
            source = random.next(n)
            target = random.next(n)
            removed = lst[:source] + lst[source + 1:]
            return replace(removed[:target] + (lst[source],) + removed[target:]), f"move {source}->{target}"
        if case == 2 and n >= 1:
            i = random.next(n)
            return replace(lst[:i] + lst[i + 1:]), f"remove {i}"
        if case == 3 and any(s.kind == "Block" for s in lst):
            blocks = [i for i in range(n) if lst[i].kind == "Block"]
            i = blocks[random.next(len(blocks))]
            return replace(lst[:i] + lst[i].body + lst[i + 1:]), f"unscope {i}"
        if case == 4 and n >= 1:
            start = random.next(n)
            count = 1 + random.next(n - start)
            wrapped = B(*lst[start:start + count])
            return replace(lst[:start] + (wrapped,) + lst[start + count:]), f"scope {start}+{count}"
        if case == 5 and any(s.kind == "Consume" for s in lst):
            moves = [i for i in range(n) if lst[i].kind == "Consume"]
            i = moves[random.next(len(moves))]
            to = i + 1 + random.next(n - i)
            # A fresh statement: statements are identified by identity when rendering.
            return replace(lst[:to] + (Consume(lst[i].name),) + lst[to:]), f"duplicate move {i}->{to}"
    return program, "none"


# ---- The oracle ----

def expect(program, lines):
    """The expected diagnostics of a straight-line program, from the rules alone (MutationTests.Oracle)."""
    name_errors = set()
    scopes = [set()]
    flat = []

    def resolve(body):
        for stmt in body:
            if stmt.kind == "Block":
                scopes.append(set())
                resolve(stmt.body)
                scopes.pop()
                continue
            flat.append(stmt)
            used = None if stmt.kind == "Declare" else (stmt.reference if stmt.kind == "Use" else stmt.name)
            if used is not None and not any(used in s for s in scopes):
                name_errors.add(("N0200", lines[id(stmt)]))
            if stmt.kind == "Declare":
                scopes[-1].add(stmt.name)
            elif stmt.kind == "Borrow":
                scopes[-1].add(stmt.reference)

    resolve(program)
    if name_errors:
        return name_errors

    errors = set()
    last_use = {}
    for i, stmt in enumerate(flat):
        if stmt.kind == "Use":
            last_use[stmt.reference] = i
    moved = set()
    reported_move = set()
    loans = []
    for i, stmt in enumerate(flat):
        line = lines[id(stmt)]

        def live(name):
            return [l for l in loans if l[1] == name and l[0] in last_use and last_use[l[0]] > i]

        def need_init(name):
            if name in moved and name not in reported_move:
                reported_move.add(name)
                errors.add(("N0250", line))

        def conflict(name, only_mutable_loans, code):
            if any(l[2] or not only_mutable_loans for l in live(name)):
                errors.add((code, line))

        k = stmt.kind
        if k == "Consume":
            need_init(stmt.name)
            conflict(stmt.name, False, "N0240")
            moved.add(stmt.name)
        elif k == "Peek":
            need_init(stmt.name)
            conflict(stmt.name, True, "N0241")
        elif k == "ReadField":
            need_init(stmt.name)
            conflict(stmt.name, True, "N0242")
        elif k == "WriteField":
            need_init(stmt.name)
            conflict(stmt.name, False, "N0240")
        elif k == "Assign":
            conflict(stmt.name, False, "N0240")
            moved.discard(stmt.name)
        elif k == "Borrow":
            need_init(stmt.name)
            conflict(stmt.name, not stmt.mutable, "N0241")
            loans.append((stmt.reference, stmt.name, stmt.mutable))
    return errors


# ---- Running ----

def mutants(seed):
    """The mutants of one seed, in order: (source, expected, applied mutations, counts toward the target)."""
    random = DotNetRandom(1000 + seed)
    seen = set()
    result = []
    resolved = attempts = 0
    while resolved < MUTANTS_PER_SEED and attempts < MUTANTS_PER_SEED * 20:
        attempts += 1
        mutant = SEEDS[seed]
        applied = []
        for _ in range(1 + random.next(3)):
            mutant, description = mutate(mutant, random)
            applied.append(description)
        source, lines = render(mutant)
        if source in seen:
            continue
        seen.add(source)
        expected = expect(mutant, lines)
        resolves = all(code != "N0200" for code, _ in expected)
        result.append((source, expected, applied, resolves))
        if resolves:
            resolved += 1
    return result


def diagnostics(compiler, source):
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "mutant.cee")
        with open(path, "w", encoding="utf-8") as f:
            f.write(source)
        run = subprocess.run([compiler, "check", "--diagnostics-format=json", path], capture_output=True, cwd=directory)
    document = json.loads(run.stdout)
    return {(d["code"], d["start"]["line"]) for d in document["diagnostics"]}


def show(diagnostics):
    return ", ".join(f"{code}@{line}" for code, line in sorted(diagnostics, key=lambda d: (d[1], d[0])))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", action="append", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()
    failed = False
    for compiler in args.compiler:
        compiler = os.path.abspath(os.path.expanduser(compiler))
        for seed in range(len(SEEDS)):
            source, lines = render(SEEDS[seed])
            if expect(SEEDS[seed], lines) or diagnostics(compiler, source):
                print(f"{compiler}: seed {seed} does not compile cleanly")
                failed = True
            work = mutants(seed)
            with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
                actual = list(pool.map(lambda m: diagnostics(compiler, m[0]), work))
            wrong = [(m, a) for m, a in zip(work, actual) if m[1] != a]
            resolved = sum(1 for m in work if m[3])
            rejected = sum(1 for m in work if m[3] and m[1])
            print(f"{os.path.basename(compiler)}: seed {seed}: {len(work)} distinct mutants; {resolved} resolve their "
                  f"names, and {rejected} of those are rejected; {len(wrong)} disagree with the oracle")
            for (mutant_source, expected, applied, _), got in wrong[:3]:
                print(f"  ({'; '.join(applied)}):\n{mutant_source}  expected: {show(expected)}\n  actual:   {show(got)}")
            if wrong or resolved != MUTANTS_PER_SEED or not resolved // 10 <= rejected <= resolved * 9 // 10:
                failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
