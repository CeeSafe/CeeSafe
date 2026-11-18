#!/usr/bin/env python3
"""Allocation budgets (ADR 0170): checks the corpus cannot make, since its programs also run in the interpreter, which
holds nothing back.

Two programs are built and run:

1. without tasks: under a budget of 1 MiB, 4 KiB blocks are refused near the 256th; what is held is within the limit;
   freed blocks come off and there is room again; outside the budget nothing is limited;
2. with tasks: a task under a budget is stopped while the task beside it gets all it asks for; two tasks started
   under one budget share it; a budget inside a budget stops at its own limit, and its blocks count against the outer
   one; when all is done no budget is current.

Each is also built with the address and undefined-behaviour sanitizers where the C compiler has them. A third runs
in the interpreter: a budget is entered and left there, and none is current.

usage: python3 tools/budget_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

SYNC = r'''/// Pushes 4 KiB blocks until one fails; how many were pushed.
fn grow(blocks: borrow mut Vector<Box<[u8; 4096]>>) -> u64 {
    var n: u64 = 0;
    loop {
        let block = match (Box.create([0u8; 4096])) {
            Ok(b) => b,
            Err(_) => { break; },
        };
        match (blocks.push(block)) {
            Ok() => {}
            Err(_) => { break; }
        }
        n += 1;
        if (n == 100000) {
            break; // no budget stopped it
        }
    }
    return n;
}

fn main() {
    Console.writeLine(if (Budget.held() is null) "no budget" else "a budget?");
    var kept = Vector<Box<[u8; 4096]>>.create();
    {
        let budget = Budget.enter(1048576)!;
        Console.writeLine(Budget.limit() ?? 0);
        let n = grow(mut kept);
        // 1 MiB of 4 KiB blocks: at most 256, fewer for the vector's own storage and the allocator's rounding.
        Console.writeLine(if (n >= 200 && n <= 256) "stopped near 1 MiB" else "not stopped where expected");
        let held = Budget.held() ?? 0;
        Console.writeLine(if (held <= 1048576 && held >= 800000) "held within the limit" else "held is off");
        kept.clear();
        Console.writeLine(if ((Budget.held() ?? 1) < 300000) "freed blocks came off" else "frees not counted");
        // Room again.
        Console.writeLine(if (grow(mut kept) >= 150) "room again" else "no room");
    }
    Console.writeLine(if (Budget.held() is null) "left" else "still in");
    kept.clear();
    Console.writeLine(if (grow(mut kept) == 100000) "no limit outside" else "limited outside");
}
'''

TASKS = r'''/// Pushes 4 KiB blocks, giving way between them, until one fails or `most` are pushed; how many.
async fn grow(blocks: borrow mut Vector<Box<[u8; 4096]>>, most: u64) -> Result<u64, AsyncError> {
    var n: u64 = 0;
    while (n < most) {
        let block = match (Box.create([0u8; 4096])) {
            Ok(b) => b,
            Err(_) => { break; },
        };
        match (blocks.push(block)) {
            Ok() => {}
            Err(_) => { break; }
        }
        n += 1;
        if (n % 16 == 0) {
            await Timer.sleep(1)?;
        }
    }
    return Ok(n);
}

/// Under a budget of 1 MiB: how many blocks it got.
async fn bounded() -> Result<u64, AsyncError> {
    let budget = match (Budget.enter(1048576)) {
        Ok(b) => b,
        Err(_) => { return Ok(0); },
    };
    var kept = Vector<Box<[u8; 4096]>>.create();
    return await grow(mut kept, 100000);
}

/// Under no budget of its own: how many blocks it got of the 2000 it asks for.
async fn free() -> Result<u64, AsyncError> {
    var kept = Vector<Box<[u8; 4096]>>.create();
    return await grow(mut kept, 2000);
}

/// Stores what  (under a budget of its own) or  got.
async fn into(out: borrow AtomicU64, limited: bool) -> Result<void, AsyncError> {
    let n = if (limited) await bounded()? else await free()?;
    out.store(n);
    return Ok();
}

/// Two tasks started under one budget share it.
async fn shared() -> Result<u64, AsyncError> {
    let budget = match (Budget.enter(1048576)) {
        Ok(b) => b,
        Err(_) => { return Ok(0); },
    };
    let one = AtomicU64.create(0);
    let two = AtomicU64.create(0);
    await Async.all(async () => await into(one, false), async () => await into(two, false))?;
    // Neither holds more than the budget; when one ends and frees its blocks, the other grows into the room, so
    // together they may get more than 256 over time, never at once.
    let each = one.load() <= 256 && two.load() <= 256 && one.load() < 2000 && two.load() < 2000;
    return Ok(if (each) one.load() + two.load() else 9999);
}

/// A budget inside a budget: the inner one's limit first, and its blocks count against the outer one.
async fn nested() -> Result<u64, AsyncError> {
    let outer = match (Budget.enter(1048576)) {
        Ok(b) => b,
        Err(_) => { return Ok(0); },
    };
    var kept = Vector<Box<[u8; 4096]>>.create();
    var inside: u64 = 0;
    {
        let inner = match (Budget.enter(262144)) {
            Ok(b) => b,
            Err(_) => { return Ok(0); },
        };
        inside = await grow(mut kept, 100000)?;
    }
    // The inner budget is gone; what was allocated under it still counts against the outer one.
    let after = await grow(mut kept, 100000)?;
    return Ok(inside * 1000 + after);
}

async fn all() -> Result<void, AsyncError> {
    // A task under a budget beside one that is not: the second gets all it asks for, while the first is stopped.
    let first = AtomicU64.create(0);
    let second = AtomicU64.create(0);
    await Async.all(async () => await into(first, true), async () => await into(second, false))?;
    await AsyncConsole.writeLine(if (first.load() >= 200 && first.load() <= 256) "bounded: stopped near 1 MiB" else "bounded: off")?;
    await AsyncConsole.writeLine(if (second.load() == 2000) "beside it: all 2000" else "beside it: limited")?;
    let sum = await shared()?;
    await AsyncConsole.writeLine(if (sum >= 200 && sum <= 512) "two tasks share one budget" else "two tasks: off")?;
    let n = await nested()?;
    let inside = n / 1000;
    let after = n % 1000;
    await AsyncConsole.writeLine(if (inside >= 40 && inside <= 64) "inner: stopped near 256 KiB" else "inner: off")?;
    await AsyncConsole.writeLine(if (inside + after >= 200 && inside + after <= 256) "outer: counts both" else "outer: off")?;
    await AsyncConsole.writeLine(if (Budget.held() is null) "none left current" else "one still current")?;
    return Ok();
}

fn main() {
    match (Async.run(all())) {
        Ok() => {}
        Err(_) => { Console.writeLine("an async error"); }
    }
}
'''

INTERPRETED = r'''fn main() {
    let budget = Budget.enter(4096)!;
    Console.writeLine(if (Budget.held() is null) "entered, and nothing held back" else "a budget is current");
    var kept = Vector<u8>.create();
    kept.reserve(65536)!;
    Console.writeLine(kept.capacity >= 65536);
}
'''

EXPECTED = {
    "sync": ["no budget", "1048576", "stopped near 1 MiB", "held within the limit", "freed blocks came off", "room again",
             "left", "no limit outside"],
    "tasks": ["bounded: stopped near 1 MiB", "beside it: all 2000", "two tasks share one budget",
              "inner: stopped near 256 KiB", "outer: counts both", "none left current"],
}
DEADLINE = 120.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-budget-") as work:
        for stem, source in (("sync", SYNC), ("tasks", TASKS)):
            path = os.path.join(work, stem + ".cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source)
            for label, flags in (("", []), (" (sanitizers)", ["--cflag", "-fsanitize=address,undefined"])):
                exe = os.path.join(work, stem + ("-san" if flags else ""))
                b = subprocess.run([compiler, "build", path, "-o", exe] + flags, capture_output=True, text=True)
                if b.returncode != 0:
                    if flags:
                        continue  # a C compiler without the sanitizers: the plain build is the check
                    problems.append(f"{stem}: build failed: {b.stderr.strip()[:600]}")
                    continue
                cases += 1
                try:
                    r = subprocess.run([exe], capture_output=True, text=True, timeout=DEADLINE)
                except subprocess.TimeoutExpired:
                    problems.append(f"{stem}{label}: did not end in time")
                    continue
                if r.stdout.splitlines() != EXPECTED[stem] or r.returncode != 0:
                    problems.append(f"{stem}{label}: wrote {r.stdout.splitlines()!r}, expected {EXPECTED[stem]!r} "
                                    f"(status {r.returncode}, {r.stderr.strip()[:300]!r})")
        path = os.path.join(work, "interpreted.cee")
        with open(path, "w", encoding="utf-8") as f:
            f.write(INTERPRETED)
        cases += 1
        r = subprocess.run([compiler, "run", "--interp", path], capture_output=True, text=True, timeout=DEADLINE)
        if r.stdout.splitlines() != ["entered, and nothing held back", "true"]:
            problems.append(f"interpreted: wrote {r.stdout!r} ({r.stderr.strip()[:300]!r})")
    for problem in problems:
        print(f"FAIL {problem}")
    print(f"budget_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
