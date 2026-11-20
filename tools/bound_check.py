#!/usr/bin/env python3
"""A bound on running without waiting (ADR 0186): checks the corpus cannot make, since a task past its bound is
ended by time natively and by fuel in the interpreter, and several cases end the process or need the clock.

1. `survive`, built with --cflag -DCEE_POLLS=1 and run natively, and run in the interpreter, the same lines each time:
   a unit's own bound (`spawnIsolatedBound`) ends its spinning child where the program has none; then, with the
   program's bound, nine ways to spin each end their isolated child alone, with PanicKind.Overran (a `while` and a
   `for` in an async body, a `loop`, a recursion with no loop, a sync helper's `while`, a counted `for` in a sync
   helper, a recursion through a function value, through two functions, and through a `dyn` method), while a sibling that waits on a timer finishes; a child that waits
   often is never ended; a child that holds a `Critical` past its bound is ended after it, not in it; and
   `Async.overruns()` counts each.
2. `ends`, built so, one case an argument, natively (they need the clock):
   - `count`: a task in no unit that computes for 100 ms past a 30 ms bound is counted and reported, not ended;
   - `panic`: with RunBound.Panic, such a task ends the process with the overrun's panic line;
   - `critical`: an isolated child in a `Critical` for 100 ms, past a 30 ms bound, is ended only after it;
   - `lenient`: a unit's own bound of 5 s lets its child compute for 100 ms past the program's 30 ms;
   - `callback`: a spin under C's frames (a comparison function `qsort` calls) is not ended there, only once back;
   - `early`: a child that computes 20 ms at a time between waits of 5 ms, under a 50 ms bound, is never ended;
   - `registry`: four threads that each start and end fifty runs while the bound is in force, then a spin, ended;
   - `stale`: a poll word set for a resume that is not the running one (a monitor's write that landed after the
     task waited, forced by a C file) ends nothing;
   - `latency`: a sibling that waits on a timer waits at most about the bound behind a spinning child (between the
     bound and 3 times it), never less.
3. `nopolls`: built without CEE_POLLS, Async.runBound answers false.
4. The same code: each of CeeSafe's benchmarks (tests/reference/benchmarks/suite) and the corpus's emit-c/adr0186
   compile, without CEE_POLLS, to the same assembly as their C with every poll taken out.

usage: python3 tools/bound_check.py --compiler out/ceesafec [--cc cc]
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

COMMON = r'''enum Fail {
    NoMemory,
    Crashed,
    Other,
}

async fn nap(ms: u64) -> Result<void, Fail> {
    match (await Timer.sleep(ms)) {
        Ok() => { return Ok(); }
        Err(_) => { return Err(Fail.Other); }
    }
}

fn cell() -> Result<Arc<AtomicU64>, Fail> {
    match (Arc<AtomicU64>.create(AtomicU64.create(0))) {
        Ok(a) => { return Ok(a); }
        Err(_) => { return Err(Fail.NoMemory); }
    }
}

fn crashChannel() -> Result<Channel<TaskCrash>, Fail> {
    match (Channel<TaskCrash>.create(16)) {
        Ok(c) => { return Ok(c); }
        Err(_) => { return Err(Fail.NoMemory); }
    }
}

/// How many crashes the watcher was told of, and how many of them were overruns.
fn overranOf(crashes: Channel<TaskCrash>) -> u64 {
    var seen: u64 = 0;
    loop {
        match (crashes.tryRecv()) {
            Some(crash) => {
                seen += if (crash.kind() is PanicKind.Overran) 1001u64 else 1u64;
            }
            null => { break; }
        }
    }
    return seen;
}

/// Spins for ever, in a sync function.
fn churn() -> u64 {
    var x: u64 = 1;
    while (x != 0) {
        x = (x ^ (x >> 1)) | 1;
    }
    return x;
}

/// Spins in a counted loop of `end` iterations, an end known only when it starts: it runs in chunks.
fn counted(end: u64) -> u64 {
    var x: u64 = 0;
    for (i in 0..end) {
        x = x ^ i;
    }
    return x;
}

/// 2^n calls, n deep: no loop.
fn walk(n: u32, salt: u64) -> u64 {
    if (n == 0) {
        return salt | 1;
    }
    return walk(n - 1, salt) ^ walk(n - 1, salt + 1);
}

/// The same through a function value.
fn hop(n: u32, salt: u64, next: @effects() fn(u32, u64) -> u64) -> u64 {
    if (n == 0) {
        return salt | 1;
    }
    return next(n - 1, salt) ^ next(n - 1, salt + 1);
}

fn hopped(n: u32, salt: u64) -> u64 => hop(n, salt, hopped);

/// The same through two functions.
fn ping(n: u32, salt: u64) -> u64 {
    if (n == 0) {
        return salt | 1;
    }
    return pong(n - 1, salt) ^ pong(n - 1, salt + 1);
}

fn pong(n: u32, salt: u64) -> u64 {
    if (n == 0) {
        return salt;
    }
    return ping(n - 1, salt) ^ ping(n - 1, salt + 1);
}

/// The same through a `dyn` method.
interface Walker {
    @effects()
    fn step(borrow this, n: u32, salt: u64) -> u64;
}

struct Stepper {
    unused: u8;
}

impl Walker for Stepper {
    fn step(borrow this, n: u32, salt: u64) -> u64 {
        if (n == 0) {
            return salt | 1;
        }
        let me: borrow dyn Walker = this;
        return me.step(n - 1, salt) ^ me.step(n - 1, salt + 1);
    }
}

fn stepped(n: u32, salt: u64) -> u64 {
    let s = Stepper { unused: 0 };
    let w: borrow dyn Walker = borrow s;
    return w.step(n, salt);
}

/// Spins in the way `how` says, after a wait.
async fn spins(how: u32, sink: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    var x: u64 = 1;
    if (how == 0) {
        while (x != 0) {
            x = (x ^ (x >> 1)) | 1;
        }
    } else if (how == 1) {
        for (i in 0..281474976710655u64) {
            x = x ^ i;
        }
    } else if (how == 2) {
        loop {
            x = (x ^ (x >> 1)) | 1;
            if (x == 0) {
                break;
            }
        }
    } else if (how == 3) {
        x = walk(48, 1);
    } else if (how == 4) {
        x = churn();
    } else if (how == 5) {
        x = counted(281474976710655);
    } else if (how == 6) {
        x = hopped(48, 1);
    } else if (how == 7) {
        x = ping(48, 1);
    } else {
        x = stepped(48, 1);
    }
    sink.get().store(x);
    return Ok();
}
'''

SURVIVE = COMMON + r'''
/// Twenty waits on a timer.
async fn ticks(done: Arc<AtomicU64>) -> Result<void, Fail> {
    var k: u64 = 0;
    while (k < 20) {
        await nap(1)?;
        k += 1;
    }
    done.get().store(k);
    return Ok();
}

/// Ten short computations, each followed by a wait.
async fn steady(done: Arc<AtomicU64>) -> Result<void, Fail> {
    var round: u64 = 0;
    var x: u64 = 0;
    while (round < 10) {
        for (i in 0..20000u64) {
            x = x ^ i;
        }
        await nap(1)?;
        round += 1;
    }
    done.get().store(round + (x & 0));
    return Ok();
}

/// A Critical past the bound, then a spin.
async fn holds(done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    var x: u64 = 0;
    {
        let critical = Critical.enter();
        for (i in 0..200000u64) {
            x = x ^ i;
        }
        done.get().store(1 + (x & 0)); // the last of it: a task ended inside it never gets here
    }
    let y = churn();
    done.get().store(y);
    return Ok();
}

struct Outcome {
    unitBound: u64;   // the watcher's count for the unit's own bound
    spun: u64;        // for the seven spinners
    ticked: u64;
    steadyCrashes: u64;
    steadyDone: u64;
    held: u64;        // for the Critical
    heldDone: u64;
    overruns: u64;
}

async fn all() -> Result<Outcome, Fail> {
    let sink = cell()?;
    // A unit's own bound where the program has none.
    let alone = crashChannel()?;
    let r0 = await Async.tasks(4, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, alone)) {
            return Err(Fail.NoMemory);
        }
        let mine = sink.clone();
        await set.spawnIsolatedBound(30, Fail.Crashed, async move () => await spins(0, mine))?;
        return Ok();
    });
    let _ = r0;
    let unitBound = overranOf(alone);
    // The program's bound from here.
    let _ = Async.runBound(40, RunBound.Count);
    let spinners = crashChannel()?;
    let ticked = cell()?;
    let r1 = await Async.tasks(16, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, spinners)) {
            return Err(Fail.NoMemory);
        }
        var how: u32 = 0;
        while (how < 9) {
            let w = how;
            let mine = sink.clone();
            await set.spawnIsolated(Fail.Crashed, async move () => await spins(w, mine))?;
            how += 1;
        }
        let t = ticked.clone();
        await set.spawn(async move () => await ticks(t))?;
        return Ok();
    });
    let _ = r1;
    let spun = overranOf(spinners);
    let quiet = crashChannel()?;
    let steadyDone = cell()?;
    let r2 = await Async.tasks(4, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, quiet)) {
            return Err(Fail.NoMemory);
        }
        let d = steadyDone.clone();
        await set.spawnIsolated(Fail.Crashed, async move () => await steady(d))?;
        return Ok();
    });
    let _ = r2;
    let steadyCrashes = overranOf(quiet);
    let critical = crashChannel()?;
    let heldDone = cell()?;
    let r3 = await Async.tasks(4, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, critical)) {
            return Err(Fail.NoMemory);
        }
        let d = heldDone.clone();
        await set.spawnIsolated(Fail.Crashed, async move () => await holds(d))?;
        return Ok();
    });
    let _ = r3;
    let held = overranOf(critical);
    return Ok(
        Outcome {
            unitBound: unitBound,
            spun: spun,
            ticked: ticked.get().load(),
            steadyCrashes: steadyCrashes,
            steadyDone: steadyDone.get().load(),
            held: held,
            heldDone: heldDone.get().load(),
            overruns: Async.overruns(),
        },
    );
}

fn say(ok: bool, what: StringView) {
    Console.write(if (ok) "ok   " else "FAIL ");
    Console.writeLine(what);
}

fn main() {
    say(Async.runBound(0, RunBound.Count), "Async.runBound answers true");
    match (Async.run(all())) {
        Ok(o) => {
            say(o.unitBound == 1001, "a unit's own bound ends its spinning child, with no bound for the program");
            say(o.spun == 9009, "nine ways to spin, each ended alone with Overran");
            say(o.ticked == 20, "the ticker beside them finished");
            say(o.steadyCrashes == 0 && o.steadyDone == 10, "a child that waits often is never ended");
            say(o.held == 1001 && o.heldDone == 1, "a Critical held past the bound: ended after it, not in it");
            say(o.overruns == 11, "Async.overruns() counted eleven");
        }
        Err(_) => { Console.writeLine("FAIL the run failed"); }
    }
}
'''

SURVIVE_LINES = [
    "ok   Async.runBound answers true",
    "ok   a unit's own bound ends its spinning child, with no bound for the program",
    "ok   nine ways to spin, each ended alone with Overran",
    "ok   the ticker beside them finished",
    "ok   a child that waits often is never ended",
    "ok   a Critical held past the bound: ended after it, not in it",
    "ok   Async.overruns() counted eleven",
]

ENDS = COMMON + r'''
@header("stdlib.h")
extern "C" {
    @effects()
    fn qsort(base: *mut void, count: usize, size: usize, compare: extern "C" fn(*void, *void) -> i32);
}

extern "C" {
    @effects()
    fn bound_check_stale(); // stale.c: this thread's poll word set for a resume that is not this one
}

/// Computes without waiting for `ms` milliseconds by the clock.
fn busy(ms: u64) -> u64 {
    let start = Timer.now();
    var x: u64 = 0;
    while (Timer.now() - start < ms) {
        x = (x ^ (x >> 1)) | 1;
    }
    return x;
}

/// C's qsort calls this: it computes for 100 ms each time, with C's frames between the executor and its polls.
/// Safety: qsort gives two elements of the array.
@callback
unsafe fn slowly(a: *void, b: *void) -> i32 {
    let first = (a as *u32).read();
    let second = (b as *u32).read();
    let _ = busy(100);
    return if (first < second) -1 else if (first > second) 1 else 0;
}

/// Safety: the array has three elements of four bytes, and `slowly` reads elements only.
fn sorted() -> u32 {
    var numbers = [3u32, 1, 2];
    unsafe { qsort(borrow mut numbers as *mut [u32; 3] as *mut void, 3, 4, slowly); }
    return numbers[0];
}

async fn computes(ms: u64, done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    let x = busy(ms);
    done.get().store(1 + (x & 0));
    return Ok();
}

async fn criticalThenSpin(start: Arc<AtomicU64>, done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    start.get().store(Timer.now());
    {
        let critical = Critical.enter();
        let _ = busy(100);
    }
    done.get().store(Timer.now());
    let _ = churn();
    return Ok();
}

async fn callbackThenSpin(start: Arc<AtomicU64>, done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    start.get().store(Timer.now());
    let first = sorted();
    done.get().store(Timer.now() + (first as u64 & 0));
    let _ = churn();
    return Ok();
}

/// Its poll word is set for another resume, as the monitor's late write would leave it: it is not ended.
/// Safety: bound_check_stale writes one word of this thread's.
async fn staleWord(done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    unsafe { bound_check_stale(); }
    var x: u64 = 0;
    var k: u64 = 0;
    while (k < 5000) {
        x = x ^ k;
        k += 1;
    }
    done.get().store(1 + (x & 0));
    return Ok();
}

/// Computes 20 ms at a time between waits of 5 ms. A sleep whose deadline has passed when it looks returns without
/// waiting (a loaded machine), so a wait is 5 ms, and only three missed in a row would come to a 50 ms bound.
async fn waitsOften(done: Arc<AtomicU64>) -> Result<void, Fail> {
    var round: u64 = 0;
    while (round < 20) {
        let _ = busy(20);
        await nap(5)?;
        round += 1;
    }
    done.get().store(round);
    return Ok();
}

async fn quick() -> Result<u64, Fail> {
    await nap(1)?;
    return Ok(1);
}

/// `k` runs one after another, each joining the monitor's registry and leaving it.
fn shortRuns(k: u64) -> u64 {
    var total: u64 = 0;
    var i: u64 = 0;
    while (i < k) {
        match (Async.run(quick())) {
            Ok(v) => { total += v; }
            Err(_) => {}
        }
        i += 1;
    }
    return total;
}

/// The longest gap between thirty waits of a millisecond.
async fn gaps(longest: Arc<AtomicU64>) -> Result<void, Fail> {
    var k: u32 = 0;
    var last = Timer.now();
    var most: u64 = 0;
    while (k < 30) {
        await nap(1)?;
        let now = Timer.now();
        if (now - last > most) {
            most = now - last;
        }
        last = now;
        k += 1;
    }
    longest.get().store(most);
    return Ok();
}

async fn spinsTimed(start: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(5)?;
    start.get().store(Timer.now());
    let _ = churn();
    return Ok();
}

/// One case: [crashes seen (1001 an overrun), what the case's cells hold].
async fn one(how: u32) -> Result<Vector<u64>, Fail> {
    let crashes = crashChannel()?;
    let a = cell()?;
    let b = cell()?;
    let r = await Async.tasks(8, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, crashes)) {
            return Err(Fail.NoMemory);
        }
        let x = a.clone();
        let y = b.clone();
        if (how == 0) {
            await set.spawn(async move () => await computes(100, x))?;
        } else if (how == 1) {
            await set.spawn(async move () => await spins(0, x))?;
        } else if (how == 2) {
            await set.spawnIsolated(Fail.Crashed, async move () => await criticalThenSpin(x, y))?;
        } else if (how == 3) {
            await set.spawnIsolatedBound(5000, Fail.Crashed, async move () => await computes(100, x))?;
        } else if (how == 4) {
            await set.spawnIsolated(Fail.Crashed, async move () => await callbackThenSpin(x, y))?;
        } else if (how == 5) {
            await set.spawnIsolated(Fail.Crashed, async move () => await waitsOften(x))?;
        } else if (how == 7) {
            await set.spawnIsolated(Fail.Crashed, async move () => await staleWord(x))?;
        } else if (how == 8) {
            await set.spawnIsolated(Fail.Crashed, async move () => await spins(0, x))?;
        } else {
            await set.spawnIsolated(Fail.Crashed, async move () => await spinsTimed(x))?;
            await set.spawn(async move () => await gaps(y))?;
        }
        return Ok();
    });
    let _ = r;
    var out = Vector<u64>.create();
    match (out.push(overranOf(crashes))) {
        Ok() => {}
        Err(_) => { return Err(Fail.NoMemory); }
    }
    match (out.push(a.get().load())) {
        Ok() => {}
        Err(_) => { return Err(Fail.NoMemory); }
    }
    match (out.push(b.get().load())) {
        Ok() => {}
        Err(_) => { return Err(Fail.NoMemory); }
    }
    return Ok(out);
}

fn main() {
    let how = Process.argument(1) ?? "";
    let which: u32 = if (how == "count") 0 else if (how == "panic") 1 else if (how == "critical") 2
        else if (how == "lenient") 3 else if (how == "callback") 4 else if (how == "early") 5
        else if (how == "stale") 7 else if (how == "registry") 8 else 6;
    let bound: u64 = if (which == 5 || which == 6) 50 else 30;
    let mode = if (which == 1) RunBound.Panic else RunBound.Count;
    if (!Async.runBound(bound, mode)) {
        Console.writeLine("no polls");
        return;
    }
    if (which == 8) {
        // Threads and runs that start and end while a bound is in force, before a spin it must still end.
        var runs: u64 = 0;
        let a = Thread.spawn(move () => shortRuns(50))!;
        let b = Thread.spawn(move () => shortRuns(50))!;
        let c = Thread.spawn(move () => shortRuns(50))!;
        runs += shortRuns(50);
        runs += a.join() + b.join() + c.join();
        Console.write("runs ");
        Console.writeLine(runs);
    }
    match (Async.run(one(which))) {
        Ok(v) => {
            Console.write("crashes ");
            Console.write(v[0]);
            Console.write(" cells ");
            Console.write(v[1]);
            Console.write(" ");
            Console.write(v[2]);
            Console.write(" overruns ");
            Console.writeLine(Async.overruns());
        }
        Err(_) => { Console.writeLine("failed"); }
    }
}
'''

STALE_C = r'''/* bound_check's `stale` case (ADR 0186 §2.2, step 5): this thread's poll word, set for a resume that is not the
   one running, as a monitor's write that lands after the task waited leaves it. */
#include <stdint.h>
extern _Thread_local uint32_t ceesafe_poll_word;
void ceesafe_poll_set(uint32_t *word, uint32_t epoch); /* the monitor's write, counted as it counts it */
void bound_check_stale(void);
void bound_check_stale(void) { ceesafe_poll_set(&ceesafe_poll_word, 0xFFFFFFFFu); }
'''

NOPOLLS = r'''fn main() {
    Console.writeLine(if (Async.runBound(250, RunBound.Count)) "bounded" else "no polls");
}
'''

DEADLINE = 240.0
FLAGS = ["-O2", "-ffp-contract=off", "-fno-math-errno"]


def strip_polls(text):
    """The C with every poll taken out: CEE_POLL lines gone, and of each `#if defined(CEE_POLLS)` block its `#else`
    part kept, or nothing."""
    out = []
    lines = text.split("\n")
    k = 0
    while k < len(lines):
        line = lines[k]
        if line.strip() == "#if defined(CEE_POLLS)":
            k += 1
            keep = False
            while lines[k].strip() != "#endif":
                if lines[k].strip() == "#else":
                    keep = True
                elif keep:
                    out.append(lines[k])
                k += 1
            k += 1
            continue
        if "CEE_POLL(" not in line:
            out.append(line)
        k += 1
    return "\n".join(out)


def assembly(cc, directory, source):
    if os.path.basename(cc).lower() in ("cl", "cl.exe"):
        # MSVC: the flags ceesafec gives cl (src/host/toolchain.cee), and a listing in place of -S.
        stem = os.path.splitext(source)[0]
        r = subprocess.run([cc, "/nologo", "/std:c11", "/bigobj", "/O2", "/I", directory, "/c", "/FA",
                            "/Fa" + stem + ".asm", "/Fo" + stem + ".obj", source], capture_output=True, text=True)
        if r.returncode != 0:
            return None, (r.stdout + r.stderr).strip()[:500]
        with open(stem + ".asm", encoding="utf-8", errors="replace") as f:
            text = f.read()
        # Comments and the TITLE line name the file; nothing else differs between two units of the same code.
        return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith((";", "TITLE"))), ""
    r = subprocess.run([cc, *FLAGS, "-I", directory, "-S", "-o", "-", source], capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip()[:500]
    # The file's own name is all that differs between two units of the same code.
    return "\n".join(l for l in r.stdout.splitlines() if not l.lstrip().startswith((".file", ".ident"))), ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    windows = os.name == "nt"
    problems = []
    cases = 0

    def expect(what, got, wanted, detail=""):
        nonlocal cases
        cases += 1
        if got != wanted:
            problems.append(f"{what}: {got!r}, expected {wanted!r} {detail}")

    with tempfile.TemporaryDirectory(prefix="ceesafe-bound-") as work:
        built = {}
        stale = os.path.join(work, "stale.c")
        with open(stale, "w", encoding="utf-8") as f:
            f.write(STALE_C)
        for stem, source, flags in (("survive", SURVIVE, ["--cflag", "-DCEE_POLLS=1"]),
                                    ("ends", ENDS, ["--cflag", "-DCEE_POLLS=1", stale]), ("nopolls", NOPOLLS, [])):
            path = os.path.join(work, stem + ".cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source)
            exe = os.path.join(work, stem + (".exe" if windows else ""))
            b = subprocess.run([compiler, "build", path, "-o", exe] + flags, capture_output=True, text=True)
            if b.returncode == 0:
                built[stem] = exe
            else:
                problems.append(f"{stem}: build failed: {b.stderr.strip()[:1500]}")

        def run(key, *words):
            return subprocess.run([built[key], *words], capture_output=True, text=True, timeout=DEADLINE)

        # 1. Every way to spin, ended alone; natively and in the interpreter.
        if "survive" in built:
            r = run("survive")
            expect("survive", r.stdout.splitlines(), SURVIVE_LINES, f"(status {r.returncode}, {r.stderr[-400:]!r})")
            ended = [line for line in r.stderr.splitlines() if "past its bound of" in line and "ended alone)" in line]
            expect("survive: overrun panic lines naming their task", len(ended), 11)
        r = subprocess.run([compiler, "run", "--interp", os.path.join(work, "survive.cee")], capture_output=True,
                           text=True, timeout=DEADLINE)
        expect("survive, interpreted", r.stdout.splitlines(), SURVIVE_LINES, f"({r.stderr[-400:]!r})")

        # 2. What needs the clock.
        if "ends" in built:
            def case(how):
                r = run("ends", how)
                words = r.stdout.splitlines()[-1].split() if r.stdout.strip() else []
                return r, (int(words[1]), int(words[3]), int(words[4]), int(words[6])) if len(words) == 7 else None

            r, v = case("count")
            expect("count: a task in no unit computes on, counted once", v, (0, 1, 0, 1), f"({r.stderr[-300:]!r})")
            cases += 1
            if "ceesafe overrun: a task ran" not in r.stderr or "RunBound.Count" not in r.stderr:
                problems.append(f"count: no overrun line: {r.stderr[-300:]!r}")
            r = run("ends", "panic")
            cases += 1
            if r.returncode == 0 or "ceesafe panic: a task ran" not in r.stderr or "past its bound of 30 ms" not in r.stderr:
                problems.append(f"panic: the process went on: status {r.returncode}, {r.stderr[-300:]!r}")
            r, v = case("critical")
            cases += 1
            if v is None or v[0] != 1001 or v[2] == 0 or v[2] - v[1] < 100 or v[3] != 1:
                problems.append(f"critical: {v!r}: not ended once, after its 100 ms Critical ({r.stderr[-300:]!r})")
            r, v = case("lenient")
            expect("lenient: a unit's own 5 s bound lets it compute 100 ms", v, (0, 1, 0, 0), f"({r.stderr[-300:]!r})")
            r, v = case("callback")
            cases += 1
            if v is None or v[0] != 1001 or v[2] == 0 or v[2] - v[1] < 200 or v[3] != 1:
                problems.append(f"callback: {v!r}: not ended once, after qsort's calls ({r.stderr[-300:]!r})")
            r, v = case("early")
            expect("early: 20 ms at a time under a 50 ms bound is never ended", v, (0, 20, 0, 0),
                   f"({r.stderr[-300:]!r})")
            r, v = case("stale")
            expect("stale: a word set for another resume ends nothing", v, (0, 1, 0, 0), f"({r.stderr[-300:]!r})")
            r, v = case("registry")
            expect("registry: 200 runs on four threads", r.stdout.splitlines()[:1], ["runs 200"],
                   f"({r.stderr[-300:]!r})")
            cases += 1
            if v is None or v[0] != 1001 or v[3] != 1:
                problems.append(f"registry: the spin after them was not ended once: {v!r}")
            r, v = case("latency")
            cases += 1
            if v is None or v[0] != 1001 or not (50 <= v[2] <= 150):
                problems.append(f"latency: {v!r}: the ticker's longest wait is not between the bound and 3 times it")

        # 3. A build without polls.
        if "nopolls" in built:
            expect("nopolls", run("nopolls").stdout.strip(), "no polls")

        # 4. Without CEE_POLLS, the code is the code without polls.
        programs = sorted(glob.glob(os.path.join(ROOT, "tests", "reference", "benchmarks", "suite", "*", "main.cee")))
        programs.append(os.path.join(ROOT, "tests", "reference", "tests", "corpus", "emit-c", "adr0186-polls.cee"))
        for path in programs:
            name = os.path.basename(os.path.dirname(path)) if path.endswith("main.cee") else os.path.basename(path)
            out = os.path.join(work, "same-" + name)
            e = subprocess.run([compiler, "build", "--emit", "c", path, "-o", out], capture_output=True, text=True)
            cases += 1
            if e.returncode != 0:
                problems.append(f"same code, {name}: --emit c failed: {e.stderr.strip()[:300]}")
                continue
            main_c = os.path.join(out, "main.c")
            with open(main_c, encoding="utf-8") as f:
                text = f.read()
            stripped = os.path.join(out, "stripped.c")
            with open(stripped, "w", encoding="utf-8") as f:
                f.write(strip_polls(text))
            a, why = assembly(args.cc, out, main_c)
            b, why2 = assembly(args.cc, out, stripped)
            if a is None or b is None:
                problems.append(f"same code, {name}: {why or why2}")
            elif a != b:
                problems.append(f"same code, {name}: its assembly differs from the C's without polls")
            elif "CEE_POLL(" not in text and "cee_counted_" not in text and name != "const-flags":
                problems.append(f"same code, {name}: no poll was written")

    for problem in problems:
        print(f"FAIL {problem}")
    print(f"bound_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
