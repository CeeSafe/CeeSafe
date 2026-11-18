#!/usr/bin/env python3
"""Named shared memory (ADR 0172): checks the corpus cannot make, since they need several processes.

One program is built and run in several ways, with a name that has this script's process id in it:

1. `parent NAME`: creates the memory, starts itself three times as `adds NAME` (`Process.spawn`), each of which opens
   the memory and adds to word 0 with `fetchAdd` and to word 1 with a `compareExchange` loop, 100 000 times; after
   they end both words are 300 000. Then: a name that is not there (`NotFound`), `create` of a name that is taken
   (`AlreadyExists`), names that are not names and a size of zero (`InvalidInput`), the size rounded to pages, a
   second mapping in the same process that sees the first one's word;
2. `probe NAME` after the parent has ended: the name is gone;
3. `range NAME`: a word past the end panics;
4. POSIX: `holds NAME` is killed while it holds the memory, and leaves the name (`probe` finds it); `removes NAME`
   takes it, and a second `removes` finds nothing. On Windows the name goes with the process.

The interpreter has no shared memory: `SharedMemory.create` is `IoError.Unsupported` there.

usage: python3 tools/shared_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

PROGRAM = r'''const ROUNDS: u64 = 100000;

/// A child: adds to word 0 by `fetchAdd` and to word 1 by a compare-and-swap loop.
fn adds(name: StringView) -> i32 {
    let memory = match (SharedMemory.open(name)) {
        Ok(m) => m,
        Err(_) => { return 4; },
    };
    var i: u64 = 0;
    while (i < ROUNDS) {
        memory.word(0).fetchAdd(1);
        let slow = memory.word(1);
        var seen = slow.load();
        loop {
            match (slow.compareExchange(seen, seen + 1)) {
                Ok(_) => { break; }
                Err(now) => { seen = now; }
            }
        }
        i += 1;
    }
    return 0;
}

fn kind(e: IoError) -> StringView {
    return match (e) {
        IoError.NotFound => "not found",
        IoError.AlreadyExists => "already exists",
        IoError.InvalidInput => "invalid input",
        IoError.Unsupported => "unsupported",
        _ => "another error",
    };
}

fn parent(name: StringView) {
    let me = Process.executable()!;
    let memory = SharedMemory.create(name, 100)!;
    Console.writeLine(if (memory.length() >= 4096 && memory.length() % 4096 == 0) "whole pages" else "not pages");
    Console.writeLine(if (memory.words() == memory.length() / 8) "words" else "words are off");
    Console.writeLine(if (memory.word(0).load() == 0 && memory.word(memory.words() - 1).load() == 0) "zeroed" else "not zeroed");
    var children = Vector<ChildProcess>.create();
    for (n in 0..3) {
        var arguments = Vector<StringView>.create();
        arguments.push("adds")!;
        arguments.push(name)!;
        children.push(Process.spawn(me.view(), arguments, ChildOptions.inherit())!)!;
    }
    var failed = false;
    for (i in 0..children.length) {
        match (children[i].wait()!) {
            ChildExit.Status(0) => {}
            _ => { failed = true; }
        }
    }
    Console.writeLine(if (failed) "a child failed" else "three children ended");
    Console.writeLine(memory.word(0).load());
    Console.writeLine(memory.word(1).load());
    // A second mapping in this process is the same memory, at another address.
    {
        let again = SharedMemory.open(name)!;
        again.word(2).store(77);
        Console.writeLine(if (memory.word(2).load() == 77 && again.length() == memory.length()) "one memory, mapped twice" else "two memories");
    }
    Console.writeLine(memory.word(2).load()); // still there: only the creator's drop removes the name
    match (SharedMemory.create(name, 100)) {
        Ok(_) => { Console.writeLine("made twice"); }
        Err(e) => { Console.writeLine(kind(e)); }
    }
    match (SharedMemory.open("cee-0172-no-such-name")) {
        Ok(_) => { Console.writeLine("opened nothing"); }
        Err(e) => { Console.writeLine(kind(e)); }
    }
    Console.writeLine(if (SharedMemory.create("", 8).isErr() && SharedMemory.create("a/b", 8).isErr()
        && SharedMemory.create("0123456789012345678901234567890", 8).isErr() && SharedMemory.open("a b").isErr()
        && SharedMemory.remove("x*").isErr() && SharedMemory.create("cee-0172-zero", 0).isErr()) "bad names and sizes refused" else "a bad name passed");
    match (SharedMemory.create("a/b", 8)) {
        Ok(_) => {}
        Err(e) => { Console.writeLine(kind(e)); }
    }
    Console.writeLine("done");
}

async fn nap(ms: u64) -> Result<void, AsyncError> => await Timer.sleep(ms);

fn main() {
    let how = Process.argument(1) ?? "";
    let name = Process.argument(2) ?? "";
    if (how == "adds") {
        Process.exit(adds(name));
    }
    if (how == "parent") {
        parent(name);
        return;
    }
    if (how == "probe") {
        match (SharedMemory.open(name)) {
            Ok(_) => { Console.writeLine("found"); }
            Err(e) => { Console.writeLine(kind(e)); }
        }
        return;
    }
    if (how == "removes") {
        match (SharedMemory.remove(name)) {
            Ok() => { Console.writeLine("removed"); }
            Err(e) => { Console.writeLine(kind(e)); }
        }
        return;
    }
    if (how == "holds") {
        let memory = SharedMemory.create(name, 8)!;
        memory.word(0).store(41);
        Console.writeLine("holding");
        Console.flush();
        let _ = Async.run(nap(30000));
        return;
    }
    if (how == "range") {
        let memory = SharedMemory.create(name, 8)!;
        Console.writeLine("before");
        Console.flush();
        memory.word(memory.words()).store(1);
        Console.writeLine("after");
    }
}
'''

INTERPRETED = r'''fn main() {
    match (SharedMemory.create("cee-0172-interp", 64)) {
        Ok(_) => { Console.writeLine("made"); }
        Err(e) => { Console.writeLine(if (e is IoError.Unsupported) "unsupported" else "another error"); }
    }
    Console.writeLine(if (SharedMemory.create("a/b", 64).isErr()) "a bad name is refused" else "a bad name passed");
}
'''

EXPECTED_PARENT = [
    "whole pages",
    "words",
    "zeroed",
    "three children ended",
    "300000",
    "300000",
    "one memory, mapped twice",
    "77",
    "already exists",
    "not found",
    "bad names and sizes refused",
    "invalid input",
    "done",
]
DEADLINE = 120.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    windows = os.name == "nt"
    name = f"cee-0172-{os.getpid()}"
    problems = []
    cases = 0

    with tempfile.TemporaryDirectory(prefix="ceesafe-shared-") as work:
        source = os.path.join(work, "shared.cee")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "shared.exe" if windows else "shared")
        built = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:800]}")
            print("shared_check: 0 cases: 1 problems")
            return 1

        def run(*words, timeout=DEADLINE):
            return subprocess.run([exe, *words], capture_output=True, text=True, timeout=timeout)

        def expect(what, r, lines):
            nonlocal cases
            cases += 1
            if r.stdout.splitlines() != lines:
                problems.append(f"{what}: wrote {r.stdout.splitlines()!r}, expected {lines!r} "
                                f"(status {r.returncode}, {r.stderr.strip()[:300]!r})")

        # 1. Three processes add to the same words.
        expect("parent", run("parent", name), EXPECTED_PARENT)
        # 2. The creator's end took the name.
        expect("after the parent", run("probe", name), ["not found"])
        # 3. A word past the end.
        r = run("range", name)
        cases += 1
        if r.stdout.splitlines() != ["before"] or r.returncode == 0 or "SharedMemory.word" not in r.stderr:
            problems.append(f"range: wrote {r.stdout!r}, status {r.returncode}, {r.stderr.strip()[:300]!r}")
        if not windows:
            # A panic runs no drop: the name is left, as for any creator that does not end normally.
            expect("after the panic", run("removes", name), ["removed"])
        # 4. A creator that is killed.
        holder = subprocess.Popen([exe, "holds", name], stdout=subprocess.PIPE, text=True)
        line = holder.stdout.readline().strip()
        cases += 1
        if line != "holding":
            problems.append(f"holds: wrote {line!r}")
        expect("while it is held", run("probe", name), ["found"])
        holder.kill()
        holder.wait(10)
        time.sleep(0.2)
        if windows:
            expect("after the kill (Windows: gone with the process)", run("probe", name), ["not found"])
        else:
            expect("after the kill: the name is left", run("probe", name), ["found"])
            expect("remove", run("removes", name), ["removed"])
            expect("remove again", run("removes", name), ["not found"])
            expect("after remove", run("probe", name), ["not found"])

        # The interpreter.
        path = os.path.join(work, "interpreted.cee")
        with open(path, "w", encoding="utf-8") as f:
            f.write(INTERPRETED)
        r = subprocess.run([compiler, "run", "--interp", path], capture_output=True, text=True, timeout=DEADLINE)
        expect("interpreted", r, ["unsupported", "a bad name is refused"])

    for problem in problems:
        print(f"FAIL {problem}")
    print(f"shared_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
