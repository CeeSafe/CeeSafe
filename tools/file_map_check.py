#!/usr/bin/env python3
"""Checks of shared mappings of a file (ADR 0232) that the corpus cannot make, since they need another process or a
program outside CeeSafe:

1. interoperation with Python's `mmap` module (an independent implementation of `MAP_SHARED`), both ways: Python
   maps a file and writes a word and bytes, and the program, mapping the same file, reads them; the program writes,
   and Python reads through its own mapping, which it held all along. Natively and in the interpreter (whose model
   writes the file, which the systems keep the same as its mapped pages);
2. a file shortened by another process under a live mapping: natively the program's next access ends it by SIGBUS
   (POSIX; ADR 0232 §2.3), and in the interpreter it is a fault;
3. SQLite's wal-index, where a `sqlite3` shell is installed: while a `sqlite3` process holds a database in WAL mode
   open, the program maps the first 32 KiB of its `-shm`, copies the two 48-byte copies of the wal-index header out
   with a fence between them (wal.c's walIndexTryHdr), and finds them equal, with iVersion 3007000, isInit 1 and an
   mxFrame equal to the frames the `-wal` file holds by its size ((size - 32) / (24 + page size)). Without `sqlite3`
   this part is skipped and said so.

usage: python3 tools/file_map_check.py --compiler out/ceesafec
"""

import argparse
import mmap
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile

PROGRAM = """
fn fail(e: IoError) -> never {
    Console.writeLine(e.message());
    Process.exit(1);
}

fn text(bytes: Slice<u8>) -> StringView => StringView.fromUtf8(bytes) ?? "?";

fn mapped(f: borrow File, length: usize) -> SharedMapping {
    return match (f.mapShared(0, length)) {
        Ok(m) => m,
        Err(e) => fail(e),
    };
}

fn main() {
    let mode = Process.argument(1)!;
    let path = Process.argument(2)!;
    let f = match (File.openReadWrite(path)) {
        Ok(f) => f,
        Err(e) => fail(e),
    };
    if (mode == "read") {
        // Python wrote a word at 0 and eleven bytes at 100.
        let m = mapped(f, 4096);
        Console.writeLine(m.load32(0, LoadOrdering.Acquire));
        var bytes = [0u8; 11];
        m.readAt(100, mut bytes[..]);
        Console.writeLine(text(bytes[..]));
    } else if (mode == "write") {
        let m = mapped(f, 4096);
        m.writeAt(200, "from ceesafe".bytes());
        m.store32(8, 0x12345678, StoreOrdering.Release);
        let _ = m.fetchAdd32(0, 1, Ordering.SequentiallyConsistent);
        Console.writeLine("written");
    } else if (mode == "hold") {
        // Maps 128 KiB, says so, and after a line arrives touches byte 65536, a page past the shortened end whatever
        // the page size (4 KiB, macOS arm64's 16 KiB, some Linux arm64's 64 KiB): Python shortens the file meanwhile.
        let m = mapped(f, 131072);
        Console.writeLine("mapped");
        var line = String.create();
        let _ = Console.readLine(mut line);
        var bytes = [0u8; 4];
        m.readAt(65536, mut bytes[..]);
        Console.writeLine("read past the end");
    } else if (mode == "shm") {
        // SQLite's wal-index: two copies of the 48-byte header, a fence between the copies (walIndexTryHdr).
        let m = mapped(f, 32768);
        var first = [0u8; 48];
        var second = [0u8; 48];
        m.readAt(0, mut first[..]);
        SharedMapping.fence();
        m.readAt(48, mut second[..]);
        var equal = true;
        for (i in 0..48usize) {
            if (first[i] != second[i]) {
                equal = false;
            }
        }
        Console.writeLine(equal);
        Console.writeLine(m.load32(0, LoadOrdering.Acquire));
        Console.writeLine(first[12]);
        Console.writeLine(m.load32(16, LoadOrdering.Acquire));
    }
}
"""


def run(command, problems, what, stdin=None):
    r = subprocess.run(command, capture_output=True, text=True, input=stdin, timeout=600)
    if r.returncode != 0:
        problems.append(f"{what}: exit {r.returncode}: {r.stdout.strip()} {r.stderr.strip()}")
        return None
    return r.stdout.split()


def python_both_ways(modes, path, problems):
    for name, command in modes:
        with open(path, "wb") as f:
            f.write(b"\0" * 4096)
        with open(path, "r+b") as f:
            m = mmap.mmap(f.fileno(), 4096, access=mmap.ACCESS_WRITE)
            m[0:4] = struct.pack("=I", 0xDEADBEEF)
            m[100:111] = b"from python"
            got = run(command + ["read", path], problems, f"{name}: read what Python wrote")
            want = [str(0xDEADBEEF), "from", "python"]
            if got is not None and got != want:
                problems.append(f"{name}: read what Python wrote: {got}, wanted {want}")
            got = run(command + ["write", path], problems, f"{name}: write for Python")
            if got is not None:
                seen = (m[200:212], struct.unpack("=I", m[8:12])[0], struct.unpack("=I", m[0:4])[0])
                want = (b"from ceesafe", 0x12345678, 0xDEADBEEF + 1)
                if seen != want:
                    problems.append(f"{name}: Python read {seen}, wanted {want}")
            m.close()


def shortened(modes, path, problems, skipped):
    for name, command in modes:
        with open(path, "wb") as f:
            f.write(b"\1" * 131072)
        p = subprocess.Popen(command + ["hold", path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        line = p.stdout.readline().strip()
        if line != "mapped":
            p.kill()
            problems.append(f"{name}: shortened: the program said {line!r}")
            continue
        os.truncate(path, 100)
        out, err = p.communicate("go\n", timeout=600)
        if name == "native":
            if p.returncode != -signal.SIGBUS:
                problems.append(f"{name}: shortened: exit {p.returncode}, wanted SIGBUS: {out.strip()} {err.strip()}")
        elif p.returncode != 70 or "shortened" not in err:
            problems.append(f"{name}: shortened: exit {p.returncode}, wanted the fault (70): {err.strip()}")


def sqlite_shm(exe, work, problems, skipped):
    sqlite = shutil.which("sqlite3")
    if sqlite is None:
        skipped.append("SQLite's -shm: no sqlite3")
        return
    database = os.path.join(work, "wal.db")
    shell = subprocess.Popen([sqlite, database], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
    try:
        shell.stdin.write("PRAGMA journal_mode=WAL;\nCREATE TABLE t(x);\n")
        shell.stdin.write("INSERT INTO t VALUES(1),(2);\nINSERT INTO t VALUES(randomblob(9000));\n.print ready\n")
        shell.stdin.flush()
        while True:
            line = shell.stdout.readline()
            if not line or line.strip() == "ready":
                break
        with open(database, "rb") as f:
            header = f.read(18)
        page = struct.unpack(">H", header[16:18])[0]
        page = 65536 if page == 1 else page
        frames = (os.path.getsize(database + "-wal") - 32) // (24 + page)
        got = run([exe, "shm", database + "-shm"], problems, "SQLite's -shm")
        want = ["true", "3007000", "1", str(frames)]
        if got is not None and got != want:
            problems.append(f"SQLite's -shm: {got}, wanted {want} (equal copies, iVersion, isInit, mxFrame)")
    finally:
        shell.stdin.close()
        shell.wait(timeout=600)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    skipped = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-maps-") as work:
        source = os.path.join(work, "maps.cee")
        with open(source, "w") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "maps")
        b = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            print(f"file_map_check: the program does not build:\n{b.stdout}{b.stderr}")
            sys.exit(1)
        modes = [("native", [exe]), ("interpreter", [compiler, "run", "--interp", source, "--"])]
        path = os.path.join(work, "shared")
        if os.name == "posix":
            python_both_ways(modes, path, problems)
            shortened(modes, path, problems, skipped)
            sqlite_shm(exe, work, problems, skipped)
        else:
            skipped.append("every part: not a POSIX host")
    print(f"file_map_check: Python's mmap both ways, a file shortened under a mapping, SQLite's -shm; "
          f"{len(problems)} problems")
    for s in skipped:
        print(f"  skipped: {s}")
    for problem in problems:
        print(f"  {problem}")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
