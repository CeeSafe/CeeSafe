#!/usr/bin/env python3
"""Checks of advisory file locks (ADR 0220) that the corpus cannot make, since they need another process or a host
that has open-file-description locks:

1. conflicts between two Files of one program, conversion and splitting, and release on close: natively and in the
   interpreter, with the same answers, on hosts with open-file-description locks (Linux, macOS); elsewhere a
   process's Files do not conflict, and this part is skipped and said so;
2. interoperation with another process's locks, both ways, against Python's `fcntl.lockf` (an independent
   implementation of POSIX record locks: classic, process-associated ones) on POSIX hosts;
3. interoperation with SQLite, where a `sqlite3` shell is installed: a database whose SHARED range the program holds
   exclusively makes `sqlite3` report "database is locked" for a read; one whose SHARED range the program holds
   shared makes a write report it; and while `sqlite3` holds `BEGIN EXCLUSIVE`, the program's `tryLock` of the SHARED
   range is false. Without `sqlite3` this part is skipped and said so.

`f.testLock` (ADR 0234) is checked in each part: (1) between two Files, with a conversion in place that fails and
keeps the held lock (§2.3); (2) against Python's shared and exclusive `fcntl.lockf` locks, each reported with its
kind, and, while the program tests a byte 200 000 times, Python's own non-waiting locks of it never fail (a test takes
nothing); (3) while `sqlite3` holds `BEGIN IMMEDIATE`, the RESERVED byte tests `Exclusive`, and `null` after `COMMIT`.

4. hard links (ADR 0255), on every host, natively and in the interpreter: after the program's `File.link`, Python's
   `os.stat` sees one file (the same `st_ino` and `st_dev`) with two links, and one after a name is removed; a link
   made by Python's `os.link` is read through one name and written through the other by the program; a lock taken
   through one name of a database conflicts with one through the other (the SQLite port's use);
5. the portable lock rules (ADR 0256), on every host, natively and in the interpreter: sequences of locks and
   unlocks on one File under `CEESAFE_LOCK_RULES=strict` and `portable` stop (panic) exactly where the ADR's model
   says; and strict refuses exactly what Windows refuses, by Windows' own `LockFileEx` and `UnlockFileEx` called
   through `ctypes` (live on Windows; elsewhere the answers recorded from it, WINDOWS_RECORDED): every step strict
   lets through, Windows grants, and the step strict refuses, Windows refuses, or grants by stacking a lock over the
   handle's own, which no POSIX host does.

The SQLite lock bytes are those of its os_unix.c: PENDING 0x40000000, RESERVED 0x40000001, SHARED 0x40000002 to
0x40000002 + 509.

usage: python3 tools/file_lock_check.py --compiler out/ceesafec
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

PENDING = 0x40000000
RESERVED = PENDING + 1
SHARED_FIRST = PENDING + 2
SHARED_SIZE = 510

CONFLICTS = """
fn pending() -> u64 => 0x40000000;
fn reserved() -> u64 => 0x40000001;
fn sharedFirst() -> u64 => 0x40000002;

fn run(path: StringView) -> Result<void, IoError> {
    let a = File.openReadWrite(path)?;
    let b = File.openReadWrite(path)?;
    Console.writeLine(a.tryLock(FileLock.Shared, sharedFirst(), 510)?);
    Console.writeLine(b.tryLock(FileLock.Shared, sharedFirst(), 510)?);
    Console.writeLine(a.tryLock(FileLock.Exclusive, sharedFirst(), 510)?);
    Console.writeLine(a.tryLock(FileLock.Exclusive, reserved(), 1)?);
    Console.writeLine(b.tryLock(FileLock.Shared, reserved(), 1)?);
    Console.writeLine(b.tryLock(FileLock.Exclusive, pending(), 2)?);
    Console.writeLine(b.tryLock(FileLock.Exclusive, pending(), 1)?);
    b.unlock(sharedFirst(), 510)?;
    Console.writeLine(a.tryLock(FileLock.Exclusive, sharedFirst(), 510)?);
    Console.writeLine(b.tryLock(FileLock.Shared, sharedFirst() + 100, 1)?);
    a.unlock(sharedFirst() + 100, 10)?;
    Console.writeLine(b.tryLock(FileLock.Shared, sharedFirst() + 100, 10)?);
    Console.writeLine(b.tryLock(FileLock.Shared, sharedFirst() + 99, 1)?);
    Console.writeLine(b.tryLock(FileLock.Shared, sharedFirst() + 110, 1)?);
    a.lock(FileLock.Shared, sharedFirst(), 100)?;
    Console.writeLine(b.tryLock(FileLock.Shared, sharedFirst(), 1)?);
    {
        let c = File.openReadWrite(path)?;
        Console.writeLine(c.tryLock(FileLock.Exclusive, 0, 1)?);
        Console.writeLine(b.tryLock(FileLock.Exclusive, 0, 1)?);
    }
    Console.writeLine(b.tryLock(FileLock.Exclusive, 0, 1)?);
    Console.writeLine(b.tryLock(FileLock.Exclusive, reserved(), 1)?);
    return Ok();
}

fn main() {
    match (run(Process.argument(1)!)) {
        Ok() => {}
        Err(e) => { Console.writeLine(e.message()); }
    }
}
"""
# Each answer follows from the rules of ADR 0220 §2.2, in order.
CONFLICTS_WANT = ["true", "true", "false", "true", "false", "false", "true", "true", "false", "true", "false",
                  "false", "true", "true", "false", "true", "false"]

# testLock between two Files of one program (ADR 0234), and a conversion in place that fails atomically (§2.3).
TESTS = """
fn sharedFirst() -> u64 => 0x40000002;

fn name(lock: FileLock?) -> StringView => match (lock) {
    Some(FileLock.Shared) => "Shared",
    Some(FileLock.Exclusive) => "Exclusive",
    null => "null",
};

fn run(path: StringView) -> Result<void, IoError> {
    let a = File.openReadWrite(path)?;
    let b = File.openReadWrite(path)?;
    a.lock(FileLock.Shared, sharedFirst(), 510)?;
    b.lock(FileLock.Shared, sharedFirst(), 510)?;
    Console.writeLine(a.tryLock(FileLock.Exclusive, sharedFirst(), 510)?);
    Console.writeLine(name(a.testLock(FileLock.Exclusive, sharedFirst(), 510)?));
    Console.writeLine(name(b.testLock(FileLock.Exclusive, sharedFirst(), 510)?));
    b.unlock(sharedFirst(), 510)?;
    Console.writeLine(name(b.testLock(FileLock.Exclusive, sharedFirst() + 509, 1)?));
    Console.writeLine(a.tryLock(FileLock.Exclusive, sharedFirst(), 510)?);
    Console.writeLine(name(b.testLock(FileLock.Shared, sharedFirst() + 7, 1)?));
    Console.writeLine(name(a.testLock(FileLock.Exclusive, sharedFirst(), 510)?));
    a.lock(FileLock.Shared, sharedFirst(), 510)?;
    Console.writeLine(name(b.testLock(FileLock.Exclusive, sharedFirst(), 510)?));
    Console.writeLine(name(b.testLock(FileLock.Shared, sharedFirst(), 510)?));
    return Ok();
}

fn main() {
    match (run(Process.argument(1)!)) {
        Ok() => {}
        Err(e) => { Console.writeLine(e.message()); }
    }
}
"""
# ADR 0234 §2.1 and §2.3, in order: the conversion fails while b shares and keeps a's shared lock; a's own lock is
# never reported to a; converted, it is reported Exclusive to b; converted back, Shared to an exclusive test only.
TESTS_WANT = ["false", "Shared", "Shared", "Shared", "true", "Exclusive", "null", "Shared", "null"]

# Tests a lock and prints what holds it: "null", "Shared" or "Exclusive" (ADR 0234).
TEST = """
fn main() {
    let f = File.open(Process.argument(1)!)!;
    Console.writeLine(match (f.testLock(FileLock.KIND, START, LENGTH)!) {
        Some(FileLock.Shared) => "Shared",
        Some(FileLock.Exclusive) => "Exclusive",
        null => "null",
    });
}
"""

# Tests an exclusive lock COUNT times as fast as it can and prints how many tests found a holder: while it runs,
# Python's non-waiting locks of the range must all succeed, since a test takes nothing (ADR 0234 §2.1).
TEST_LOOP = """
fn main() {
    let f = File.openReadWrite(Process.argument(1)!)!;
    var held: u32 = 0;
    for (i in 0..COUNT) {
        if (f.testLock(FileLock.Exclusive, START, LENGTH)!.isSome()) {
            held += 1;
        }
    }
    Console.writeLine(held);
}
"""

# Takes a lock, says so, and holds it until a line arrives on standard input.
HOLD = """
fn main() {
    let f = File.openReadWrite(Process.argument(1)!)!;
    Console.writeLine(if (f.tryLock(FileLock.KIND, START, LENGTH)!) "locked" else "busy");
    Console.flush();
    var line = String.create();
    let _ = Console.readLine(mut line);
}
"""

# Tries a lock and prints whether it was taken.
TRY = """
fn main() {
    let f = File.openReadWrite(Process.argument(1)!)!;
    Console.writeLine(f.tryLock(FileLock.KIND, START, LENGTH)!);
}
"""


# File.link (ADR 0255): links argument 1 under the name argument 2 and prints "Ok" or the error.
LINK = """
fn main() {
    match (File.link(Process.argument(1)!, Process.argument(2)!)) {
        Ok() => { Console.writeLine("Ok"); }
        Err(e) => { Console.writeLine(e.message()); }
    }
}
"""

# Reads the file at argument 1, prints it, and writes argument 3 at the start of the file at argument 2.
READ_WRITE = """
fn main() {
    var f = File.open(Process.argument(1)!)!;
    var buffer = [0u8; 64];
    let n = f.read(mut buffer[..])!;
    Console.writeLine(StringView.fromUtf8(buffer[0..n]) ?? "?");
    let g = File.openReadWrite(Process.argument(2)!)!;
    g.writeAt(Process.argument(3)!.bytes(), 0)!;
}
"""

# ADR 0256: the steps given as arguments, on one File of argument 1: `L S|X start length` a tryLock, printed;
# `U start length` an unlock, printed "ok". A step the rules refuse panics, so the lines printed say where.
RULES = """
fn number(i: usize) -> u64 => NumberParsing.parseU64(Process.argument(i)!.bytes())!;

fn main() {
    let f = File.openReadWrite(Process.argument(1)!)!;
    var i: usize = 2;
    while (Process.argument(i).isSome()) {
        let op = Process.argument(i)!.bytes()[0];
        if (op == 'U') {
            f.unlock(number(i + 1), number(i + 2))!;
            Console.writeLine("ok");
            i += 3;
        } else {
            let kind = if (Process.argument(i + 1)!.bytes()[0] == 'S') FileLock.Shared else FileLock.Exclusive;
            Console.writeLine(f.tryLock(kind, number(i + 2), number(i + 3))!);
            i += 4;
        }
        Console.flush();
    }
}
"""

# Sequences of steps on one handle (ADR 0256 §4): ("L", "S" or "X", start, length) a lock that does not wait,
# ("U", start, length) an unlock. Same, overlapping, inner, adjacent and disjoint ranges of each kind; exact, partial,
# joined, unheld and repeated unlocks; os_win.c's and os_unix.c's steps.
RULE_SEQUENCES = [
    ("S then S, same range", [("L", "S", 100, 10), ("L", "S", 100, 10)]),
    ("X then X, same range", [("L", "X", 100, 10), ("L", "X", 100, 10)]),
    ("S then X, same range (os_unix.c's conversion)", [("L", "S", 100, 10), ("L", "X", 100, 10)]),
    ("X then S, same range", [("L", "X", 100, 10), ("L", "S", 100, 10)]),
    ("S then S, overlapping", [("L", "S", 100, 10), ("L", "S", 105, 10)]),
    ("S then X, overlapping", [("L", "S", 100, 10), ("L", "X", 105, 10)]),
    ("X then S, overlapping", [("L", "X", 100, 10), ("L", "S", 105, 10)]),
    ("X then X, overlapping", [("L", "X", 100, 10), ("L", "X", 105, 10)]),
    ("X then S, inside", [("L", "X", 100, 10), ("L", "S", 102, 2)]),
    ("S then X, around", [("L", "S", 102, 2), ("L", "X", 100, 10)]),
    ("S then S, adjacent", [("L", "S", 100, 10), ("L", "S", 110, 10)]),
    ("X then X, adjacent", [("L", "X", 100, 10), ("L", "X", 110, 10)]),
    ("X, disjoint", [("L", "X", 100, 10), ("L", "X", 200, 10), ("U", 200, 10), ("U", 100, 10)]),
    ("unlock exactly", [("L", "S", 100, 10), ("U", 100, 10)]),
    ("unlock unheld", [("U", 100, 10)]),
    ("unlock a part", [("L", "S", 100, 10), ("U", 100, 5)]),
    ("unlock more", [("L", "S", 100, 10), ("U", 100, 20)]),
    ("unlock two as one", [("L", "X", 100, 1), ("L", "X", 101, 1), ("U", 100, 2)]),
    ("unlock twice", [("L", "S", 100, 10), ("U", 100, 10), ("U", 100, 10)]),
    ("lock after unlock", [("L", "X", 100, 10), ("U", 100, 10), ("L", "X", 100, 10), ("U", 100, 10)]),
    ("os_win.c: SHARED to EXCLUSIVE", [("L", "S", 0x40000002, 510), ("L", "X", 0x40000001, 1),
                                       ("L", "X", 0x40000000, 1), ("U", 0x40000002, 510),
                                       ("L", "X", 0x40000002, 510), ("U", 0x40000002, 510), ("U", 0x40000000, 1),
                                       ("U", 0x40000001, 1)]),
    ("os_unix.c: SHARED to EXCLUSIVE", [("L", "S", 0x40000002, 510), ("L", "X", 0x40000001, 1),
                                        ("L", "X", 0x40000002, 510)]),
]

# What Windows answers to each sequence on one handle, step by step, up to the step strict refuses: "granted",
# "refused" (LockFileEx's ERROR_LOCK_VIOLATION, UnlockFileEx's ERROR_NOT_LOCKED), or "stacked" (granted over the
# handle's own lock, and one unlock of it leaves the bytes locked: another handle's exclusive try still fails).
# Recorded from windows_answers() on Windows; on Windows the checker computes them live instead. These are GitHub's
# windows-2022 runner's (Windows Server 2022), printed by this script in the windows workflow's run 45577525614 on
# 2026-11-25 (branch fix/g5-files): Windows stacks a shared lock over the handle's own lock of either kind, and refuses
# an exclusive one over any.
WINDOWS_RECORDED = {
    'S then S, same range': ['granted', 'stacked'],
    'X then X, same range': ['granted', 'refused'],
    "S then X, same range (os_unix.c's conversion)": ['granted', 'refused'],
    'X then S, same range': ['granted', 'stacked'],
    'S then S, overlapping': ['granted', 'stacked'],
    'S then X, overlapping': ['granted', 'refused'],
    'X then S, overlapping': ['granted', 'stacked'],
    'X then X, overlapping': ['granted', 'refused'],
    'X then S, inside': ['granted', 'stacked'],
    'S then X, around': ['granted', 'refused'],
    'S then S, adjacent': ['granted', 'granted'],
    'X then X, adjacent': ['granted', 'granted'],
    'X, disjoint': ['granted', 'granted', 'granted', 'granted'],
    'unlock exactly': ['granted', 'granted'],
    'unlock unheld': ['refused'],
    'unlock a part': ['granted', 'refused'],
    'unlock more': ['granted', 'refused'],
    'unlock two as one': ['granted', 'granted', 'refused'],
    'unlock twice': ['granted', 'granted', 'refused'],
    'lock after unlock': ['granted', 'granted', 'granted', 'granted'],
    'os_win.c: SHARED to EXCLUSIVE': ['granted', 'granted', 'granted', 'granted', 'granted', 'granted', 'granted', 'granted'],
    'os_unix.c: SHARED to EXCLUSIVE': ['granted', 'granted', 'refused'],
}


def rules_model(steps, rules):
    """ADR 0256 §2.2's answers, step by step, until the first refusal ("panic")."""
    held = []
    out = []
    for step in steps:
        if step[0] == "U":
            _, start, length = step
            if (start, start + length) in [(h[0], h[1]) for h in held]:
                held = [h for h in held if (h[0], h[1]) != (start, start + length)]
                out.append("ok")
                continue
            out.append("panic")
            break
        _, kind, start, length = step
        end = start + length
        overlapping = [h for h in held if h[0] < end and start < h[1]]
        if not overlapping:
            held.append((start, end, kind))
            out.append("true")
        elif (rules == "portable" and len(overlapping) == 1 and overlapping[0][:2] == (start, end)
              and overlapping[0][2] != kind):
            held = [h for h in held if h[:2] != (start, end)] + [(start, end, kind)]
            out.append("true")
        else:
            out.append("panic")
            break
    return out


def rules_arguments(steps):
    words = []
    for step in steps:
        words += [str(w) for w in step]
    return words


def windows_answers(path, steps):
    """Windows' own answers (WINDOWS_RECORDED's form), with LockFileEx and UnlockFileEx through ctypes: an
    implementation independent of CeeSafe's runtime. Every step is tried without waiting."""
    import ctypes
    import ctypes.wintypes as w
    k = ctypes.WinDLL("kernel32", use_last_error=True)

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p), ("Offset", w.DWORD),
                    ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]

    k.CreateFileW.restype = w.HANDLE
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    k.LockFileEx.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, w.DWORD, ctypes.POINTER(OVERLAPPED)]
    k.UnlockFileEx.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.POINTER(OVERLAPPED)]
    k.CloseHandle.argtypes = [w.HANDLE]

    def handle():
        # GENERIC_READ | GENERIC_WRITE, every sharing mode, OPEN_EXISTING, as the runtime opens a file
        return k.CreateFileW(path, 0xC0000000, 7, None, 3, 0x80, None)

    def lock(h, exclusive, start, length):
        at = OVERLAPPED(Offset=start & 0xFFFFFFFF, OffsetHigh=start >> 32)
        return bool(k.LockFileEx(h, (2 if exclusive else 0) | 1, 0, length & 0xFFFFFFFF, length >> 32,
                                 ctypes.byref(at)))

    def unlock(h, start, length):
        at = OVERLAPPED(Offset=start & 0xFFFFFFFF, OffsetHigh=start >> 32)
        return bool(k.UnlockFileEx(h, 0, length & 0xFFFFFFFF, length >> 32, ctypes.byref(at)))

    h = handle()
    other = handle()
    held = []
    out = []
    try:
        for step in steps:
            if step[0] == "U":
                _, start, length = step
                done = unlock(h, start, length)
                out.append("granted" if done else "refused")
                if done:
                    held = [x for x in held if (x[0], x[1]) != (start, start + length)]
                continue
            _, kind, start, length = step
            overlaps = [x for x in held if x[0] < start + length and start < x[1]]
            if not lock(h, kind == "X", start, length):
                out.append("refused")
                continue
            if overlaps:
                # Granted over the handle's own lock: release it once and see whether the bytes are still locked.
                unlock(h, start, length)
                first = overlaps[0]
                still = not lock(other, True, first[0], first[1] - first[0])
                if not still:
                    unlock(other, first[0], first[1] - first[0])
                out.append("stacked" if still else "granted")
                if not still:
                    held.append((start, start + length))
            else:
                held.append((start, start + length))
                out.append("granted")
    finally:
        k.CloseHandle(h)
        k.CloseHandle(other)
    return out


def program(template, kind, start, length):
    return template.replace("KIND", kind).replace("START", str(start)).replace("LENGTH", str(length))


def build(compiler, work, name, source, problems):
    path = os.path.join(work, name + ".cee")
    with open(path, "w") as f:
        f.write(source)
    exe = os.path.join(work, name)
    b = subprocess.run([compiler, "build", path, "-o", exe], capture_output=True, text=True)
    if b.returncode != 0:
        problems.append(f"{name}: build failed: {b.stderr.strip()[-600:]}")
        return None
    return exe


class Holder:
    """A program holding a lock until it is told to end."""

    def __init__(self, exe, database):
        self.process = subprocess.Popen([exe, database], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.said = self.process.stdout.readline().strip()

    def end(self):
        self.process.communicate("\n", timeout=30)


def python_try(database, exclusive, start, length):
    """Whether Python, in this process, takes the lock without waiting (a classic fcntl lock); it is released."""
    import fcntl
    fd = os.open(database, os.O_RDWR)
    try:
        fcntl.lockf(fd, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB, length, start, 0)
        fcntl.lockf(fd, fcntl.LOCK_UN, length, start, 0)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    skipped = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-locks-") as work:
        database = os.path.join(work, "db")
        open(database, "wb").close()

        # 1. Two Files of one program, natively and in the interpreter.
        if sys.platform.startswith("linux") or sys.platform == "darwin":
            exe = build(compiler, work, "conflicts", CONFLICTS, problems)
            if exe:
                p = subprocess.run([exe, database], capture_output=True, text=True, timeout=60)
                if p.stdout.split() != CONFLICTS_WANT:
                    problems.append(f"conflicts natively: {p.stdout.split()} (status {p.returncode}), "
                                    f"want {CONFLICTS_WANT}")
            p = subprocess.run([compiler, "run", "--interp", os.path.join(work, "conflicts.cee"), "--", database],
                               capture_output=True, text=True, timeout=120)
            if p.stdout.split() != CONFLICTS_WANT:
                problems.append(f"conflicts in the interpreter: {p.stdout.split()} (status {p.returncode}, "
                                f"{p.stderr.strip()[-300:]}), want {CONFLICTS_WANT}")
            # testLock between the Files, and a conversion in place (ADR 0234).
            exe = build(compiler, work, "tests", TESTS, problems)
            if exe:
                p = subprocess.run([exe, database], capture_output=True, text=True, timeout=60)
                if p.stdout.split() != TESTS_WANT:
                    problems.append(f"testLock natively: {p.stdout.split()} (status {p.returncode}), "
                                    f"want {TESTS_WANT}")
            p = subprocess.run([compiler, "run", "--interp", os.path.join(work, "tests.cee"), "--", database],
                               capture_output=True, text=True, timeout=120)
            if p.stdout.split() != TESTS_WANT:
                problems.append(f"testLock in the interpreter: {p.stdout.split()} (status {p.returncode}, "
                                f"{p.stderr.strip()[-300:]}), want {TESTS_WANT}")
        else:
            skipped.append(f"conflicts between Files of a process ({sys.platform} has no open-file-description locks)")

        if os.name == "nt":
            skipped.append("other processes' locks (Python's fcntl is POSIX only)")
        else:
            # 2. Another process: the program holds, Python tries; Python holds, the program tries.
            hold_ex = build(compiler, work, "hold_ex", program(HOLD, "Exclusive", SHARED_FIRST, SHARED_SIZE), problems)
            hold_sh = build(compiler, work, "hold_sh", program(HOLD, "Shared", SHARED_FIRST, SHARED_SIZE), problems)
            try_ex = build(compiler, work, "try_ex", program(TRY, "Exclusive", SHARED_FIRST, SHARED_SIZE), problems)
            try_sh = build(compiler, work, "try_sh", program(TRY, "Shared", SHARED_FIRST, SHARED_SIZE), problems)
            if hold_ex and hold_sh:
                h = Holder(hold_ex, database)
                if h.said != "locked":
                    problems.append(f"hold exclusive: said {h.said!r}")
                if python_try(database, False, SHARED_FIRST, SHARED_SIZE):
                    problems.append("Python took a shared lock over the program's exclusive one")
                if python_try(database, False, SHARED_FIRST + 509, 1) or python_try(database, True, SHARED_FIRST, 1):
                    problems.append("Python took a lock on a byte of the program's exclusive range")
                if not python_try(database, True, PENDING, 2):
                    problems.append("Python could not lock bytes next to the program's range")
                h.end()
                if not python_try(database, True, SHARED_FIRST, SHARED_SIZE):
                    problems.append("the program's lock outlived it")
                h = Holder(hold_sh, database)
                if h.said != "locked":
                    problems.append(f"hold shared: said {h.said!r}")
                if not python_try(database, False, SHARED_FIRST, SHARED_SIZE):
                    problems.append("Python's shared lock conflicted with the program's shared one")
                if python_try(database, True, SHARED_FIRST + 10, 1):
                    problems.append("Python took an exclusive lock over the program's shared one")
                h.end()
            if try_ex and try_sh:
                import fcntl
                fd = os.open(database, os.O_RDWR)
                fcntl.lockf(fd, fcntl.LOCK_EX, 1, SHARED_FIRST + 200, 0)
                for exe, want in ((try_sh, "false"), (try_ex, "false")):
                    p = subprocess.run([exe, database], capture_output=True, text=True, timeout=60)
                    if p.stdout.strip() != want:
                        problems.append(f"{os.path.basename(exe)} against Python's exclusive lock: "
                                        f"{p.stdout.strip()!r}, want {want}")
                fcntl.lockf(fd, fcntl.LOCK_UN, 1, SHARED_FIRST + 200, 0)
                fcntl.lockf(fd, fcntl.LOCK_SH, 1, SHARED_FIRST + 200, 0)
                for exe, want in ((try_sh, "true"), (try_ex, "false")):
                    p = subprocess.run([exe, database], capture_output=True, text=True, timeout=60)
                    if p.stdout.strip() != want:
                        problems.append(f"{os.path.basename(exe)} against Python's shared lock: "
                                        f"{p.stdout.strip()!r}, want {want}")
                os.close(fd)
                p = subprocess.run([try_ex, database], capture_output=True, text=True, timeout=60)
                if p.stdout.strip() != "true":
                    problems.append(f"try_ex after Python's locks went: {p.stdout.strip()!r}")

            # testLock (ADR 0234) against Python's locks: each reported with its kind, and nothing taken.
            test_ex = build(compiler, work, "test_ex", program(TEST, "Exclusive", RESERVED, 1), problems)
            test_sh = build(compiler, work, "test_sh", program(TEST, "Shared", RESERVED, 1), problems)
            loop_source = program(TEST_LOOP, "Exclusive", RESERVED, 1).replace("COUNT", "200000u32")
            test_loop = build(compiler, work, "test_loop", loop_source, problems)
            if test_ex and test_sh:
                import fcntl

                def tested(want_ex, want_sh, what):
                    for exe, want in ((test_ex, want_ex), (test_sh, want_sh)):
                        p = subprocess.run([exe, database], capture_output=True, text=True, timeout=60)
                        if p.stdout.strip() != want:
                            problems.append(f"{os.path.basename(exe)} {what}: {p.stdout.strip()!r} "
                                            f"{p.stderr.strip()[-200:]!r}, want {want}")

                tested("null", "null", "with no lock held")
                fd = os.open(database, os.O_RDWR)
                fcntl.lockf(fd, fcntl.LOCK_SH, 1, RESERVED, 0)
                tested("Shared", "null", "against Python's shared lock")
                fcntl.lockf(fd, fcntl.LOCK_EX, 1, RESERVED, 0)
                tested("Exclusive", "Exclusive", "against Python's exclusive lock")
                fcntl.lockf(fd, fcntl.LOCK_UN, 1, RESERVED, 0)
                tested("null", "null", "after Python's lock went")
                os.close(fd)
            if test_loop:
                # While the program tests the byte 200 000 times, Python takes and releases it without waiting: a test
                # that took the lock, even for an instant, would make some of Python's attempts fail.
                import fcntl
                fd = os.open(database, os.O_RDWR)
                p = subprocess.Popen([test_loop, database], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                attempts = refused = 0
                while p.poll() is None:
                    attempts += 1
                    try:
                        fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, RESERVED, 0)
                        fcntl.lockf(fd, fcntl.LOCK_UN, 1, RESERVED, 0)
                    except OSError:
                        refused += 1
                out, err = p.communicate(timeout=120)
                os.close(fd)
                if p.returncode != 0 or not out.strip().isdigit():
                    problems.append(f"test_loop: status {p.returncode}: {out.strip()!r} {err.strip()[-200:]!r}")
                elif refused:
                    problems.append(f"testLock took the lock: {refused} of Python's {attempts} attempts were refused "
                                    f"while the program only tested")
                elif attempts < 10:
                    skipped.append(f"testLock takes nothing: only {attempts} of Python's attempts overlapped the test")

            # 3. SQLite.
            sqlite3 = shutil.which("sqlite3")
            if sqlite3 is None:
                skipped.append("SQLite (no sqlite3 on PATH)")
            elif hold_ex and hold_sh and try_sh:
                sql = os.path.join(work, "app.db")
                p = subprocess.run([sqlite3, sql, "CREATE TABLE t(x); INSERT INTO t VALUES (1);"],
                                   capture_output=True, text=True, timeout=60)
                if p.returncode != 0:
                    problems.append(f"sqlite3 could not make the database: {p.stderr.strip()}")
                else:
                    h = Holder(hold_ex, sql)
                    p = subprocess.run([sqlite3, sql, "SELECT x FROM t;"], capture_output=True, text=True, timeout=60)
                    if p.returncode == 0 or "database is locked" not in p.stderr:
                        problems.append(f"sqlite3 read under the program's exclusive lock: status {p.returncode}, "
                                        f"{p.stdout.strip()!r} {p.stderr.strip()!r}")
                    h.end()
                    h = Holder(hold_sh, sql)
                    p = subprocess.run([sqlite3, sql, "SELECT x FROM t;"], capture_output=True, text=True, timeout=60)
                    if p.returncode != 0 or p.stdout.strip() != "1":
                        problems.append(f"sqlite3 could not read beside the program's shared lock: "
                                        f"{p.stdout.strip()!r} {p.stderr.strip()!r}")
                    p = subprocess.run([sqlite3, sql, "INSERT INTO t VALUES (2);"], capture_output=True, text=True,
                                       timeout=60)
                    if p.returncode == 0 or "database is locked" not in p.stderr:
                        problems.append(f"sqlite3 wrote under the program's shared lock: status {p.returncode}, "
                                        f"{p.stderr.strip()!r}")
                    h.end()
                    script = f"BEGIN EXCLUSIVE;\n.shell {try_sh} {sql}\nCOMMIT;\n.shell {try_sh} {sql}\n"
                    p = subprocess.run([sqlite3, sql], input=script, capture_output=True, text=True, timeout=60)
                    if p.stdout.split() != ["false", "true"]:
                        problems.append(f"the program's shared lock during and after sqlite3's BEGIN EXCLUSIVE: "
                                        f"{p.stdout.split()}, want ['false', 'true'] ({p.stderr.strip()!r})")
                    if test_ex:
                        # SQLite's hot-journal question (unixCheckReservedLock): is RESERVED held?
                        script = f"BEGIN IMMEDIATE;\n.shell {test_ex} {sql}\nCOMMIT;\n.shell {test_ex} {sql}\n"
                        p = subprocess.run([sqlite3, sql], input=script, capture_output=True, text=True, timeout=60)
                        if p.stdout.split() != ["Exclusive", "null"]:
                            problems.append(f"testLock of RESERVED during and after sqlite3's BEGIN IMMEDIATE: "
                                            f"{p.stdout.split()}, want ['Exclusive', 'null'] ({p.stderr.strip()!r})")

        # 4. Hard links (ADR 0255), natively and in the interpreter, against Python's os.stat and os.link.
        link_exe = build(compiler, work, "link", LINK, problems)
        rw_exe = build(compiler, work, "read_write", READ_WRITE, problems)
        for how, runner in (("natively", lambda exe, name, *a: [exe] + list(a)),
                            ("in the interpreter", lambda exe, name, *a:
                             [compiler, "run", "--interp", os.path.join(work, name + ".cee"), "--"] + list(a))):
            if not (link_exe and rw_exe):
                break
            first = os.path.join(work, "first")
            second = os.path.join(work, "second")
            with open(first, "wb") as f:
                f.write(b"by Python")
            p = subprocess.run(runner(link_exe, "link", first, second), capture_output=True, text=True, timeout=120)
            a, b = os.stat(first), os.stat(second)
            if p.stdout.strip() != "Ok" or (a.st_ino, a.st_dev) != (b.st_ino, b.st_dev) or b.st_nlink != 2:
                problems.append(f"File.link {how}: {p.stdout.strip()!r} {p.stderr.strip()[-200:]!r}, inodes "
                                f"{a.st_ino} and {b.st_ino}, {b.st_nlink} links: want one file with 2 links")
            p = subprocess.run(runner(link_exe, "link", first, second), capture_output=True, text=True, timeout=120)
            if "exists" not in p.stdout:
                problems.append(f"File.link {how} over an existing name: {p.stdout.strip()!r}, want AlreadyExists")
            os.remove(first)
            if os.stat(second).st_nlink != 1:
                problems.append(f"File.link {how}: {os.stat(second).st_nlink} links after one name was removed")
            # The other way: Python links, the program reads through one name and writes through the other.
            os.link(second, first)
            p = subprocess.run(runner(rw_exe, "read_write", first, second, "BY"), capture_output=True, text=True,
                               timeout=120)
            with open(first, "rb") as f:
                seen = f.read()
            if p.stdout.strip() != "by Python" or seen != b"BY Python":
                problems.append(f"a link made by os.link, {how}: read {p.stdout.strip()!r}, then {seen!r} through "
                                f"the first name, want 'by Python' and b'BY Python'")
            os.remove(first)
            os.remove(second)
        # The finding's use: one database under two names, whose locks are one file's.
        hold = build(compiler, work, "hold_link", program(HOLD, "Exclusive", RESERVED, 1), problems)
        try_link = build(compiler, work, "try_link", program(TRY, "Shared", RESERVED, 1), problems)
        if hold and try_link and link_exe:
            named = os.path.join(work, "named.db")
            other = os.path.join(work, "other.db")
            open(named, "wb").close()
            subprocess.run([link_exe, named, other], capture_output=True, timeout=60)
            h = Holder(hold, named)
            p = subprocess.run([try_link, other], capture_output=True, text=True, timeout=60)
            h.end()
            if h.said != "locked" or p.stdout.strip() != "false":
                problems.append(f"a lock through one name of a linked file: {h.said!r}, and through the other "
                                f"{p.stdout.strip()!r}, want 'locked' and 'false'")

        # 5. Portable lock rules (ADR 0256): the program under strict and portable against the rules, and strict
        # against Windows' own answers, live on Windows, recorded elsewhere.
        rules_exe = build(compiler, work, "rules", RULES, problems)
        rules_path = os.path.join(work, "rules.db")
        open(rules_path, "wb").close()
        recorded = {}
        for name, steps in RULE_SEQUENCES:
            for rules in ("strict", "portable"):
                want = rules_model(steps, rules)
                env = dict(os.environ, CEESAFE_LOCK_RULES=rules)
                for how in ("natively", "in the interpreter"):
                    if how == "natively":
                        if not rules_exe:
                            continue
                        argv = [rules_exe, rules_path] + rules_arguments(steps)
                    else:
                        argv = ([compiler, "run", "--interp", os.path.join(work, "rules.cee"), "--", rules_path] +
                                rules_arguments(steps))
                    p = subprocess.run(argv, capture_output=True, text=True, timeout=120, env=env)
                    got = p.stdout.split()
                    if p.returncode != 0:
                        got.append("panic" if "ceesafe panic: FileLocks." in p.stderr else f"status {p.returncode}")
                    if os.name == "nt" and rules == "portable" and got != want and len(got) == len(want) and \
                            got[-1] == "false" and want[-1] == "true":
                        continue  # a conversion `portable` lets through, which Windows itself refuses (§2.1)
                    if got != want:
                        problems.append(f"{rules} rules {how}, {name}: {got} {p.stderr.strip()[-200:]!r}, want {want}")
            strict = rules_model(steps, "strict")
            windows = None
            if os.name == "nt":
                windows = windows_answers(rules_path, steps[:len(strict)])
                recorded[name] = windows
            elif WINDOWS_RECORDED is not None:
                windows = WINDOWS_RECORDED.get(name)
                if windows is None:
                    problems.append(f"no Windows answers recorded for {name!r}")
            if windows is not None:
                # Every step strict lets through, Windows grants; the step strict refuses, Windows refuses, or grants
                # over the handle's own lock by stacking the two, which no POSIX host does (§2.2).
                for i, answer in enumerate(strict):
                    want = ("granted",) if answer != "panic" else ("refused", "stacked")
                    if windows[i] not in want:
                        problems.append(f"strict and Windows differ at step {i + 1} of {name}: strict "
                                        f"{answer}, Windows {windows[i]}")
        if os.name == "nt":
            print("WINDOWS_RECORDED = " + repr(recorded))
        elif WINDOWS_RECORDED is None:
            skipped.append("strict against Windows' answers (none recorded)")
    print(f"file_lock_check: Files of one program, other processes' locks, SQLite, links, portable rules; {len(problems)} problems")
    for s in skipped:
        print("  SKIP " + s)
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
