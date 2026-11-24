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
    print(f"file_lock_check: Files of one program, other processes' locks, SQLite; {len(problems)} problems")
    for s in skipped:
        print("  SKIP " + s)
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
