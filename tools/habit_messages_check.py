#!/usr/bin/env python3
"""The parser's messages for forms other languages write (spec/diagnostics.md, "Forms from other languages"):
each program below has one such form, and `ceesafec check` must report exactly one diagnostic for it: the code, on
the line of the form, with a message that names what CeeSafe writes instead. One diagnostic, because the parser goes
on as if CeeSafe's form had been written: a second one would be the cascade these messages were added to end.

The expectations are the table in spec/diagnostics.md, not output recorded from the compiler.

usage: python3 tools/habit_messages_check.py --compiler out/ceesafec
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

# (what is written, the program, the line of the form, the code, what the message must contain)
CASES = [
    [
        "if let",
        "fn ifLet(found: i32?) -> i32 {\n    if let Some(n) = found {\n        return n;\n    }\n    return 0;\n}\n",
        2,
        "N0100",
        "no 'if let'"
    ],
    [
        "if x > 2 {",
        "fn ifBare(x: i32) -> i32 {\n    if x > 2 {\n        return 1;\n    }\n    return 0;\n}\n",
        2,
        "N0100",
        "'if (x > 2) { … }'"
    ],
    [
        "while n > 2 {",
        "fn whileBare(x: i32) -> i32 {\n    var n = x;\n    while n > 2 {\n        n -= 1;\n    }\n    return n;\n}\n",
        3,
        "N0100",
        "'while (x > 2) { … }'"
    ],
    [
        "for x in xs {",
        "fn forBare(xs: Slice<i32>) -> i32 {\n    var t = 0;\n    for x in xs {\n        t += x;\n    }\n    return t;\n}\n",
        3,
        "N0100",
        "'for (x in xs) { … }'"
    ],
    [
        "match x {",
        "fn matchBare(x: i32?) -> i32 {\n    match x {\n        Some(v) => { return v; }\n        null => { return 0; }\n    }\n}\n",
        2,
        "N0100",
        "'match (x) { … }'"
    ],
    [
        "a guard without parentheses",
        "fn guardBare(x: i32?) -> i32 => match (x) {\n    Some(v) if v > 3 => v,\n    _ => 0,\n};\n",
        2,
        "N0100",
        "'Some(v) if (v > 3) => …'"
    ],
    [
        "a statement as an arm",
        "fn armStatement(x: i32?) -> i32 {\n    match (x) {\n        Some(v) => return v,\n        null => return 0,\n    }\n}\n",
        3,
        "N0100",
        "'=> { return v; }'"
    ],
    [
        "a tuple value",
        "fn tupleValue() -> i32 {\n    let p = (1, 2);\n    return 0;\n}\n",
        2,
        "N0100",
        "no tuples"
    ],
    [
        "a tuple type",
        "fn tupleType() -> (i32, i32) {\n    return 0;\n}\n",
        1,
        "N0100",
        "no tuple types"
    ],
    [
        "a type alias",
        "type Id = i32;\n",
        1,
        "N0101",
        "no type aliases"
    ],
    [
        "let mut",
        "fn letMut() -> i32 {\n    let mut n = 1;\n    n += 1;\n    return n;\n}\n",
        2,
        "N0100",
        "'var n = …;'"
    ],
    [
        "a Rust closure",
        "fn closure(x: i32) -> i32 {\n    let g = |a| a + 1;\n    return g(x);\n}\n",
        2,
        "N0102",
        "'(a) => a + 1'"
    ],
    [
        "else if without parentheses",
        "fn elseIf(x: i32) -> i32 {\n    if (x > 2) {\n        return 1;\n    } else if x > 1 {\n        return 2;\n    }\n    return 0;\n}\n",
        4,
        "N0100",
        "'if (x > 2) { … }'"
    ],
    [
        "an if expression without parentheses",
        "fn ifValue(x: i32) -> i32 {\n    let y = if x > 2 { 1 } else { 0 };\n    return y;\n}\n",
        2,
        "N0100",
        "'if (x > 2) { … }'"
    ],
    ["a & type", "fn amp(s: &str, n: &mut i32) -> i32 {\n    return 1;\n}\n", 1, "N0105", "'borrow T'"],
    ["a & expression", "fn lend(n: borrow i32) {}\n\nfn use1() {\n    let x = 1;\n    lend(&x);\n}\n", 5, "N0102", "'borrow x'"],
    ["a &mut expression", "fn lend(n: borrow mut i32) {}\n\nfn use1() {\n    var x = 1;\n    lend(&mut x);\n}\n", 5, "N0102", "'f(mut x)'"],
    ["&self", "struct P {\n    n: i32;\n\n    fn get(&self) -> i32 {\n        return this.n;\n    }\n}\n", 4, "N0100", "'borrow this'"],
    ["&mut self with a parameter", "struct P {\n    n: i32;\n\n    fn set(&mut self, v: i32) {\n        this.n = v;\n    }\n}\n", 4, "N0100", "'borrow mut this'"],
    ["self by value", "struct P {\n    n: i32;\n\n    fn get(self) -> i32 {\n        return this.n;\n    }\n}\n", 4, "N0100", "'this'"],
    ["self in a body", "struct P {\n    n: i32;\n\n    fn get(borrow this) -> i32 {\n        return self.n;\n    }\n}\n", 5, "N0200", "'this'"],
    ["None", "fn none1() {\n    let o: i32? = None;\n}\n", 2, "N0200", "'null'"],
    [".await", "async fn one() -> i32 {\n    return 1;\n}\n\nasync fn two() -> i32 {\n    return one().await;\n}\n", 6, "N0100", "'await f()'"],
    ["pub fn", "pub fn f1() -> i32 {\n    return 1;\n}\n\nfn g1() -> i32 => f1();\n", 1, "N0101", "'public'"],
    ["trait", "trait Shape {\n    fn area(&self) -> i32;\n}\n", 1, "N0101", "'interface'"],
    ["len() of an array", "fn len1() -> usize {\n    let a = [1, 2];\n    return a.len();\n}\n", 3, "N0200", "'length'"],
    ["len() of a vector", "fn len2(v: borrow Vector<i32>) -> usize => v.len();\n", 1, "N0200", "'length'"],
    ["len() of text", "fn len3(s: StringView) -> usize => s.len();\n", 1, "N0200", "'length'"]
]

# A struct literal in a parenthesised condition is still one: the recovery's rule must not reach it.
ACCEPTED = """struct P {
    a: i32;
}

fn f(x: i32) -> i32 {
    if (P { a: x }.a > 2) {
        return 1;
    }
    return 0;
}

fn main() {}
"""


def diagnostics(compiler, work, source):
    path = os.path.join(work, "main.cee")
    with open(path, "w", encoding="utf-8") as f:
        f.write(source)
    run = subprocess.run([compiler, "check", "--diagnostics-format=json", path], capture_output=True, text=True,
                         timeout=120)
    return json.loads(run.stdout)["diagnostics"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory() as work:
        for what, program, line, code, phrase in CASES:
            found = diagnostics(compiler, work, program + "\nfn main() {}\n")
            if len(found) != 1:
                problems.append(f"{what}: expected one diagnostic, got "
                                f"{[(d['code'], d['start']['line']) for d in found]}")
                continue
            d = found[0]
            if d["code"] != code or d["start"]["line"] != line:
                problems.append(f"{what}: expected {code} at line {line}, got {d['code']} at {d['start']['line']}")
            elif phrase not in d["message"]:
                problems.append(f"{what}: the message does not say {phrase!r}: {d['message']}")
        if diagnostics(compiler, work, ACCEPTED):
            problems.append("a struct literal in a parenthesised condition is no longer accepted")
    for p in problems:
        print("FAIL", p)
    print(f"habit_messages_check: {len(CASES)} forms and 1 accepted program: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
