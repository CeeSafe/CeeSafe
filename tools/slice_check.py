#!/usr/bin/env python3
"""Slices (ADR 0189): an isolated unit past its slice is paused at a poll and goes on later, on a stack of its own.
Checks the corpus cannot make, since they need the clock, the threads' interleaving or a build with polls.

1. `slices`, built with --cflag -DCEE_POLLS=1, one case an argument:
   - `share`: two isolated units compute 300 ms each under a 10 ms slice; a task in no unit that waits on a timer of
     1 ms waits far less than 300 ms behind them, both finish, and pauses are counted;
   - `result`: two units under a 1 ms slice compute numbers in a loop that keeps twelve integers and six floats live
     across its polls, and a recursion 27 deep in Fibonacci's tree; paused hundreds of times, they print what the same
     build prints without a slice (`result none`) and what the interpreter prints;
   - `bound`: a 10 ms slice and a 100 ms bound: a unit that never ends is paused, then ended with PanicKind.Overran,
     while its neighbour's waits stay short;
   - `mutex`, `critical`, `callback`, `endonly`: a unit that computes 100 ms holding a lib/std Mutex (taken with
     tryLock: `lock` may block, N0295), inside a Critical, under C's frames (a comparison qsort calls) or in a
     function that calls a C function itself is not paused there (its neighbour waits the 100 ms), and is paused once
     out of it;
   - `endbound`: under a 50 ms bound, a unit that never leaves a function that calls C is ended, not paused;
   - `trap`: a unit paused several times then traps: it ends alone, its neighbour finishes, the process goes on;
   - `dies`: a unit's task is paused when another task of the unit traps: it never runs again;
   - `cancel`: a paused unit whose task set is cancelled by a timeout goes on to its next wait and ends;
   - `handoff` (ADR 0191 §2.2): beside a unit that computes 300 ms under a 10 ms slice, two tasks pass a number to
     and fro 20 times, each pass a wake: they finish in far less than the 300 ms, where a paused unit that went back
     in the one queue made each pass wait a slice;
   - `busy` (ADR 0191 §2.1): two tasks pass a number to and fro for 300 ms, which keeps the executor's queue full,
     beside a task that waits on 1 ms timers: its longest wait stays far below the 300 ms, with or without a slice;
   - `measured` (ADR 0193): under a 1 ms slice and a 200 ms bound, a unit alone that never ends is ended after about
     200 ms, the time it ran measured at each pause, where counting its pauses times the slice let it run twice that;
   - `readiness` (ADR 0203): beside a unit computing 500 ms under a 20 ms slice, a task reading what another thread
     writes over TCP every 13 ms gets each write within a few ms (mean <= 3, longest <= 8), where the slice ended by
     the clock alone made it wait for the slice's rest (about 10 ms on average); without a slice it waits for the unit;
   - `workers`: under Async.runWorkers(4), units started by tasks on every worker compute and are paused, each on its
     own worker's stacks, and finish with the right numbers (each unit computes for well over a slice, more than
     Windows' clock tick of about 15.6 ms, or it may end before a slice runs out);
   - `none` cases: with no slice, nothing is paused.
   Each normal exit is run with the allocation report too (CEESAFE_ALLOC_REPORT=1): nothing live but what a crashed
   unit left.
2. `nopolls`: built without CEE_POLLS, Async.runSlice answers false and the program's results are the same.
3. The interpreter: Async.runSlice is true, nothing is paused, the numbers are the same.

usage: python3 tools/slice_check.py --compiler out/ceesafec [--cc cc] [--cflag F ...]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

PROGRAM = r'''enum Fail {
    NoMemory,
    Crashed,
    Other,
}

@header("stdlib.h")
extern "C" {
    @effects()
    fn qsort(base: *mut void, count: usize, size: usize, compare: extern "C" fn(*void, *void) -> i32);
    @effects()
    fn abs(x: i32) -> i32;
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

/// How many crashes the watcher was told of, an overrun counting 1001.
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

/// Computes without waiting for `ms` milliseconds by the clock.
fn busy(ms: u64) -> u64 {
    let start = Timer.now();
    var x: u64 = 0;
    while (Timer.now() - start < ms) {
        x = (x ^ (x >> 1)) | 1;
    }
    return x;
}

/// The same in a function that calls a C function itself: its polls may end its task, never pause it.
/// Safety: abs reads its argument only.
fn busyInC(ms: u64) -> u64 {
    let start = Timer.now();
    var x: i32 = 0;
    while (Timer.now() - start < ms) {
        x = unsafe { abs(x - 3) } % 1024;
    }
    return truncate<u64>(x);
}

/// Never ends, in a function that calls C.
/// Safety: abs reads its argument only.
fn foreverInC() -> i32 {
    var x: i32 = 1;
    while (x != 0) {
        x = unsafe { abs(x) } % 1024 + 1;
    }
    return x;
}

/// Never ends.
fn churn() -> u64 {
    var x: u64 = 1;
    while (x != 0) {
        x = (x ^ (x >> 1)) | 1;
    }
    return x;
}

/// C's qsort calls this: it computes for 100 ms, with C's frames between the executor and its polls.
/// Safety: qsort gives two elements of the array.
@callback
unsafe fn slowly(a: *void, b: *void) -> i32 {
    let first = (a as *u32).read();
    let second = (b as *u32).read();
    let _ = busy(50);
    return if (first < second) -1 else if (first > second) 1 else 0;
}

/// Safety: the array has three elements of four bytes, and `slowly` reads elements only.
fn sorted() -> u32 {
    var numbers = [3u32, 1, 2];
    unsafe { qsort(borrow mut numbers as *mut [u32; 3] as *mut void, 3, 4, slowly); }
    return numbers[0];
}

/// Twelve integers and six floats live across every poll of a counted loop, so a switch that lost a callee-saved
/// register would show in the result.
fn mix(n: u64) -> u64 {
    var a: u64 = 1;
    var b: u64 = 2;
    var c: u64 = 3;
    var d: u64 = 5;
    var e: u64 = 7;
    var f: u64 = 11;
    var g: u64 = 13;
    var h: u64 = 17;
    var i: u64 = 19;
    var j: u64 = 23;
    var k: u64 = 29;
    var l: u64 = 31;
    var p: f64 = 0.5;
    var q: f64 = 1.5;
    var r: f64 = 2.5;
    var s: f64 = 3.5;
    var t: f64 = 4.5;
    var u: f64 = 5.5;
    for (m in 0..n) {
        a = (a ^ (b >> 3)) ^ (m << 1);
        b = (b ^ (c << 5)) ^ (a >> 7);
        c = (c ^ (d >> 11)) ^ m;
        d = (d ^ (e << 13)) ^ (c >> 2);
        e = (e ^ (f >> 17)) ^ (d << 3);
        f = (f ^ (g << 19)) ^ (e >> 5);
        g = (g ^ (h >> 23)) ^ (f << 7);
        h = (h ^ (i << 29)) ^ (g >> 11);
        i = (i ^ (j >> 31)) ^ (h << 13);
        j = (j ^ (k << 37)) ^ (i >> 17);
        k = (k ^ (l >> 41)) ^ (j << 19);
        l = (l ^ (a << 43)) ^ (k >> 23);
        p = p * 0.999 + 0.001 * q;
        q = q * 0.998 + 0.002 * r;
        r = r * 0.997 + 0.003 * s;
        s = s * 0.996 + 0.004 * t;
        t = t * 0.995 + 0.005 * u;
        u = u * 0.994 + 0.006 * p;
    }
    let floats = saturatingTruncate<u64>((p + q + r + s + t + u) * 1000000.0);
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j ^ k ^ l ^ floats;
}

fn fib(n: u64) -> u64 => if (n < 2) n else fib(n - 1) + fib(n - 2);

async fn computes(ms: u64, done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    let x = busy(ms);
    done.get().store(1 + (x & 0));
    return Ok();
}

async fn mixes(n: u64, done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    done.get().store(mix(n));
    return Ok();
}

async fn fibs(n: u64, done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    done.get().store(fib(n));
    return Ok();
}

async fn forever(done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    done.get().store(churn());
    return Ok();
}

async fn foreverC(done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    done.get().store(truncate<u64>(foreverInC()));
    return Ok();
}

/// The pauses while it computed 100 ms in the way `how` names, plus one; 1 + 1000 * the pauses after it, in `after`.
async fn heldThenFree(how: u32, lock: Arc<Mutex<u64>>, inside: Arc<AtomicU64>, after: Arc<AtomicU64>)
    -> Result<void, Fail> {
    await nap(1)?;
    let before = Async.pauses();
    if (how == 0) {
        let guard = lock.get().tryLock() ?? { return Err(Fail.Other); };
        let _ = busy(100);
        let _ = guard;
    } else if (how == 1) {
        let critical = Critical.enter();
        let _ = busy(100);
        let _ = critical;
    } else if (how == 2) {
        let _ = sorted();
        let _ = sorted();
    } else {
        let _ = busyInC(100);
    }
    let held = Async.pauses() - before;
    inside.get().store(1 + held);
    let free = Async.pauses();
    let _ = busy(80);
    after.get().store(1 + 1000 * (Async.pauses() - free));
    return Ok();
}

async fn trapsLate(done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(1)?;
    let x = busy(60);
    let numbers = [1u64, 2, 3];
    let at = (x & 0) as usize + 3 + truncate<usize>(done.get().load());
    done.get().store(numbers[at]);
    return Ok();
}

/// A unit whose first task computes 300 ms while the second traps 30 ms in.
async fn diesPaused(computed: Arc<AtomicU64>) -> Result<void, Fail> {
    let numbers = [1u64, 2, 3];
    let r = await Async.all(
        async () => {
            await Timer.sleep(1)?;
            let x = busy(300);
            computed.get().store(1 + (x & 0));
            return Ok();
        },
        async () => {
            await Timer.sleep(30)?;
            let at = 3 + truncate<usize>(computed.get().load());
            computed.get().store(numbers[at]);
            return Ok();
        },
    );
    return match (r) {
        Ok(_) => Ok(),
        Err(_) => Err(Fail.Other),
    };
}

/// The longest gap between `count` waits of a millisecond.
async fn gaps(count: u32, longest: Arc<AtomicU64>) -> Result<void, Fail> {
    var k: u32 = 0;
    var last = Timer.now();
    var most: u64 = 0;
    while (k < count) {
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

fn numbers() -> Result<Channel<u64>, Fail> {
    match (Channel<u64>.create(1)) {
        Ok(c) => { return Ok(c); }
        Err(_) => { return Err(Fail.NoMemory); }
    }
}

/// Sends back each number it is given, until it is given 0.
async fn pong(from: Channel<u64>, to: Channel<u64>) -> Result<void, Fail> {
    loop {
        let n = match (await from.recvAsync()) {
            Ok(v) => v ?? 0,
            Err(_) => { return Err(Fail.Other); }
        };
        match (await to.sendAsync(n)) {
            Ok() => {}
            Err(_) => { return Err(Fail.Other); }
        }
        if (n == 0) {
            return Ok();
        }
    }
}

/// Passes a number to `pong` and back: `rounds` times (`took` gets the ms they took), or with `rounds` 0 for `ms`
/// milliseconds. Each pass is a wake of the other task, so one of the two is always ready.
async fn ping(rounds: u64, ms: u64, to: Channel<u64>, from: Channel<u64>, took: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(5)?;
    let start = Timer.now();
    var k: u64 = 0;
    while (if (rounds > 0) k < rounds else Timer.now() - start < ms) {
        k += 1;
        match (await to.sendAsync(k)) {
            Ok() => {}
            Err(_) => { return Err(Fail.Other); }
        }
        match (await from.recvAsync()) {
            Ok(_) => {}
            Err(_) => { return Err(Fail.Other); }
        }
    }
    took.get().store(Timer.now() - start);
    match (await to.sendAsync(0)) {
        Ok() => {}
        Err(_) => { return Err(Fail.Other); }
    }
    let _ = await from.recvAsync();
    return Ok();
}

/// A worker's share: a task set with two units of its own.
async fn onWorker(n: u64, x: Arc<AtomicU64>, y: Arc<AtomicU64>) -> Result<void, Fail> {
    let r = await Async.tasks(4, Fail.NoMemory, async (set) => {
        let a = x.clone();
        let b = y.clone();
        await set.spawnIsolated(Fail.Crashed, async move () => await mixes(n, a))?;
        await set.spawnIsolated(Fail.Crashed, async move () => await fibs(25, b))?;
        return Ok();
    });
    return r;
}

/// One case: [crashes seen, the cells].
async fn one(
    how: u32,
    small: bool,
    a0: Arc<AtomicU64>,
    b0: Arc<AtomicU64>,
    c0: Arc<AtomicU64>,
    d0: Arc<AtomicU64>,
    crashes: Channel<TaskCrash>,
) -> Result<void, Fail> {
    let lock = match (Mutex<u64>.create(0)) {
        Ok(m) => m,
        Err(_) => { return Err(Fail.NoMemory); }
    };
    let shared = match (Arc<Mutex<u64>>.create(lock)) {
        Ok(a) => a,
        Err(_) => { return Err(Fail.NoMemory); }
    };
    let r = await Async.tasks(8, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, crashes)) {
            return Err(Fail.NoMemory);
        }
        let a = a0.clone();
        let b = b0.clone();
        let c = c0.clone();
        let d = d0.clone();
        if (how == 0) {
            await set.spawnIsolated(Fail.Crashed, async move () => await computes(300, a))?;
            await set.spawnIsolated(Fail.Crashed, async move () => await computes(300, b))?;
            await set.spawn(async move () => await gaps(100, c))?;
        } else if (how == 1) {
            let big: u64 = if (small) 20000 else 30000000;
            let deep: u64 = if (small) 15 else 27;
            await set.spawnIsolated(Fail.Crashed, async move () => await mixes(big, a))?;
            await set.spawnIsolated(Fail.Crashed, async move () => await fibs(deep, b))?;
            await set.spawnIsolated(Fail.Crashed, async move () => await mixes(big * 2 / 3, c))?;
        } else if (how == 2) {
            await set.spawnIsolated(Fail.Crashed, async move () => await forever(a))?;
            await set.spawn(async move () => await gaps(60, c))?;
        } else if (how >= 3 && how <= 6) {
            let m = shared.clone();
            await set.spawnIsolated(Fail.Crashed, async move () => await heldThenFree(how - 3, m, a, b))?;
            await set.spawn(async move () => await gaps(100, c))?;
        } else if (how == 14) {
            await set.spawnIsolated(Fail.Crashed, async move () => await forever(a))?;
        } else if (how == 7) {
            await set.spawnIsolated(Fail.Crashed, async move () => await foreverC(a))?;
            await set.spawn(async move () => await gaps(60, c))?;
        } else if (how == 8) {
            await set.spawnIsolated(Fail.Crashed, async move () => await trapsLate(a))?;
            await set.spawn(async move () => await gaps(100, c))?;
        } else if (how == 9) {
            await set.spawnIsolated(Fail.Crashed, async move () => await diesPaused(a))?;
            await set.spawn(async move () => await gaps(60, c))?;
        } else if (how == 12 || how == 13) {
            // handoff: twenty round trips beside a unit that computes 300 ms; busy: round trips for 300 ms, which
            // keep the queue full, beside a task that waits on 1 ms timers.
            let there = numbers()?;
            let back = numbers()?;
            let p1 = there.clone();
            let p2 = back.clone();
            await set.spawn(async move () => await pong(p1, p2))?;
            if (how == 12) {
                await set.spawnIsolated(Fail.Crashed, async move () => await computes(300, a))?;
                await set.spawn(async move () => await ping(20, 0, there, back, c))?;
            } else {
                await set.spawn(async move () => await ping(0, 300, there, back, b))?;
                await set.spawn(async move () => await gaps(100, c))?;
            }
        } else if (how == 10) {
            let timed = await Async.timeout(30, Fail.Other, async () => await Async.tasks(
                4,
                Fail.NoMemory,
                async (s2) => {
                    let e = a.clone();
                    await s2.spawnIsolated(Fail.Crashed, async move () => await computes(150, e))?;
                    return Ok();
                },
            ));
            d.get().store(match (timed) {
                Ok() => 1,
                Err(Fail.Other) => 2,
                Err(_) => 3,
            });
        }
        return Ok();
    });
    let _ = r;
    return Ok();
}

/// On a thread of its own: connects to `port` and writes its clock (8 bytes) `count` times, `gap` ms apart.
async fn sender(port: u16, count: u32, gap: u64) -> Result<void, Fail> {
    let stream = match (await AsyncTcpStream.connect("127.0.0.1", port)) {
        Ok(s) => s,
        Err(_) => { return Err(Fail.Other); },
    };
    var k: u32 = 0;
    while (k < count) {
        await nap(gap)?;
        var clock = [0u8; 8];
        let now = Timer.now();
        for (i in 0..8) {
            clock[i] = truncate<u8>(now >> (i as u64 * 8));
        }
        match (await stream.write(clock[..])) {
            Ok() => {}
            Err(_) => { return Err(Fail.Other); }
        }
        k += 1;
    }
    return Ok();
}

/// Reads the sender's clocks: the longest wait from a write to its read in `longest`, their mean in `mean`.
async fn receiver(stream: AsyncTcpStream, count: u32, longest: Arc<AtomicU64>, mean: Arc<AtomicU64>)
    -> Result<void, Fail> {
    var clock = [0u8; 8];
    var have: usize = 0;
    var most: u64 = 0;
    var sum: u64 = 0;
    var k: u32 = 0;
    while (k < count) {
        let n = match (await stream.read(mut clock[have..])) {
            Ok(n) => n,
            Err(_) => { return Err(Fail.Other); },
        };
        if (n == 0) {
            return Err(Fail.Other);
        }
        have += n;
        if (have == 8) {
            var sent: u64 = 0;
            for (i in 0..8) {
                sent = sent | ((clock[i] as u64) << (i as u64 * 8));
            }
            let now = Timer.now();
            let waited = if (now > sent) now - sent else 0;
            if (waited > most) {
                most = waited;
            }
            sum += waited;
            have = 0;
            k += 1;
        }
    }
    longest.get().store(most);
    mean.get().store(sum / (count as u64));
    return Ok();
}

/// ADR 0203: a unit computes 500 ms under the slice beside a task that reads what another thread writes.
async fn readiness(listener: AsyncTcpListener, a: Arc<AtomicU64>, c: Arc<AtomicU64>, d: Arc<AtomicU64>, crashes: Channel<TaskCrash>)
    -> Result<void, Fail> {
    let r = await Async.tasks(4, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, crashes)) {
            return Err(Fail.NoMemory);
        }
        let stream = match (await listener.accept()) {
            Ok(s) => s,
            Err(_) => { return Err(Fail.Other); },
        };
        let done = a.clone();
        let longest = c.clone();
        let mean = d.clone();
        await set.spawnIsolated(Fail.Crashed, async move () => await computes(500, done))?;
        await set.spawn(async move () => await receiver(stream, 15, longest, mean))?;
        return Ok();
    });
    return r;
}

/// Under Async.runWorkers(4): four tasks any worker may take, each with a task set of two units.
async fn onWorkers(x: Arc<AtomicU64>, y: Arc<AtomicU64>) -> Result<void, Fail> {
    let r = await Async.scope(Fail.NoMemory, (s) => {
        for (k in 0..4u64) {
            let _ = k;
            let p = x.clone();
            let q = y.clone();
            s.spawnSend(async move () => await onWorker(50000000, p, q));
        }
    });
    return r;
}

fn report(text: StringView, value: u64) {
    Console.write(text);
    Console.write(" ");
    Console.writeLine(value);
}

fn main() {
    let how = Process.argument(1) ?? "";
    let second = Process.argument(2) ?? "";
    let none = second == "none";
    let small = second == "small";
    let which: u32 = if (how == "share") 0 else if (how == "result") 1 else if (how == "bound") 2
        else if (how == "mutex") 3 else if (how == "critical") 4 else if (how == "callback") 5
        else if (how == "endonly") 6 else if (how == "endbound") 7 else if (how == "trap") 8
        else if (how == "dies") 9 else if (how == "cancel") 10 else if (how == "workers") 11
        else if (how == "handoff") 12 else if (how == "busy") 13 else if (how == "measured") 14
        else if (how == "readiness") 15 else 99;
    let slice: u64 = if (none) 0 else if (which == 1 || which == 14) 1 else if (which == 8 || which == 9) 5
        else if (which == 15) 20 else 10;
    let sliced = Async.runSlice(slice);
    Console.writeLine(if (sliced) "sliced" else "no slice");
    if (which == 2) {
        let _ = Async.runBound(100, RunBound.Count);
    } else if (which == 7) {
        let _ = Async.runBound(50, RunBound.Count);
    } else if (which == 14) {
        let _ = Async.runBound(200, RunBound.Count);
    }
    let a = cell()!;
    let b = cell()!;
    let c = cell()!;
    let d = cell()!;
    let crashes = crashChannel()!;
    let start = Timer.now();
    if (which == 11) {
        let w = Async.runWorkers(4, onWorkers(a.clone(), c.clone()));
        let _ = w;
    } else if (which == 15) {
        let listener = AsyncTcpListener.bind("127.0.0.1", 0)!;
        let port = listener.port()!;
        let writer = Thread.spawn(move () => Async.run(sender(port, 15, 13)))!;
        match (Async.run(readiness(listener, a.clone(), c.clone(), d.clone(), crashes.clone()))) {
            Ok() => {}
            Err(_) => { Console.writeLine("failed"); }
        }
        let sent = writer.join();
        let _ = sent;
    } else {
        match (Async.run(one(which, small, a.clone(), b.clone(), c.clone(), d.clone(), crashes.clone()))) {
            Ok() => {}
            Err(_) => { Console.writeLine("failed"); }
        }
    }
    let ended = Timer.now();
    report("crashes", overranOf(crashes));
    report("a", a.get().load());
    report("b", b.get().load());
    report("c", c.get().load());
    report("d", d.get().load());
    report("ms", ended - start);
    report("pauses", Async.pauses());
    report("overruns", Async.overruns());
}
'''

problems = []


def expect(what, ok, detail=""):
    if not ok:
        problems.append(what + (": " + detail if detail else ""))


def run(command, env=None, timeout=60):
    try:
        return subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def values(text):
    out = {}
    for line in text.splitlines():
        m = re.match(r"^(\w+) (\d+)$", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    out["sliced"] = text.startswith("sliced")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cc", default=None)
    parser.add_argument("--cflag", action="append", default=[])
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    env = dict(os.environ)
    if args.cc:
        env["CC"] = args.cc
    work = tempfile.mkdtemp(prefix="slice_check_")
    source = os.path.join(work, "slices.cee")
    with open(source, "w") as f:
        f.write(PROGRAM)
    suffix = ".exe" if os.name == "nt" else ""
    exe = os.path.join(work, "slices" + suffix)
    plain = os.path.join(work, "slices-nopolls" + suffix)
    flags = [x for c in args.cflag for x in ("--cflag", c)]
    for target, polls in ((exe, True), (plain, False)):
        command = [compiler, "build", source, "-o", target] + flags + (["--cflag", "-DCEE_POLLS=1"] if polls else [])
        if args.cc:
            command += ["--cc", args.cc]
        b = run(command, env=env, timeout=600)
        if b is None or b.returncode != 0:
            print(f"slice_check: the build failed: {(b.stdout + b.stderr)[:2000] if b else 'timed out'}")
            return 1

    def case(name, extra=(), report=True, binary=exe):
        r = run([binary, name, *extra], env=dict(env, CEESAFE_ALLOC_REPORT="1") if report else env, timeout=60)
        if r is None:
            problems.append(f"{name}: timed out")
            return {}
        v = values(r.stdout)
        v["stderr"] = r.stderr
        v["status"] = r.returncode
        if report:
            m = re.search(r"ceesafe alloc: attempts=\d+ live=(\d+)", r.stderr)
            k = re.search(r"ceesafe alloc: isolated leaked=(\d+)", r.stderr)
            if m is None:
                problems.append(f"{name}: no allocation report (exit {r.returncode}): {r.stderr[-400:]}")
            else:
                live = int(m.group(1)) - (int(k.group(1)) if k else 0)
                expect(f"{name}: nothing live but what a crashed unit left", live <= 0, r.stderr[-300:])
        expect(f"{name}: exits normally", r.returncode == 0, f"exit {r.returncode}: {r.stderr[-400:]}")
        return v

    # share: neighbours wait far less than 300 ms behind two units that compute.
    v = case("share")
    expect("share: sliced", v.get("sliced"))
    expect("share: both units finished", v.get("a") == 1 and v.get("b") == 1, str(v))
    expect("share: pauses counted", v.get("pauses", 0) >= 4, str(v))
    expect("share: the neighbour waits far less than the computation", 0 < v.get("c", 999) < 200, str(v))
    n = case("share", ["none"])
    expect("share none: nothing paused", n.get("pauses") == 0, str(n))
    expect("share none: the neighbour waits for the computation", n.get("c", 0) >= 250, str(n))

    # handoff: tasks that wake each other are not held a slice at each wake behind a paused unit (ADR 0191 §2.2).
    v = case("handoff")
    expect("handoff: the unit finished", v.get("a") == 1, str(v))
    expect("handoff: pauses counted", v.get("pauses", 0) >= 4, str(v))
    expect("handoff: twenty round trips beside it take far less than its 300 ms", v.get("c", 999) < 100, str(v))

    # busy: a queue kept full still lets due timers run (ADR 0191 §2.1).
    for extra in ([], ["none"]):
        v = case("busy", extra)
        label = "busy" + (" none" if extra else "")
        expect(f"{label}: the round trips ran for their 300 ms", v.get("b", 0) >= 300, str(v))
        expect(f"{label}: a 1 ms timer beside them waits far less than 300 ms", 0 < v.get("c", 999) < 50, str(v))

    # result: the same numbers paused hundreds of times, without a slice, and in the interpreter.
    v = case("result")
    n = case("result", ["none"])
    numbers = (n.get("a"), n.get("b"), n.get("c"))
    expect("result: paused often", v.get("pauses", 0) >= 50, str(v))
    expect("result: fib(27)", v.get("b") == 196418, str(v))
    expect("result: the same numbers as without a slice",
           (v.get("a"), v.get("b"), v.get("c")) == (n.get("a"), n.get("b"), n.get("c")) and v.get("a"), f"{v} {n}")

    # bound: paused, then ended past the bound.
    v = case("bound")
    expect("bound: ended with Overran", v.get("crashes") == 1001, str(v))
    expect("bound: paused before it was ended", v.get("pauses", 0) >= 3, str(v))
    expect("bound: its neighbour's waits stay short", 0 < v.get("c", 999) < 90, str(v))
    told = v.get("stderr", "")
    expect("bound: the panic line", "past its bound of 100 ms" in told and "ended alone" in told, told[:300])

    # measured (ADR 0193): alone under a 1 ms slice and a 200 ms bound, a unit that never ends is ended once it has run
    # 200 ms. The monitor sees each slice late, so counting the pauses times the slice let it run 400 ms and more.
    v = case("measured")
    expect("measured: ended with Overran", v.get("crashes") == 1001, str(v))
    expect("measured: paused before it was ended", v.get("pauses", 0) >= 20, str(v))
    expect("measured: ended near its bound of 200 ms, not past it by its slices' lateness",
           200 <= v.get("ms", 0) < 300, str(v))

    # readiness (ADR 0203): beside a unit computing 500 ms under a 20 ms slice, a task reading what another thread
    # writes every 13 ms gets each write within a few ms: the monitor saw the socket ready and ended the slice. With
    # the slice ended by the clock alone, a write waited for the rest of the slice: about 10 ms on average.
    for extra in ([], ["none"]):
        v = case("readiness", extra)
        label = "readiness" + (" none" if extra else "")
        expect(f"{label}: the unit finished", v.get("a") == 1, str(v))
        if extra:
            expect(f"{label}: without a slice the reader waits for the unit", v.get("c", 0) >= 300, str(v))
        else:
            expect(f"{label}: paused for what came", v.get("pauses", 0) >= 15, str(v))
            expect(f"{label}: each write read within a few ms, not the slice's rest",
                   v.get("d", 99) <= 3 and v.get("c", 99) <= 8, str(v))

    # Where a task is not paused: a Mutex, a Critical, C's frames, a function that calls C.
    for name in ("mutex", "critical", "callback", "endonly"):
        v = case(name)
        expect(f"{name}: not paused inside", v.get("a") == 1, str(v))
        expect(f"{name}: paused once out of it", v.get("b", 0) >= 1001, str(v))
        expect(f"{name}: its neighbour waited for it", v.get("c", 0) >= 90, str(v))
        expect(f"{name}: nothing crashed", v.get("crashes") == 0, str(v))

    # endbound: a function that calls C is ended at the bound, never paused.
    v = case("endbound")
    expect("endbound: ended with Overran", v.get("crashes") == 1001, str(v))
    expect("endbound: never paused", v.get("pauses") == 0, str(v))

    # trap: paused, then trapped: it ends alone.
    v = case("trap")
    expect("trap: one crash, not an overrun", v.get("crashes") == 1, str(v))
    expect("trap: paused first", v.get("pauses", 0) >= 2, str(v))
    expect("trap: its neighbour finished", v.get("c", 0) > 0, str(v))

    # dies: a paused task of a unit that died never runs again.
    v = case("dies")
    expect("dies: one crash", v.get("crashes") == 1, str(v))
    expect("dies: the paused task never ran again", v.get("a") == 0, str(v))
    expect("dies: its neighbour finished", v.get("c", 0) > 0, str(v))

    # cancel: a unit paused when its set is cancelled goes on to its next wait.
    v = case("cancel")
    expect("cancel: the timeout's result", v.get("d") in (1, 2), str(v))
    expect("cancel: nothing crashed", v.get("crashes") == 0, str(v))

    # workers: every worker's units paused and right.
    v = case("workers")
    n = case("workers", ["none"])
    expect("workers: paused", v.get("pauses", 0) >= 4, str(v))
    expect("workers: the same numbers as without a slice", (v.get("a"), v.get("c")) == (n.get("a"), n.get("c"))
           and v.get("c") == 75025, f"{v} {n}")

    # nopolls: no slice, the same numbers.
    v = case("result", binary=plain)
    expect("nopolls: Async.runSlice is false", v.get("sliced") is False and v.get("pauses") == 0, str(v))
    expect("nopolls: the same numbers", (v.get("a"), v.get("b"), v.get("c")) == numbers, f"{v} {numbers}")

    # The interpreter: a slice taken, nothing paused, the same numbers as natively (smaller ones: it is slower).
    s = case("result", ["small"])
    r = run([compiler, "run", "--interp", source, "--", "result", "small"], env=env, timeout=1800)
    if r is None:
        problems.append("interp: timed out")
    else:
        i = values(r.stdout)
        expect("interp: Async.runSlice is true", i.get("sliced") is True, r.stdout[:200])
        expect("interp: nothing paused", i.get("pauses") == 0, r.stdout[:300])
        expect("interp: the same numbers as natively", (i.get("a"), i.get("b"), i.get("c")) ==
               (s.get("a"), s.get("b"), s.get("c")) and s.get("b") == 610, f"{i} {s}")

    for p in problems:
        print("FAIL " + p)
    print(f"slice_check: 15 cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
