#!/usr/bin/env python3
"""What a child receives from `Process.run`, `Process.capture` and `Process.spawn` (type-system.md §26.5, §26.7,
§26.16): checks the corpus cannot make, since its runner starts no helper programs.

A program is built that runs two children with the same awkward arguments (spaces, quotes, backslashes before
quotes and at the end, empty arguments, tabs, newlines, `%name%`, cmd.exe's metacharacters, non-ASCII text) through
each of the three, and gives `capture`'s child an input of every byte value, `\\r\\n` and a zero byte among them:

1. a Python script, which writes each argument it was given and its standard input, in hex;
2. the program itself (`Process.executable`), which writes the same. It is given the arguments that are ASCII only:
   on Windows a CeeSafe program's arguments come through the ANSI code page.

The arguments reach the program in hex, so they need no quoting on the way in, and are compared with the list this
script made: every argument must arrive as one, unchanged, and the input byte for byte. On Windows a `.bat` or
`.cmd` program must be refused (`InvalidInput`) by all three, under each spelling Windows reads as one, before
anything runs (BatBadBut, CVE-2024-24576), and `cmd.exe /d /c "exit 3"` still runs. Everywhere a missing program is
`NotFound`. Before 0.95.8 every `capture` failed on Windows, and `run` split an argument with a space in two
(ADR 0245; CEESAFE_FINDINGS 45 and 46 of the SQLite port).

usage: python3 tools/process_check.py --compiler out/ceesafec [--write DIR]
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = r'''// `check PYTHON SCRIPT BATCHES INPUT ARGUMENT...`, each but PYTHON and SCRIPT in hex after an `x` (BATCHES is a
// directory with batch files, or `-`); `check child KIND MODE ARGUMENT...` is the program as its own child.
fn nibble(c: u8) -> u8 {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return c - 'a' + 10;
}

fn decoded(text: StringView) -> Vector<u8> {
    let b = text.bytes();
    var out = Vector<u8>.create();
    var i: usize = 1;
    while (i + 1 < b.length) {
        out.push(nibble(b[i]) * 16 + nibble(b[i + 1]))!;
        i += 2;
    }
    return out;
}

fn say(kind: StringView, mode: StringView, what: StringView, bytes: Slice<u8>) {
    let digits = "0123456789abcdef".bytes();
    var line = Vector<u8>.create();
    line.extend(kind.bytes())!;
    line.push(' ')!;
    line.extend(mode.bytes())!;
    line.push(' ')!;
    line.extend(what.bytes())!;
    line.push(' ')!;
    for (b in bytes) {
        line.push(digits[(b >> 4) as usize])!;
        line.push(digits[(b & 15) as usize])!;
    }
    line.push('\n')!;
    Console.writeBytes(line.view());
}

fn child() {
    let kind = Process.argument(2) ?? "";
    let mode = Process.argument(3) ?? "";
    var i: usize = 4;
    loop {
        match (Process.argument(i)) {
            Some(a) => { say(kind, mode, "ARG", a.bytes()); }
            null => { break; }
        }
        i += 1;
    }
    var input = Vector<u8>.create();
    var buffer = [0u8; 4096];
    loop {
        let n = Console.read(mut buffer[..])!;
        if (n == 0) {
            break;
        }
        input.extend(buffer[..n])!;
    }
    say(kind, mode, "IN", input.view());
    say(kind, mode, "END", "".bytes());
    Console.flush();
}

fn status(kind: StringView, mode: StringView, result: Result<i32, IoError>) {
    Console.write(kind);
    Console.write(" ");
    Console.write(mode);
    match (result) {
        Ok(code) => {
            Console.write(" status ");
            Console.writeLine(code as i64);
        }
        Err(e) => {
            Console.write(" error ");
            Console.writeLine(
                if (e is IoError.InvalidInput) "InvalidInput" else if (e is IoError.NotFound) "NotFound" else "other",
            );
        }
    }
    Console.flush();
}

fn waited(child: Result<ChildProcess, IoError>) -> Result<i32, IoError> {
    var c = child?;
    return match (c.wait()?) {
        ChildExit.Status(code) => Ok(code),
        ChildExit.Signal(number) => Ok(128 + number),
    };
}

fn captured(kind: StringView, output: Result<ProcessOutput, IoError>) -> Result<i32, IoError> {
    let o = output?;
    Console.writeBytes(o.stdout.view());
    Console.writeBytes(o.stderr.view());
    return Ok(o.status);
}

// The three ways, with `program lead kind mode arguments...`.
fn exercise(
    program: StringView,
    lead: StringView,
    kind: StringView,
    rest: borrow Vector<StringView>,
    input: Slice<u8>,
) {
    let quiet = ChildOptions { input: ChildStream.Null, output: ChildStream.Inherit, error: ChildStream.Inherit };
    var list = Vector<StringView>.create();
    list.push(lead)!;
    list.push(kind)!;
    list.push("run")!;
    for (a in rest) {
        list.push(a)!;
    }
    status(kind, "run", Process.run(program, list));
    list[2] = "capture";
    status(kind, "capture", captured(kind, Process.capture(program, list, Some(input))));
    list[2] = "spawn";
    status(kind, "spawn", waited(Process.spawn(program, list, quiet)));
}

fn refusals(directory: Slice<u8>) {
    var none = Vector<StringView>.create();
    let names: [StringView; 6] = ["t.bat", "T.CMD", "t.bat.", "t.cmd. .", "t.bat::$DATA", "T.Bat"];
    for (k in 0..6) {
        var path = Vector<u8>.create();
        path.extend(directory)!;
        path.push('\\')!;
        path.extend(names[k].bytes())!;
        let program = StringView.fromUtf8(path.view())!;
        status("batch", names[k], Process.run(program, none));
        status("batch", names[k], captured("batch", Process.capture(program, none, null)));
        status("batch", names[k], waited(Process.spawn(program, none, ChildOptions.quiet())));
    }
    var exit = Vector<StringView>.create();
    exit.push("/d")!;
    exit.push("/c")!;
    exit.push("exit 3")!;
    status("cmd", "run", Process.run("cmd.exe", exit));
}

fn main() {
    let first = Process.argument(1) ?? "";
    if (first == "child") {
        child();
        return;
    }
    let python = first;
    let script = Process.argument(2)!;
    let batches = Process.argument(3)!;
    let input = decoded(Process.argument(4)!);
    var raw = Vector<Vector<u8>>.create();
    var i: usize = 5;
    loop {
        match (Process.argument(i)) {
            Some(a) => { raw.push(decoded(a))!; }
            null => { break; }
        }
        i += 1;
    }
    var all = Vector<StringView>.create();
    var ascii = Vector<StringView>.create();
    for (r in raw.view()) {
        let text = StringView.fromUtf8(r.view())!;
        all.push(text)!;
        var plain = true;
        for (b in r.view()) {
            if (b >= 128) {
                plain = false;
            }
        }
        if (plain) {
            ascii.push(text)!;
        }
    }
    exercise(python, script, "py", all, input.view());
    let me = Process.executable()!;
    exercise(me.view(), "child", "self", ascii, input.view());
    var none = Vector<StringView>.create();
    status("missing", "run", Process.run("no-such-program-anywhere", none));
    status("missing", "capture", captured("missing", Process.capture("no-such-program-anywhere", none, null)));
    status("missing", "spawn", waited(Process.spawn("no-such-program-anywhere", none, ChildOptions.quiet())));
    if (batches != "-") {
        let directory = decoded(batches);
        refusals(directory.view());
    }
    Console.writeLine("done");
}
'''

CHILD = r'''import sys
kind, mode, arguments = sys.argv[1], sys.argv[2], sys.argv[3:]
out = sys.stdout.buffer
for a in arguments:
    out.write(f"{kind} {mode} ARG {a.encode('utf-8', 'surrogateescape').hex()}\n".encode())
data = sys.stdin.buffer.read()
out.write(f"{kind} {mode} IN {data.hex()}\n".encode())
out.write(f"{kind} {mode} END \n".encode())
out.flush()
'''

ARGUMENTS = [
    "plain",
    "two words",
    "",
    '"',
    'say "hi"',
    "back\\slash",
    "trail\\",
    "trail space\\",
    '\\\\"',
    'a\\\\\\"b',
    "\\\\server\\share\\",
    "tab\there",
    "new\nline",
    "%PATH%",
    "^&|<>()!",
    "*?",
    "-x",
    '""',
    " ",
    "",
    "ünïcødé €",
    "end",
]
INPUT = b"line one\r\nline two\n\x00\x1a" + bytes(range(256)) * 20
BATCH_NAMES = ["t.bat", "T.CMD", "t.bat.", "t.cmd. .", "t.bat::$DATA", "T.Bat"]
WINDOWS = os.name == "nt"
DEADLINE = 120.0


def expected():
    lines = []
    for kind, arguments in (("py", ARGUMENTS), ("self", [a for a in ARGUMENTS if a.isascii()])):
        for mode in ("run", "capture", "spawn"):
            for a in arguments:
                lines.append(f"{kind} {mode} ARG {a.encode('utf-8').hex()}")
            lines.append(f"{kind} {mode} IN {(INPUT if mode == 'capture' else b'').hex()}")
            lines.append(f"{kind} {mode} END ")
            lines.append(f"{kind} {mode} status 0")
    for mode in ("run", "capture", "spawn"):
        lines.append(f"missing {mode} error NotFound")
    if WINDOWS:
        for name in BATCH_NAMES:
            lines.extend([f"batch {name} error InvalidInput"] * 3)
        lines.append("cmd run status 3")
    lines.append("done")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--write", metavar="DIR", help="only write the programs to DIR (to try them by hand)")
    args = parser.parse_args()
    if args.write:
        os.makedirs(args.write, exist_ok=True)
        for name, source in (("check.cee", PROGRAM), ("child.py", CHILD)):
            with open(os.path.join(args.write, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(source)
        print(f"process_check: wrote 2 programs to {args.write}")
        return 0
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-process-") as work:
        source = os.path.join(work, "check.cee")
        script = os.path.join(work, "child.py")
        for path, text in ((source, PROGRAM), (script, CHILD)):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        batches = "-"
        if WINDOWS:
            # Batch files that would say they ran: none of them may run.
            for name in ("t.bat", "t.cmd"):
                with open(os.path.join(work, name), "w", encoding="ascii", newline="\r\n") as f:
                    f.write("@echo off\necho batch file ran %*\n")
            batches = "x" + work.encode("utf-8").hex()
        exe = os.path.join(work, "check.exe" if WINDOWS else "check")
        b = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {(b.stdout + b.stderr).strip()[:800]}")
        else:
            command = [exe, sys.executable, script, batches, "x" + INPUT.hex()]
            command += ["x" + a.encode("utf-8").hex() for a in ARGUMENTS]
            try:
                r = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=DEADLINE)
                got = [line.rstrip(b"\r").decode("utf-8", "replace") for line in r.stdout.split(b"\n") if line]
                want = expected()
                if got != want:
                    for k in range(max(len(got), len(want))):
                        g = got[k] if k < len(got) else "<nothing>"
                        w = want[k] if k < len(want) else "<nothing>"
                        if g != w:
                            problems.append(f"line {k + 1}: got {g[:200]!r}, expected {w[:200]!r}")
                    problems.append(f"stderr: {r.stderr.decode('utf-8', 'replace').strip()[:600]!r}")
                if r.returncode != 0:
                    problems.append(f"the program's status was {r.returncode}")
            except subprocess.TimeoutExpired:
                problems.append("the program did not end in time")
    for problem in problems[:60]:
        print(f"FAIL {problem}")
    print(f"process_check: {len(expected())} lines: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
