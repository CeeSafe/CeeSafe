#!/usr/bin/env python3
"""What a child receives from `Process.run`, `Process.capture` and `Process.spawn` (type-system.md §26.5, §26.7,
§26.16), names that are not text (ADR 0257), a child's environment (ADR 0258), and how a program is found
(ADR 0259): checks the corpus cannot make, since its runner starts no helper programs.

A program is built that runs two children with the same awkward arguments (spaces, quotes, backslashes before
quotes and at the end, empty arguments, tabs, newlines, `%name%`, cmd.exe's metacharacters, non-ASCII text) through
each of the three, and gives `capture`'s child an input of every byte value, `\\r\\n` and a zero byte among them:

1. a Python script, which writes each argument it was given and its standard input, in hex;
2. the program itself (`Process.executable`), which writes the same. Since ADR 0259 it is given every argument, the
   non-ASCII ones too: on Windows a CeeSafe program reads its command line in UTF-16 (before, the ANSI code page's).

The arguments reach the program in hex, so they need no quoting on the way in, and are compared with the list this
script made: every argument must arrive as one, unchanged, and the input byte for byte. On Windows a `.bat` or
`.cmd` program must be refused (`InvalidInput`) by all three, under each spelling Windows reads as one, before
anything runs (BatBadBut, CVE-2024-24576), and `cmd.exe /d /c "exit 3"` still runs. Everywhere a missing program is
`NotFound`. Before 0.95.8 every `capture` failed on Windows, and `run` split an argument with a space in two
(ADR 0245; CEESAFE_FINDINGS 45 and 46 of the SQLite port).

Then, each against what Python's `os` sees:

- **Names that are not text** (ADR 0257). On Linux and FreeBSD two file names with a byte that is not UTF-8
  (`a\\xffb.txt`), on Windows two with an unpaired surrogate (`a\\udc80b.txt`, WTF-8 `61 ed b2 80 62 …`): the program
  creates the first, writes it, asks its kind, lists the directory with `listOs` (and `list`, which must refuse it),
  renames it to the second and reads it back; Python then finds the second with the program's bytes. Both names are
  given to both children as `OsStringView` arguments through `runWith`, `captureWith` and `spawn`, and arrive as
  they were. macOS's file systems refuse a name that is not UTF-8 (`EILSEQ`, which Python's `open` meets too): there
  the create is `InvalidInput` and the files are skipped, which Python's own attempt decides. The program is also
  given one such argument directly (bytes on POSIX, a lone surrogate on Windows): `Process.argumentOs` has it,
  `Process.argument` is null.
- **A child's environment** (ADR 0258). The program starts itself with options that set (an ASCII, a non-ASCII and
  an empty value), set a name twice, remove a variable, set HOME (the SQLite port's case), and set a name in lower
  case (on Windows it replaces the variable whose name differs only in case), through `runWith`, `captureWith` and
  `spawn`; the child prints what `Environment.variableOs` gives for each name. A cleared environment with one set,
  and the names Windows and POSIX refuse (`A=B`, an empty name, `CEESAFE_HANDOVER`, a zero byte in a value), which
  must be `InvalidInput`.
- **The child's PATH** (ADR 0259 §2.4): with the options' PATH set to a directory holding a copy of the program as
  `ceesafe-onchild`, `runWith`, `captureWith` and `spawn` find it, and a copy only on this process's PATH
  (`ceesafe-onparent`) is `NotFound`; without the option, or with PATH set and then removed, the reverse.
- **Finding a program** (ADR 0259). A copy of the program is planted in its working directory as `ceesafe-planted`
  (and on Windows as `cmd.exe`): a bare `ceesafe-planted` is `NotFound` from `run`, `runWith` and `spawn`, `./` (`.\\`)
  runs it, and on Windows `cmd.exe` is System32's, not the planted one.

usage: python3 tools/process_check.py --compiler out/ceesafec [--write DIR]
"""

import argparse
import errno
import os
import shutil
import subprocess
import sys
import tempfile

PROGRAM = r'''// `check PYTHON SCRIPT BATCHES INPUT WORK NAME1 NAME2 RAW ARGUMENT...`: each of BATCHES (a directory with batch files,
// or `-`), INPUT, WORK (a directory for the named files, or `-`), NAME1, NAME2 and the ARGUMENTs in hex after an `x`;
// RAW as it is, bytes that are not text. `check child KIND MODE ARGUMENT...`, `check osChild KIND MODE ARGUMENT...` and
// `check envChild MODE NAME...` are the program as its own child; `check planted` (or `/d`) the planted copy.
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

fn hex(bytes: Slice<u8>) -> Vector<u8> {
    let digits = "0123456789abcdef".bytes();
    var line = Vector<u8>.create();
    for (b in bytes) {
        line.push(digits[(b >> 4) as usize])!;
        line.push(digits[(b & 15) as usize])!;
    }
    return line;
}

fn say(kind: StringView, mode: StringView, what: StringView, bytes: Slice<u8>) {
    var line = Vector<u8>.create();
    line.extend(kind.bytes())!;
    line.push(' ')!;
    line.extend(mode.bytes())!;
    line.push(' ')!;
    line.extend(what.bytes())!;
    line.push(' ')!;
    line.extend(hex(bytes).view())!;
    line.push('\n')!;
    Console.writeBytes(line.view());
}

fn child(os: bool) {
    let kind = Process.argument(2) ?? "";
    let mode = Process.argument(3) ?? "";
    var i: usize = 4;
    loop {
        if (os) {
            match (Process.argumentOs(i)) {
                Some(a) => { say(kind, mode, "ARG", a.bytes()); }
                null => { break; }
            }
        } else {
            match (Process.argument(i)) {
                Some(a) => { say(kind, mode, "ARG", a.bytes()); }
                null => { break; }
            }
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

fn environmentChild() {
    let mode = Process.argument(2) ?? "";
    var i: usize = 3;
    loop {
        let name = Process.argument(i) ?? { break; };
        match (Environment.variableOs(name)) {
            Some(value) => { say("env", mode, name, value.bytes()); }
            null => {
                Console.write("env ");
                Console.write(mode);
                Console.write(" ");
                Console.write(name);
                Console.writeLine(" unset");
            }
        }
        i += 1;
    }
    Console.flush();
}

fn kindOf(e: IoError) -> StringView =>
    if (e is IoError.InvalidInput) "InvalidInput" else if (e is IoError.NotFound) "NotFound"
        else if (e is IoError.Unsupported) "Unsupported" else "other";

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
            Console.writeLine(kindOf(e));
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

fn captured(output: Result<ProcessOutput, IoError>) -> Result<i32, IoError> {
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
    let quiet = ChildOptions.streams(ChildStream.Null, ChildStream.Inherit, ChildStream.Inherit);
    var list = Vector<StringView>.create();
    list.push(lead)!;
    list.push(kind)!;
    list.push("run")!;
    for (a in rest) {
        list.push(a)!;
    }
    status(kind, "run", Process.run(program, list));
    list[2] = "capture";
    status(kind, "capture", captured(Process.capture(program, list, Some(input))));
    list[2] = "spawn";
    status(kind, "spawn", waited(Process.spawn(program, list, quiet)));
}

// The same with OS-string arguments (ADR 0258 §2.4), through runWith, captureWith and spawn.
fn exerciseOs(program: OsStringView, lead: StringView, kind: StringView, rest: borrow Vector<OsStringView>) {
    let quiet = ChildOptions.streams(ChildStream.Null, ChildStream.Inherit, ChildStream.Inherit);
    var list = Vector<OsStringView>.create();
    list.push(OsStringView.fromText(lead))!;
    list.push(OsStringView.fromText(kind))!;
    list.push(OsStringView.fromText("runWith"))!;
    for (a in rest) {
        list.push(a)!;
    }
    status(kind, "runWith", Process.runWith(program, list, quiet));
    list[2] = OsStringView.fromText("captureWith");
    status(kind, "captureWith", captured(Process.captureWith(program, list, null, quiet)));
    list[2] = OsStringView.fromText("spawn");
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
        status("batch", names[k], captured(Process.capture(program, none, null)));
        status("batch", names[k], waited(Process.spawn(program, none, ChildOptions.quiet())));
    }
    var exit = Vector<StringView>.create();
    exit.push("/d")!;
    exit.push("/c")!;
    exit.push("exit 3")!;
    status("cmd", "run", Process.run("cmd.exe", exit));
}

// Names that are not text (ADR 0257): made, listed, renamed and read in WORK, and given to both children.
fn names(me: OsStringView, python: StringView, script: StringView, work: Slice<u8>, first: Slice<u8>, second: Slice<u8>) {
    var a = OsString.copyOf(work)!;
    a.append("/")!;
    a.appendBytes(first)!;
    var b = OsString.copyOf(work)!;
    b.append("/")!;
    b.appendBytes(second)!;
    var made = true;
    match (File.create(a.view())) {
        Ok(file) => {
            var f = file;
            f.write("ok".bytes())!;
            Console.writeLine("os create ok");
        }
        Err(e) => {
            Console.write("os create error ");
            Console.writeLine(kindOf(e));
            made = false;
        }
    }
    if (made) {
        Console.writeLine(if (File.kind(a.view())! == Some(PathKind.File)) "os kind file" else "os kind ?");
        let listed = Directory.listOs(OsStringView.fromBytes(work))!;
        for (name in listed) {
            say("os", "list", "name", name.bytes());
        }
        match (Directory.list(OsStringView.fromBytes(work))) {
            Ok(_) => { Console.writeLine("os text list ok"); }
            Err(e) => {
                Console.write("os text list error ");
                Console.writeLine(kindOf(e));
            }
        }
        File.rename(a.view(), b.view())!;
        var f = File.open(b.view())!;
        var buffer = [0u8; 16];
        let n = f.read(mut buffer[..])!;
        say("os", "read", "bytes", buffer[..n]);
    }
    var rest = Vector<OsStringView>.create();
    rest.push(OsStringView.fromBytes(first))!;
    rest.push(OsStringView.fromBytes(second))!;
    exerciseOs(OsStringView.fromText(python), script, "pyos", rest);
    exerciseOs(me, "osChild", "selfos", rest);
}

// A child's environment (ADR 0258), seen by the program as its own child.
fn environment(me: OsStringView) {
    let asked: [StringView; 8] = [
        "CEESAFE_SET", "CEESAFE_WIDE", "CEESAFE_EMPTY", "CEESAFE_GONE", "CEESAFE_KEEP", "CEESAFE_TWICE", "HOME",
        "CEESAFE_ONLY",
    ];
    var list = Vector<StringView>.create();
    list.push("envChild")!;
    list.push("run")!;
    for (k in 0..8) {
        list.push(asked[k])!;
    }
    var options = ChildOptions.streams(ChildStream.Null, ChildStream.Inherit, ChildStream.Inherit);
    options.setEnvironment("CEESAFE_SET", "value")!;
    options.setEnvironment("CEESAFE_WIDE", "välue €")!;
    options.setEnvironment("CEESAFE_EMPTY", "")!;
    options.removeEnvironment("CEESAFE_GONE")!;
    options.setEnvironment("CEESAFE_TWICE", "1")!;
    options.setEnvironment("CEESAFE_TWICE", "2")!;
    options.setEnvironment("ceesafe_keep", "lower")!;
    options.setEnvironment("HOME", "/home/elsewhere")!;
    status("env", "run", Process.runWith(me, list, options));
    list[1] = "capture";
    status("env", "capture", captured(Process.captureWith(me, list, null, options)));
    list[1] = "spawn";
    status("env", "spawn", waited(Process.spawn(me, list, options)));
    var cleared = ChildOptions.quiet();
    cleared.setEnvironment("CEESAFE_SET", "forgotten")!;
    cleared.clearEnvironment();
    cleared.setEnvironment("CEESAFE_ONLY", "1")!;
    cleared.output = ChildStream.Inherit;
    list[1] = "cleared";
    status("env", "cleared", Process.runWith(me, list, cleared));
    let zero = [0x61u8, 0x00, 0x62];
    for (k in 0..4) {
        var bad = ChildOptions.quiet();
        if (k == 0) {
            bad.setEnvironment("A=B", "x")!;
        } else if (k == 1) {
            bad.setEnvironment("", "x")!;
        } else if (k == 2) {
            bad.setEnvironment("CEESAFE_HANDOVER", "x")!;
        } else {
            bad.setEnvironment("CEESAFE_ZERO", OsStringView.fromBytes(zero[..]))!;
        }
        status("env", "bad", waited(Process.spawn(me, list, bad)));
        status("env", "bad", Process.runWith(me, list, bad));
        status("env", "bad", captured(Process.captureWith(me, list, null, bad)));
    }
}

// Finding a program (ADR 0259): a planted copy in the working directory runs only when named with a directory.
fn planted(windows: bool) {
    var none = Vector<StringView>.create();
    status("plant", "bare run", Process.run("ceesafe-planted", none));
    status("plant", "bare runWith", Process.runWith("ceesafe-planted", none, ChildOptions.inherit()));
    status("plant", "bare spawn", waited(Process.spawn("ceesafe-planted", none, ChildOptions.inherit())));
    var word = Vector<StringView>.create();
    word.push("planted")!;
    status("plant", "named", Process.run(if (windows) ".\\ceesafe-planted" else "./ceesafe-planted", word));
}

// The child's PATH (ADR 0259 §2.4): a bare name is looked up on it when the options set it, else on this process's.
fn childPath() {
    let bin = Environment.variableOs("CEESAFE_CHILD_BIN")!;
    var word = Vector<StringView>.create();
    word.push("planted")!;
    var options = ChildOptions.inherit();
    options.setEnvironment("PATH", bin)!;
    status("path", "child runWith", Process.runWith("ceesafe-onchild", word, options));
    status("path", "child captureWith", captured(Process.captureWith("ceesafe-onchild", word, null, options)));
    status("path", "child spawn", waited(Process.spawn("ceesafe-onchild", word, options)));
    status("path", "parent's under child", Process.runWith("ceesafe-onparent", word, options));
    let plain = ChildOptions.inherit();
    status("path", "parent runWith", Process.runWith("ceesafe-onparent", word, plain));
    status("path", "child's under parent", Process.runWith("ceesafe-onchild", word, plain));
    var removed = ChildOptions.inherit();
    removed.setEnvironment("PATH", bin)!;
    removed.removeEnvironment("PATH")!;
    status("path", "removed", Process.runWith("ceesafe-onparent", word, removed));
}

fn main() {
    let first = Process.argument(1) ?? "";
    if (first == "child" || first == "osChild") {
        child(first == "osChild");
        return;
    }
    if (first == "envChild") {
        environmentChild();
        return;
    }
    if (first == "argv") {
        // The command line as the runtime split it (ADR 0259 §2.1), each argument's bytes.
        var k: usize = 0;
        loop {
            let a = Process.argumentOs(k) ?? { break; };
            say("argv", "arg", "os", a.bytes());
            k += 1;
        }
        Console.flush();
        return;
    }
    if (first == "planted" || first == "/d") {
        Console.writeLine("planted");
        Console.flush();
        Process.exit(7);
    }
    let python = first;
    let script = Process.argument(2)!;
    let batches = Process.argument(3)!;
    let input = decoded(Process.argument(4)!);
    let work = Process.argument(5)!;
    let name1 = decoded(Process.argument(6)!);
    let name2 = decoded(Process.argument(7)!);
    let raw = Process.argumentOs(8)!;
    say("raw", "argument", "os", raw.bytes());
    Console.writeLine(if (Process.argument(8).isNull()) "raw text null" else "raw text given");
    var given = Vector<Vector<u8>>.create();
    var i: usize = 9;
    loop {
        match (Process.argument(i)) {
            Some(a) => { given.push(decoded(a))!; }
            null => { break; }
        }
        i += 1;
    }
    var all = Vector<StringView>.create();
    for (r in given.view()) {
        all.push(StringView.fromUtf8(r.view())!)!;
    }
    exercise(python, script, "py", all, input.view());
    let me = Process.executableOs()!;
    let meText = Process.executable()!;
    exercise(meText.view(), "child", "self", all, input.view());
    var none = Vector<StringView>.create();
    status("missing", "run", Process.run("no-such-program-anywhere", none));
    status("missing", "capture", captured(Process.capture("no-such-program-anywhere", none, null)));
    status("missing", "spawn", waited(Process.spawn("no-such-program-anywhere", none, ChildOptions.quiet())));
    let windows = batches != "-";
    if (windows) {
        let directory = decoded(batches);
        refusals(directory.view());
    }
    if (work != "-") {
        let place = decoded(work);
        names(me.view(), python, script, place.view(), name1.view(), name2.view());
    }
    environment(me.view());
    planted(windows);
    childPath();
    Console.writeLine("done");
}
'''

CHILD = r'''import os, sys
kind, mode, arguments = sys.argv[1], sys.argv[2], sys.argv[3:]
errors = "surrogatepass" if os.name == "nt" else "surrogateescape"
out = sys.stdout.buffer
for a in arguments:
    out.write(f"{kind} {mode} ARG {a.encode('utf-8', errors).hex()}\n".encode())
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
DEADLINE = 180.0

# Names that are not text: bytes on POSIX, WTF-8 of a lone surrogate on Windows.
if WINDOWS:
    NAME1, NAME2 = "a\udc80b.txt", "c\ud800d.txt"
    RAW = "r\udc81w"
else:
    NAME1, NAME2 = os.fsdecode(b"a\xffb.txt"), os.fsdecode(b"c\xfe\xedd.txt")
    RAW = os.fsdecode(b"r\x80w")


def encoded(name):
    """The name's bytes as the system gives them to a CeeSafe program: WTF-8 on Windows, the bytes elsewhere."""
    return name.encode("utf-8", "surrogatepass") if WINDOWS else os.fsencode(name)


ASKED = ["CEESAFE_SET", "CEESAFE_WIDE", "CEESAFE_EMPTY", "CEESAFE_GONE", "CEESAFE_KEEP", "CEESAFE_TWICE", "HOME",
         "CEESAFE_ONLY"]
PARENT_ENVIRONMENT = {"CEESAFE_GONE": "gone", "CEESAFE_KEEP": "keep", "HOME": "/home/start"}


def environment_lines(mode, values):
    lines = []
    for name in ASKED:
        if name in values:
            lines.append(f"env {mode} {name} {values[name].encode('utf-8').hex()}")
        else:
            lines.append(f"env {mode} {name} unset")
    return lines


def expected(names_made):
    lines = [f"raw argument os {encoded(RAW).hex()}", "raw text null"]
    for kind in ("py", "self"):
        for mode in ("run", "capture", "spawn"):
            for a in ARGUMENTS:
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
    if names_made is not None:
        if names_made:
            lines.append("os create ok")
            lines.append("os kind file")
            lines.append(f"os list name {encoded(NAME1).hex()}")
            lines.append("os text list error InvalidInput")
            lines.append(f"os read bytes {b'ok'.hex()}")
        else:
            lines.append("os create error InvalidInput")
        for kind in ("pyos", "selfos"):
            for mode in ("runWith", "captureWith", "spawn"):
                lines.append(f"{kind} {mode} ARG {encoded(NAME1).hex()}")
                lines.append(f"{kind} {mode} ARG {encoded(NAME2).hex()}")
                lines.append(f"{kind} {mode} IN ")
                lines.append(f"{kind} {mode} END ")
                lines.append(f"{kind} {mode} status 0")
    changed = dict(PARENT_ENVIRONMENT)
    changed.update({"CEESAFE_SET": "value", "CEESAFE_WIDE": "välue €", "CEESAFE_EMPTY": "", "CEESAFE_TWICE": "2",
                    "HOME": "/home/elsewhere"})
    del changed["CEESAFE_GONE"]
    if WINDOWS:
        changed["CEESAFE_KEEP"] = "lower"  # names without case: `ceesafe_keep` replaced it
    for mode in ("run", "capture", "spawn"):
        lines.extend(environment_lines(mode, changed))
        lines.append(f"env {mode} status 0")
    lines.extend(environment_lines("cleared", {"CEESAFE_ONLY": "1"}))
    lines.append("env cleared status 0")
    for _ in range(4):
        lines.extend(["env bad error InvalidInput"] * 3)
    lines.append("plant bare run error NotFound")
    lines.append("plant bare runWith error NotFound")
    lines.append("plant bare spawn error NotFound")
    lines.append("planted")
    lines.append("plant named status 7")
    lines += ["planted", "path child runWith status 7", "planted", "path child captureWith status 7", "planted",
              "path child spawn status 7", "path parent's under child error NotFound", "planted",
              "path parent runWith status 7", "path child's under parent error NotFound", "planted",
              "path removed status 7"]
    lines.append("done")
    return lines


# Command lines for the splitter (ADR 0259 §2.1), after `"<program>" argv`, compared with CommandLineToArgvW's split.
COMMAND_LINES = [
    'a b c', '"a b" c', 'a""b', '"a""b"', '"a""b c" d', '"""a"""', '""""', '"" ""x', '"ab"c""d e', 'a\\\\"b c"',
    'a\\"b', 'a\\\\\\"b', 'a\\b\\\\c', 'trail\\', '"trail\\\\"', '"x\\\\" y', '\t"tab"\t x ', '"', '"a', 'a"',
    '\\"', '\\\\"', '"\\""', '"\\\\""', 'ünï "cødé €"', '""a""b""', '"""', '""""""', 'a "" b', '  lead  trail  ',
    'x"y z"w', '"a b"c"d e"', '\\\\server\\share\\ "C:\\dir\\"', 'end',
]


def split_like_windows(line):
    """CommandLineToArgvW's split of `line`, by the system itself (ctypes)."""
    import ctypes
    from ctypes import wintypes
    split = ctypes.windll.shell32.CommandLineToArgvW
    split.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    split.restype = ctypes.POINTER(wintypes.LPWSTR)
    count = ctypes.c_int()
    argv = split(line, ctypes.byref(count))
    out = [argv[i] for i in range(count.value)]
    ctypes.windll.kernel32.LocalFree(argv)
    return out


def check_splitter(exe, problems):
    """On Windows: the program splits each command line as CommandLineToArgvW does (ADR 0259 §2.1)."""
    for tail in COMMAND_LINES:
        line = f'"{exe}" argv {tail}'
        want = [f"argv arg os {a.encode('utf-8', 'surrogatepass').hex()}" for a in split_like_windows(line)]
        r = subprocess.run(line, capture_output=True, timeout=60)
        got = [g.rstrip(b"\r").decode("utf-8", "replace") for g in r.stdout.split(b"\n") if g]
        if got != want:
            problems.append(f"command line {tail!r}: got {got}, expected {want}")
    return len(COMMAND_LINES)


def try_names(work):
    """Whether this file system takes the two names, as Python's own attempt finds (macOS's refuse them: EILSEQ)."""
    probe = os.path.join(work, NAME1)
    try:
        with open(probe, "wb"):
            pass
        os.remove(probe)
        return True
    except OSError as e:
        if e.errno in (errno.EILSEQ, errno.EINVAL):
            return False
        raise


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
    want = []
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
        files = os.path.join(work, "names")
        os.mkdir(files)
        made = try_names(files)
        exe = os.path.join(work, "check.exe" if WINDOWS else "check")
        b = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {(b.stdout + b.stderr).strip()[:800]}")
        else:
            # The planted copies, in the working directory the program runs in (ADR 0259).
            plant = os.path.join(work, "plant")
            os.mkdir(plant)
            shutil.copy(exe, os.path.join(plant, "ceesafe-planted.exe" if WINDOWS else "ceesafe-planted"))
            if WINDOWS:
                shutil.copy(exe, os.path.join(plant, "cmd.exe"))
            # ADR 0259 §2.4: one copy only on the child's PATH (the options'), one only on this process's.
            suffix = ".exe" if WINDOWS else ""
            onchild = os.path.join(work, "onchild")
            onparent = os.path.join(work, "onparent")
            os.mkdir(onchild)
            os.mkdir(onparent)
            shutil.copy(exe, os.path.join(onchild, "ceesafe-onchild" + suffix))
            shutil.copy(exe, os.path.join(onparent, "ceesafe-onparent" + suffix))
            environment = dict(os.environ)
            for name in list(environment):
                if name.upper().startswith("CEESAFE_"):
                    del environment[name]
            environment.update(PARENT_ENVIRONMENT)
            # No entry of PATH may name the working directory, which POSIX's empty entry and `.` do.
            environment["PATH"] = os.pathsep.join([onparent] + [p for p in environment.get("PATH", "").split(os.pathsep)
                                                                if p not in ("", ".")])
            environment["CEESAFE_CHILD_BIN"] = onchild
            command = [exe, sys.executable, script, batches, "x" + INPUT.hex(), "x" + files.encode("utf-8").hex(),
                       "x" + encoded(NAME1).hex(), "x" + encoded(NAME2).hex(), RAW]
            command += ["x" + a.encode("utf-8").hex() for a in ARGUMENTS]
            want = expected(made)
            try:
                r = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=DEADLINE, cwd=plant,
                                   env=environment)
                got = [line.rstrip(b"\r").decode("utf-8", "replace") for line in r.stdout.split(b"\n") if line]
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
            if made:
                # What the program left: the second name, which Python's own listing must show, with its bytes.
                listed = sorted(encoded(n) for n in os.listdir(files))
                if listed != [encoded(NAME2)]:
                    problems.append(f"the directory holds {[n.hex() for n in listed]}, expected {encoded(NAME2).hex()}")
                else:
                    with open(os.path.join(files, NAME2), "rb") as f:
                        if f.read() != b"ok":
                            problems.append("the renamed file does not hold what was written")
            print(f"process_check: names that are not text {'made' if made else 'refused by this file system'}")
            if WINDOWS:
                lines = check_splitter(exe, problems)
                print(f"process_check: {lines} command lines split as CommandLineToArgvW splits them")
    for problem in problems[:60]:
        print(f"FAIL {problem}")
    print(f"process_check: {len(want)} lines: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
