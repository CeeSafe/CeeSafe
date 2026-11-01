#!/usr/bin/env python3
"""Model-based property tests of the built-in collections (ROADMAP, the collections' properties).

For each collection (vector, string, slotmap, stablevector, views) and seed 1..6, a generator writes a CeeSafe program of
80 random operations while a model computes what it must print: values, lengths, capacities (§23.1 growth), stale
handles, slices and the order of every drop. The generator draws from .NET's System.Random(seed), reproduced in
mutation_suite.py, so the programs are fixed by their seeds. Each program is built and run under the given C flags,
and must print exactly the model's output and exit 0; with --sweep, it also passes the fault-injection sweep.

usage: python3 tools/collection_properties.py --compiler out/ceesafec [--sweep]
           [--cflag=FLAG]... [--keep DIR]
"""
import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutation_suite import DotNetRandom  # noqa: E402
import fault_injection  # noqa: E402

OPERATIONS = 80
KINDS = ("vector", "string", "slotmap", "stablevector", "views")


def next_range(random, low, high):
    """Random.Next(minValue, maxValue) for a range that fits in an int."""
    return int(random._sample() * (1.0 / 2147483647) * (high - low)) + low


def grow(length, capacity, extra):
    need = length + extra
    return capacity if need <= capacity else max(max(capacity * 2, 4), need)


def boundaries(text):
    result, offset = [], 0
    for ch in text:
        result.append(offset)
        offset += len(ch.encode("utf-8"))
    result.append(offset)
    return result


class Generator:
    def __init__(self):
        self.program = []
        self.stdout = []
        self.names = 0

    def line(self, code):
        self.program.append("    " + code + "\n")

    def expect(self, value):
        self.stdout.append(("true" if value else "false") if isinstance(value, bool) else str(value))
        self.stdout.append("\n")

    def fresh(self, prefix):
        name = f"{prefix}{self.names}"
        self.names += 1
        return name

    def vector(self, random):
        self.program.append(
            "struct Item { id: i32; drop { Console.write(\"d\"); Console.writeLine(this.id); } }\n\n"
            "fn sumIds(xs: Slice<Item>) -> i32 {\n    var total = 0;\n"
            "    for (i in 0..xs.length) { total += xs[i].id; }\n    return total;\n}\n\n"
            "fn main() {\n    var v = Vector<Item>.create();\n")
        model, capacity, next_id = [], 0, 1
        for _ in range(OPERATIONS):
            choice = random.next(100)
            if choice < 40 or (len(model) == 0 and choice < 70):
                capacity = grow(len(model), capacity, 1)
                model.append(next_id)
                self.line(f"v.push(Item {{ id: {next_id} }})!;")
                next_id += 1
            elif choice < 52:
                if not model:
                    self.line("Console.writeLine(v.pop() == null);")
                    self.expect("true")
                else:
                    last = model.pop()
                    self.line("Console.writeLine(v.pop()!.id);")
                    self.expect(last)
                    self.expect(f"d{last}")
            elif choice < 64 and model:
                i = random.next(len(model))
                self.expect(f"d{model[i]}")  # the old element is destroyed by the assignment
                model[i] = next_id
                self.line(f"v[{i}] = Item {{ id: {next_id} }};")
                next_id += 1
            elif choice < 74 and model:
                i = random.next(len(model))
                name = self.fresh("r")
                self.line(f"let {name} = borrow v[{i}];")
                self.line(f"Console.writeLine({name}.id);")
                self.expect(model[i])
            elif choice < 82:
                self.line("Console.writeLine(v.length);")
                self.expect(len(model))
                self.line("Console.writeLine(v.capacity);")
                self.expect(capacity)
            elif choice < 92:
                self.line("Console.writeLine(sumIds(v));")
                self.expect(sum(model))
            elif choice < 95:
                self.line("v.clear();")
                for i in model:
                    self.expect(f"d{i}")
                model = []
            else:
                self.line("Console.writeLine(v.view().length);")
                self.expect(len(model))
        for i in model:
            self.expect(f"d{i}")  # `v` is destroyed at the end of `main`, elements in index order
        self.program.append("}\n")

    def string(self, random):
        pieces = ["a", "bc", "é", "日本", "", "xyz", "ñ", "Ω", "🙂"]
        self.program.append("fn main() {\n    var s = String.create();\n")
        model, capacity = "", 0
        for _ in range(OPERATIONS):
            data = model.encode("utf-8")
            choice = random.next(100)
            if choice < 40:
                piece = pieces[random.next(len(pieces))]
                capacity = grow(len(data), capacity, len(piece.encode("utf-8")))
                model += piece
                self.line(f"s.append(\"{piece}\")!;")
            elif choice < 50:
                self.line("Console.writeLine(s);")
                self.expect(model)
            elif choice < 60:
                self.line("Console.writeLine(s.length);")
                self.expect(len(data))
                self.line("Console.writeLine(s.capacity);")
                self.expect(capacity)
            elif choice < 75:
                b = boundaries(model)
                lo = b[random.next(len(b))]
                hi = b[random.next(len(b))]
                lo, hi = min(lo, hi), max(lo, hi)
                self.line(f"Console.writeLine(s[{lo}..{hi}]);")
                self.expect(data[lo:hi].decode("utf-8"))
            elif choice < 85 and data:
                i = random.next(len(data))
                self.line(f"Console.writeLine(s[{i}]);")
                self.expect(data[i])
            elif choice < 95:
                same = random.next(2) == 0
                text = model if same else model + "x"
                self.line(f"Console.writeLine(s.view() == \"{text}\");")
                self.expect(same)
            else:
                self.line("s.clear();")
                model = ""
        self.program.append("}\n")

    def views(self, random):
        pieces = ["ab", "é", "日本", "xyz", "ñ", "🙂", "q"]
        text = "".join(pieces[random.next(len(pieces))] for _ in range(12))
        self.program.append("fn main() {\n")
        self.line("var text = String.create();")
        self.line(f"text.append(\"{text}\")!;")
        self.line("let base = text.view();")
        self.line("var v = Vector<StringView>.create();")
        data = text.encode("utf-8")
        b = boundaries(text)

        def piece():
            lo = b[random.next(len(b))]
            hi = b[random.next(len(b))]
            lo, hi = min(lo, hi), max(lo, hi)
            return lo, hi, data[lo:hi].decode("utf-8")

        model = []
        for _ in range(OPERATIONS):
            choice = random.next(100)
            if choice < 40 or (not model and choice < 60):
                lo, hi, s = piece()
                model.append(s)
                self.line(f"v.push(base[{lo}..{hi}])!;")
            elif choice < 55 and model:
                i = random.next(len(model))
                self.line(f"Console.writeLine(v[{i}]);")
                self.expect(model[i])
            elif choice < 65 and model:
                i = random.next(len(model))
                lo, hi, s = piece()
                model[i] = s
                self.line(f"v[{i}] = base[{lo}..{hi}];")
            elif choice < 75:
                if not model:
                    self.line("Console.writeLine(v.pop() == null);")
                    self.expect("true")
                else:
                    self.line("Console.writeLine(v.pop()!);")
                    self.expect(model.pop())
            elif choice < 85:
                self.line("Console.writeLine(v.length);")
                self.expect(len(model))
            else:
                name = self.fresh("t")
                self.line(f"var {name}: usize = 0;")
                self.line(f"for (part in v) {{ {name} += part.length; }}")
                self.line(f"Console.writeLine({name});")
                self.expect(sum(len(m.encode("utf-8")) for m in model))
        self.line("Console.writeLine(v.length);")
        self.expect(len(model))
        self.line("text.clear(); // the vector and its views are dead here")
        self.line("Console.writeLine(text.length);")
        self.expect(0)
        self.program.append("}\n")

    def slotmap(self, random):
        self.program.append("fn main() {\n    var m = SlotMap<i32>.create();\n    var hs = Vector<Handle<i32>>.create();\n")
        handles = []
        for _ in range(OPERATIONS):
            choice = random.next(100)
            if choice < 35 or not handles:
                value = next_range(random, -1000, 1000)
                handles.append([value, True])
                self.line(f"hs.push(m.insert({value})!)!;")
                continue
            j = random.next(len(handles))
            current, live = handles[j]
            if choice < 55:
                name = self.fresh("g")
                self.line(f"match (m.get(hs[{j}])) {{ Some({name}) => {{ Console.writeLine({name}); }} "
                          f"null => {{ Console.writeLine(\"null\"); }} }}")
                self.expect(current if live else "null")
            elif choice < 70:
                name = self.fresh("x")
                self.line(f"match (m.remove(hs[{j}])) {{ Some({name}) => {{ Console.writeLine({name}); }} "
                          f"null => {{ Console.writeLine(\"null\"); }} }}")
                self.expect(current if live else "null")
                handles[j] = [current, False]
            elif choice < 82:
                value = next_range(random, -1000, 1000)
                name = self.fresh("w")
                self.line(f"match (m.getMut(hs[{j}])) {{ Some({name}) => {{ {name} = {value}; }} null => {{ }} }}")
                if live:
                    handles[j] = [value, True]
            elif choice < 92:
                self.line(f"Console.writeLine(m.contains(hs[{j}]));")
                self.expect(live)
            else:
                self.line("Console.writeLine(m.length);")
                self.expect(sum(1 for _, alive in handles if alive))
        self.program.append("}\n")

    def stablevector(self, random):
        self.program.append("fn main() {\n    var sv = StableVector<i32>.create();\n")
        model = []
        for _ in range(OPERATIONS):
            choice = random.next(100)
            if choice < 40 or not model:
                value = next_range(random, -1000, 1000)
                self.line(f"Console.writeLine(sv.push({value})!);")
                self.expect(len(model))
                model.append(value)
            elif choice < 55:
                i = random.next(len(model))
                self.line(f"Console.writeLine(sv[{i}]);")
                self.expect(model[i])
            elif choice < 70:
                i = random.next(len(model))
                value = next_range(random, -1000, 1000)
                self.line(f"sv[{i}] = {value};")
                model[i] = value
            elif choice < 85:
                # A borrow held across pushes, which may allocate new chunks (§24.2).
                i = random.next(len(model))
                name = self.fresh("p")
                self.line(f"let {name} = borrow sv[{i}];")
                for _ in range(next_range(random, 1, 20)):
                    value = next_range(random, -1000, 1000)
                    self.line(f"sv.push({value})!;")
                    model.append(value)
                self.line(f"Console.writeLine({name});")
                self.expect(model[i])
            else:
                self.line("Console.writeLine(sv.length);")
                self.expect(len(model))
        self.program.append("}\n")


def generate(kind, seed):
    generator = Generator()
    getattr(generator, kind)(DotNetRandom(seed))
    return "".join(generator.program), "".join(generator.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sweep", action="store_true")
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--keep", help="write the programs into this directory and keep them")
    args = parser.parse_args()
    compiler = os.path.abspath(os.path.expanduser(args.compiler))
    problems, points = [], 0
    with tempfile.TemporaryDirectory() as temporary:
        directory = os.path.abspath(args.keep) if args.keep else temporary
        os.makedirs(os.path.join(directory, "run-pass"), exist_ok=True)
        for kind in KINDS:
            for seed in range(1, 7):
                program, stdout = generate(kind, seed)
                relative = f"run-pass/property-{kind}-{seed}.cee"
                with open(os.path.join(directory, relative), "w", encoding="utf-8") as f:
                    f.write(program)
                exe = os.path.join(temporary, f"property-{kind}-{seed}")
                flags = [x for flag in args.cflag for x in ("--cflag", flag)]
                built = subprocess.run([compiler, "build"] + flags + [relative, "-o", exe], cwd=directory,
                                       capture_output=True, text=True)
                if built.returncode != 0:
                    problems.append(f"{relative}: build failed: {built.stderr[:800]}")
                    continue
                ran = subprocess.run([exe], capture_output=True, timeout=60)
                if ran.returncode != 0 or ran.stdout.decode("utf-8") != stdout:
                    problems.append(f"{relative}: status {ran.returncode}, output differs from the model")
                if args.sweep:
                    _, attempts, failures = fault_injection.sweep((compiler, None, directory, relative))
                    points += len(fault_injection.failure_points(attempts)) if attempts else 0
                    problems += [f"{relative}: {f}" for f in failures]
    for p in problems:
        print("FAIL", p)
    print(f"collection_properties: {len(KINDS) * 6} programs"
          f"{f', {points} failure points swept' if args.sweep else ''}: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
