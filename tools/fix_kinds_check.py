#!/usr/bin/env python3
"""The fixes of spec/tooling.md §5.2, kind by kind: `ceesafec fix` turns each faulty program into the text the table's
row describes, and that text then checks with no error ("applying a fix removes that diagnostic and introduces none
at the edited place"). The expected texts are written from the table, not taken from the compiler.

usage: python3 tools/fix_kinds_check.py --compiler out/ceesafec
"""
import argparse
import os
import subprocess
import sys
import tempfile

# (the row, the code, the program, the program fixed)
CASES = [
    ("mut before a borrow mut argument, and let to var", "N0249",
     "fn bump(x: borrow mut i32) {\n    x += 1;\n}\n\nfn main() {\n    let n = 1;\n    bump(n);\n    Console.writeLine(n);\n}\n",
     "fn bump(x: borrow mut i32) {\n    x += 1;\n}\n\nfn main() {\n    var n = 1;\n    bump(mut n);\n    Console.writeLine(n);\n}\n"),
    ("let to var for an assigned local", "N0211",
     "fn main() {\n    let x = 1;\n    x = 2;\n    Console.writeLine(x);\n}\n",
     "fn main() {\n    var x = 1;\n    x = 2;\n    Console.writeLine(x);\n}\n"),
    ("truncate for a narrowing as", "N0208",
     "fn main() {\n    let wide: u32 = 300;\n    let b = wide as u8;\n    Console.writeLine(b);\n}\n",
     "fn main() {\n    let wide: u32 = 300;\n    let b = truncate<u8>(wide);\n    Console.writeLine(b);\n}\n"),
    ("borrow before a place where a reference is expected", "N0205",
     "fn main() {\n    let n: i32 = 5;\n    let r: borrow i32 = n;\n    Console.writeLine(r + 0);\n}\n",
     "fn main() {\n    let n: i32 = 5;\n    let r: borrow i32 = borrow n;\n    Console.writeLine(r + 0);\n}\n"),
    ("borrow mut before a place, then let to var (N0248)", "N0205",
     "fn main() {\n    let n: i32 = 5;\n    let r: borrow mut i32 = n;\n    r += 1;\n    Console.writeLine(n);\n}\n",
     "fn main() {\n    var n: i32 = 5;\n    let r: borrow mut i32 = borrow mut n;\n    r += 1;\n    Console.writeLine(n);\n}\n"),
    ("as after a place whose number widens without loss", "N0205",
     "struct P {\n    x: i32;\n}\n\nfn main() {\n    let p = P { x: 5 };\n    let n: u8 = 7;\n    let z: i64 = p.x;\n    let f: f64 = n;\n    Console.writeLine(z);\n    Console.writeLine(f);\n}\n",
     "struct P {\n    x: i32;\n}\n\nfn main() {\n    let p = P { x: 5 };\n    let n: u8 = 7;\n    let z: i64 = p.x as i64;\n    let f: f64 = n as f64;\n    Console.writeLine(z);\n    Console.writeLine(f);\n}\n"),
    ("asBorrow for a borrow of an optional", "N0205",
     "fn main() {\n    let o: i32? = Some(3);\n    let r: borrow i32? = borrow o;\n    match (r) {\n        Some(v) => { Console.writeLine(v + 0); }\n        null => {}\n    }\n}\n",
     "fn main() {\n    let o: i32? = Some(3);\n    let r: borrow i32? = o.asBorrow();\n    match (r) {\n        Some(v) => { Console.writeLine(v + 0); }\n        null => {}\n    }\n}\n"),
    ("as after each operand of a sum that widens", "N0205",
     "fn main() {\n    let a: i32 = 1;\n    let z: i64 = a + a;\n    Console.writeLine(z);\n}\n",
     "fn main() {\n    let a: i32 = 1;\n    let z: i64 = a as i64 + a as i64;\n    Console.writeLine(z);\n}\n"),
    ("the declared type for a var that copies through a reference", "N0246",
     "fn next(len: borrow mut usize) -> usize {\n    var k = len;\n    k += 1;\n    return k;\n}\n\nfn main() {\n    var n: usize = 1;\n    Console.writeLine(next(mut n));\n}\n",
     "fn next(len: borrow mut usize) -> usize {\n    var k: usize = len;\n    k += 1;\n    return k;\n}\n\nfn main() {\n    var n: usize = 1;\n    Console.writeLine(next(mut n));\n}\n"),
]

# What must get no fix: a widening of what is not a variable or a field (the row's condition), and a narrowing.
NO_FIX = [
    ("a call's result that would widen",
     "fn one() -> i32 => 1;\n\nfn main() {\n    let z: i64 = one();\n    Console.writeLine(z);\n}\n"),
    ("a number that would narrow", "fn main() {\n    let a: i64 = 1;\n    let z: i32 = a;\n    Console.writeLine(z);\n}\n"),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-fixes-") as work:
        path = os.path.join(work, "main.cee")

        def run(*command):
            return subprocess.run([compiler, *command, "main.cee"], cwd=work, capture_output=True, text=True, timeout=120)

        for row, code, fault, fixed in CASES:
            with open(path, "w", encoding="utf-8") as f:
                f.write(fault)
            before = run("check")
            if f"error[{code}]" not in before.stderr + before.stdout:
                problems.append(f"{row}: the program does not get {code}")
                continue
            run("fix")
            with open(path, encoding="utf-8") as f:
                after = f.read()
            if after != fixed:
                problems.append(f"{row}: fixed to\n{after}expected\n{fixed}")
            checked = run("check")
            if checked.returncode != 0:
                problems.append(f"{row}: the fixed program does not check: {(checked.stderr + checked.stdout)[:300]}")
        for row, fault in NO_FIX:
            with open(path, "w", encoding="utf-8") as f:
                f.write(fault)
            run("fix")
            with open(path, encoding="utf-8") as f:
                if f.read() != fault:
                    problems.append(f"{row}: a fix was applied where none is sure")
    for p in problems:
        print("FAIL", p)
    print(f"fix_kinds_check: {len(CASES)} fixes, {len(NO_FIX)} without: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
