#!/usr/bin/env python3
"""Isolated tasks (ADR 0175): checks the corpus cannot make, since a trap leaks what its task held (the corpus counts
leaks) and several cases end the process.

Two programs are built and run:

1. `survive`: six isolated children of a task set that each trap another way, beside three that work (the set's
   result, its count, the watcher's TaskCrash and its kind); a unit with tasks under it of every kind (a scope's
   children parked on a timer and on a channel, a race, a nested task set with an ordinary and an isolated child),
   whose root traps, and again with one of those tasks trapping: none of them runs again, also when what it waits
   for comes; a Mutex and an AsyncMutex held at the trap, which the next takers find poisoned (and trap for,
   alone), and which `recover` and `heal` make whole; `trySpawnIsolated` (ADR 0175 amendment 1), which starts while
   there is room, is refused while the set is full, and whose child's trap is the set's crash. It runs natively, with
   the address and undefined-behaviour sanitizers, and in the interpreter, with the same fifteen lines each time;
2. `ends`, one case an argument:
   - `isolated`: the child traps and the program goes on, the panic's line naming the task;
   - `critical`, `callback`, `blocking`, `plain`: a trap in a `Critical` section, under C's frames (a comparison
     function `qsort` calls), on `Async.blocking`'s thread, and in a child started with `spawn`: each ends the
     process, with the panic's line and no task named;
   - `many`: ten thousand children that each hold 64 KiB and trap; each crash's `leakedBytes` and the process's
     total are what they held;
   - `sockets`: a connection handed to an isolated child whose unit traps while a task of it is parked reading;
     the peer's later write is read by nobody, the peer ends its side, and the owner's `closer` still ends the
     connection, which the peer sees (on macOS that took amendment 1's fix).
     Also with the sanitizers.

usage: python3 tools/isolated_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

SURVIVE = r'''// Isolated children of a task set that trap, and what is left standing (ADR 0175).
enum Fail {
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

fn counter() -> Result<Arc<AtomicU64>, Fail> {
    match (Arc<AtomicU64>.create(AtomicU64.create(0))) {
        Ok(a) => { return Ok(a); }
        Err(_) => { return Err(Fail.NoMemory); }
    }
}

/// Traps in the way `how` says, after a wait.
async fn traps(how: u32) -> Result<void, Fail> {
    await nap(2)?;
    var numbers = Vector<u64>.create();
    match (numbers.push(1)) {
        Ok() => {}
        Err(_) => { return Err(Fail.NoMemory); }
    }
    if (how == 0) {
        let n = numbers[numbers.length + how as usize]; // index
    } else if (how == 1) {
        var big: u8 = 250;
        big += truncate<u8>(numbers.length as u64 + 9); // overflow
    } else if (how == 2) {
        let wrong: Result<u32, Fail> = Err(Fail.Other);
        let n = wrong!; // unwrap
    } else if (how == 3) {
        let nothing: u32? = null;
        let n = nothing!; // unwrap null
    } else if (how == 4) {
        panic("an explicit panic");
    } else {
        let zero = numbers.length as u64 - 1;
        let n = 10 / zero; // division by zero
    }
    return Ok();
}

async fn works(done: Arc<AtomicU64>) -> Result<void, Fail> {
    await nap(10)?;
    done.get().fetchAdd(1);
    return Ok();
}

struct Kinds {
    result: u32; // 0 ok, 1 crashed, 2 other
    crashes: u64;
    worked: u64;
    kinds: u32;  // one bit for each kind seen by the watcher
    watched: u64;
    numbered: bool;
}

/// Six children that each trap another way, beside three that work.
async fn kinds() -> Result<Kinds, Fail> {
    let worked = counter()?;
    let seen = worked.clone();
    let crashes = match (Channel<TaskCrash>.create(16)) {
        Ok(c) => c,
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let counted = AtomicU64.create(0);
    let ids = AtomicU64.create(0);
    let r = await Async.tasks(16, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, crashes)) {
            return Err(Fail.NoMemory);
        }
        var how: u32 = 0;
        while (how < 6) {
            let w = how;
            let id = await set.spawnIsolated(Fail.Crashed, async move () => await traps(w))?;
            if (id != 0) {
                ids.fetchAdd(1);
            }
            if (how % 2 == 0) {
                let mine = worked.clone();
                await set.spawn(async move () => await works(mine))?;
            }
            how += 1;
        }
        await nap(40)?;
        counted.store(set.crashes());
        return Ok();
    });
    var kindsSeen: u32 = 0;
    var watched: u64 = 0;
    loop {
        match (crashes.tryRecv()) {
            Some(crash) => {
                watched += 1;
                let bit: u32 = match (crash.kind()) {
                    PanicKind.Index => 1,
                    PanicKind.Overflow => 2,
                    PanicKind.Unwrap => 4,
                    PanicKind.UnwrapNull => 8,
                    PanicKind.Explicit => 16,
                    PanicKind.DivideByZero => 32,
                    _ => 64,
                };
                kindsSeen = kindsSeen | bit;
            }
            null => { break; }
        }
    }
    return Ok(Kinds {
        result: match (r) {
            Ok() => 0,
            Err(Fail.Crashed) => 1,
            Err(_) => 2,
        },
        crashes: counted.load(),
        worked: seen.get().load(),
        kinds: kindsSeen,
        watched: watched,
        numbered: ids.load() == 6,
    });
}

/// Parks on a timer, then says that it ran: it must not, once its unit has trapped.
async fn sleeper(ran: borrow Arc<AtomicU64>, ms: u64) -> Result<void, Fail> {
    await nap(ms)?;
    ran.get().fetchAdd(1);
    return Ok();
}

///  with a value, for a race.
async fn slow(ran: borrow Arc<AtomicU64>, ms: u64) -> Result<u32, Fail> {
    await nap(ms)?;
    ran.get().fetchAdd(1);
    return Ok(1);
}

/// Parks on a channel nobody fills until the test's end.
async fn listener(ran: borrow Arc<AtomicU64>, gate: borrow Channel<u8>) -> Result<void, Fail> {
    match (await gate.recvAsync()) {
        Ok(_) => {}
        Err(_) => { return Err(Fail.Other); }
    }
    ran.get().fetchAdd(1);
    return Ok();
}

/// A unit with tasks under it of every kind, each parked; then its root traps, or (deep) one of those tasks does.
async fn family(ran: borrow Arc<AtomicU64>, gate: borrow Channel<u8>, deep: bool) -> Result<void, Fail> {
    await Async.scope(Fail.NoMemory, (s) => {
        // A scope's children, each parked: on a timer, on a channel, in a race, in a task set of its own.
        s.spawn(async () => await sleeper(ran, 30));
        s.spawn(async () => await listener(ran, gate));
        s.spawn(async () => {
            await nap(5)?;
            if (deep) {
                await traps(0)?; // a task under the root traps
            }
            return Ok();
        });
        s.spawn(async () => {
            let first = await Async.race(async () => await slow(ran, 30), async () => await slow(ran, 35))?;
            return Ok();
        });
        s.spawn(async () => {
            // A nested task set with an ordinary and an isolated child, and (shallow) the trap.
            return await Async.tasks(4, Fail.NoMemory, async (inner) => {
                let e = ran.clone();
                let f = ran.clone();
                await inner.spawn(async move () => await sleeper(e, 30))?;
                let id = await inner.spawnIsolated(Fail.Crashed, async move () => await sleeper(f, 30))?;
                await nap(10)?;
                if (!deep) {
                    await traps(2)?;
                }
                return Ok();
            });
        });
    })?;
    ran.get().fetchAdd(100); // after the trap: never
    return Ok();
}

struct Family {
    result: u32;
    ran: u64;
    crashes: u64;
}

async fn families(deep: bool) -> Result<Family, Fail> {
    let ran = counter()?;
    let seen = ran.clone();
    let gate = match (Channel<u8>.create(4)) {
        Ok(c) => c,
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let opener = gate.clone();
    let counted = AtomicU64.create(0);
    let r = await Async.tasks(4, Fail.NoMemory, async (set) => {
        let mine = ran.clone();
        let theirs = gate.clone();
        let id = await set.spawnIsolated(Fail.Crashed, async move () => await family(mine, theirs, deep))?;
        await nap(80)?; // past every timer of the unit
        // What would wake the unit's listener: it is given a value, and still does not run.
        match (opener.trySend(1)) {
            Ok() => {}
            Err(_) => {}
        }
        await nap(20)?;
        counted.store(set.crashes());
        return Ok();
    });
    return Ok(Family {
        result: match (r) {
            Ok() => 0,
            Err(Fail.Crashed) => 1,
            Err(_) => 2,
        },
        ran: seen.get().load(),
        crashes: counted.load(),
    });
}

struct Count {
    n: u64;
}

/// Takes the lock and traps with it held.
async fn holds(shared: borrow Arc<Mutex<Count>>, slow: borrow Arc<AsyncMutex<Count>>) -> Result<void, Fail> {
    var guard = shared.get().tryLock() ?? { return Err(Fail.Other); };
    let data = guard.data();
    data.n += 1; // half of a change
    var other = match (await slow.get().lock()) {
        Ok(g) => g,
        Err(_) => { return Err(Fail.Other); },
    };
    let more = other.data();
    more.n += 1;
    await nap(2)?;
    return await traps(0);
}

/// Takes a lock that may be poisoned: it then traps (PanicKind.Poisoned).
async fn takes(shared: borrow Arc<Mutex<Count>>) -> Result<void, Fail> {
    await nap(20)?;
    var guard = shared.get().tryLock() ?? { return Err(Fail.Other); };
    let data = guard.data();
    data.n += 100;
    return Ok();
}

async fn takesSlow(slow: borrow Arc<AsyncMutex<Count>>) -> Result<void, Fail> {
    await nap(20)?;
    var guard = match (await slow.get().lock()) {
        Ok(g) => g,
        Err(_) => { return Err(Fail.Other); },
    };
    let data = guard.data();
    data.n += 100;
    return Ok();
}

struct Locks {
    crashes: u64;
    poisonedKinds: u64; // crashes of kind Poisoned
    recovered: bool;
    healed: u64;        // the value once it is whole again and taken with lock()
    slowHealed: u64;
}

async fn locks() -> Result<Locks, Fail> {
    let shared = match (Mutex<Count>.create(Count { n: 0 })) {
        Ok(m) => match (Arc<Mutex<Count>>.create(m)) {
            Ok(a) => a,
            Err(_) => { return Err(Fail.NoMemory); },
        },
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let slow = match (AsyncMutex<Count>.create(Count { n: 0 })) {
        Ok(m) => match (Arc<AsyncMutex<Count>>.create(m)) {
            Ok(a) => a,
            Err(_) => { return Err(Fail.NoMemory); },
        },
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let crashes = match (Channel<TaskCrash>.create(8)) {
        Ok(c) => c,
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let counted = AtomicU64.create(0);
    let r = await Async.tasks(8, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, crashes)) {
            return Err(Fail.NoMemory);
        }
        let one = shared.clone();
        let two = shared.clone();
        let three = slow.clone();
        let four = slow.clone();
        let a = await set.spawnIsolated(Fail.Crashed, async move () => await holds(one, three))?;
        // Each finds its lock poisoned, and traps for it, alone.
        let b = await set.spawnIsolated(Fail.Crashed, async move () => await takes(two))?;
        let c = await set.spawnIsolated(Fail.Crashed, async move () => await takesSlow(four))?;
        await nap(60)?;
        counted.store(set.crashes());
        return Ok();
    });
    var poisoned: u64 = 0;
    loop {
        match (crashes.tryRecv()) {
            Some(crash) => {
                if (crash.kind() is PanicKind.Poisoned) {
                    poisoned += 1;
                }
            }
            null => { break; }
        }
    }
    // The owner repairs the data and heals the locks.
    var recovered = false;
    {
        var guard = shared.get().tryRecover() ?? { return Err(Fail.Other); };
        recovered = guard.poisoned() && guard.data().n == 1;
        let data = guard.data();
        data.n = 7;
        guard.heal();
    }
    var healed: u64 = 0;
    {
        var guard = shared.get().tryLock() ?? { return Err(Fail.Other); };
        healed = guard.data().n;
    }
    var slowHealed: u64 = 0;
    {
        var guard = match (await slow.get().recover()) {
            Ok(g) => g,
            Err(_) => { return Err(Fail.Other); },
        };
        recovered = recovered && guard.poisoned() && guard.data().n == 1;
        let data = guard.data();
        data.n = 9;
        guard.heal();
    }
    {
        var guard = match (await slow.get().lock()) {
            Ok(g) => g,
            Err(_) => { return Err(Fail.Other); },
        };
        slowHealed = guard.data().n;
    }
    return Ok(Locks {
        crashes: counted.load(),
        poisonedKinds: poisoned,
        recovered: recovered,
        healed: healed,
        slowHealed: slowHealed,
    });
}

struct Tries {
    started: u32;
    refused: u32;
    numbered: u32;
    crashes: u64;
    result: u32; // 1: the set's result is what the crashed child stands for
}

/// `trySpawnIsolated` (ADR 0175 amendment 1): into a set of two, two start (one traps), a third is refused while
/// they run, and a fourth starts once there is room.
async fn tries() -> Result<Tries, Fail> {
    let started = AtomicU64.create(0);
    let refused = AtomicU64.create(0);
    let numbered = AtomicU64.create(0);
    let counted = AtomicU64.create(0);
    let r = await Async.tasks(2, Fail.NoMemory, async (set) => {
        var k: u32 = 0;
        while (k < 4) {
            if (k == 3) {
                await nap(60)?; // both have ended: room again
            }
            let w = k;
            let tried = set.trySpawnIsolated(Fail.Crashed, async move () => {
                if (w == 0) {
                    return await traps(0);
                }
                return await nap(if (w == 1) 30u64 else 1u64);
            });
            match (tried) {
                Some(id) => {
                    started.fetchAdd(1);
                    if (id != 0) {
                        numbered.fetchAdd(1);
                    }
                }
                null => { refused.fetchAdd(1); }
            }
            k += 1;
        }
        await nap(10)?;
        counted.store(set.crashes());
        return Ok();
    });
    return Ok(Tries {
        started: truncate<u32>(started.load()),
        refused: truncate<u32>(refused.load()),
        numbered: truncate<u32>(numbered.load()),
        crashes: counted.load(),
        result: match (r) {
            Err(Fail.Crashed) => 1,
            _ => 0,
        },
    });
}

struct All {
    kinds: Kinds;
    shallow: Family;
    deep: Family;
    locks: Locks;
    tries: Tries;
    total: u64;
}

async fn all() -> Result<All, Fail> {
    let k = await kinds()?;
    let shallow = await families(false)?;
    let deep = await families(true)?;
    let l = await locks()?;
    let t = await tries()?;
    return Ok(All { kinds: k, shallow: shallow, deep: deep, locks: l, tries: t, total: Isolation.report().crashes });
}

fn say(ok: bool, what: StringView) {
    Console.write(if (ok) "ok   " else "FAIL ");
    Console.writeLine(what);
}

fn main() {
    match (Async.run(all())) {
        Ok(o) => {
            say(o.kinds.numbered, "six isolated children, each numbered");
            say(o.kinds.result == 1, "the set's result is what a crashed child stands for");
            say(o.kinds.crashes == 6, "six crashes counted");
            say(o.kinds.worked == 3, "their three siblings finished");
            say(o.kinds.watched == 6 && o.kinds.kinds == 63, "the watcher was told of each, by its kind");
            say(o.shallow.result == 1 && o.shallow.crashes == 1, "a root that traps: one crash");
            say(o.shallow.ran == 0, "none of the tasks under it ran again");
            say(o.deep.result == 1 && o.deep.crashes == 1, "a task under the root that traps: one crash");
            say(o.deep.ran == 0, "neither its root nor the other tasks ran again");
            say(o.locks.crashes == 3 && o.locks.poisonedKinds == 2, "a lock held at the trap poisons: the next two takers trap for it");
            say(o.locks.recovered, "recover() takes a poisoned lock, with the data as the trap left it");
            say(o.locks.healed == 7 && o.locks.slowHealed == 9, "healed, lock() takes it again");
            say(o.tries.started == 3 && o.tries.refused == 1 && o.tries.numbered == 3, "trySpawnIsolated: three started, each numbered, one refused while the set was full");
            say(o.tries.crashes == 1 && o.tries.result == 1, "trySpawnIsolated: its child's trap is the set's crash");
            say(o.total == 12, "twelve crashes in all, and the program goes on");
        }
        Err(_) => { Console.writeLine("FAIL the run returned an error"); }
    }
}
'''

ENDS = r'''// What still ends the process (ADR 0175 §2.3), what a crash leaks, and a socket's closer: one case an argument.
@header("stdlib.h")
extern "C" {
    @effects()
    fn qsort(base: *mut void, count: usize, size: usize, compare: extern "C" fn(*void, *void) -> i32);
}

enum Fail {
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

fn trap(n: usize) -> u64 {
    var numbers = [1u64, 2, 3];
    return numbers[n + 3]; // index out of bounds
}

/// C's qsort calls this, and it traps: with C's frames between the executor and the trap.
/// Safety: qsort gives two elements of the array.
@callback
unsafe fn backwards(a: *void, b: *void) -> i32 {
    let first = (a as *u32).read();
    return truncate<i32>(trap(first as usize));
}

/// Safety: the array has three elements of four bytes, and `backwards` reads elements only.
fn sorted() {
    var numbers = [3u32, 1, 2];
    unsafe { qsort(borrow mut numbers as *mut [u32; 3] as *mut void, 3, 4, backwards); }
}

async fn child(how: u32) -> Result<void, Fail> {
    await nap(2)?;
    if (how == 0) {
        let critical = Critical.enter();
        let n = trap(0); // in a critical section
    } else if (how == 1) {
        sorted(); // under C's frames
    } else if (how == 2) {
        let n = await Async.blocking(() => trap(0)); // on another thread
    } else {
        let n = trap(0);
    }
    return Ok();
}

/// One child, isolated or (`plain`) not, that traps as `how` says: the process ends, or prints "survived".
async fn one(how: u32, plain: bool) -> Result<u64, Fail> {
    let counted = AtomicU64.create(0);
    let r = await Async.tasks(4, Fail.NoMemory, async (set) => {
        if (plain) {
            await set.spawn(async move () => await child(how))?;
        } else {
            let id = await set.spawnIsolated(Fail.Crashed, async move () => await child(how))?;
        }
        await nap(30)?;
        counted.store(set.crashes());
        return Ok();
    });
    return Ok(counted.load());
}

/// Holds 64 KiB and traps.
async fn greedy() -> Result<void, Fail> {
    var block = Vector<u8>.create();
    match (block.reserve(65536)) {
        Ok() => {}
        Err(_) => { return Err(Fail.NoMemory); }
    }
    let n = trap(0);
    return Ok();
}

struct Many {
    crashes: u64;
    leaked: u64;
    each: u64; // the first crash's leakedBytes
}

/// `count` children that each hold 64 KiB and trap, 64 at a time.
async fn many(count: u64) -> Result<Many, Fail> {
    let crashes = match (Channel<TaskCrash>.create(1)) {
        Ok(c) => c,
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let r = await Async.tasks(64, Fail.NoMemory, async (set) => {
        if (!Isolation.watch(set, crashes)) {
            return Err(Fail.NoMemory);
        }
        var started: u64 = 0;
        while (started < count) {
            let id = await set.spawnIsolated(Fail.Crashed, async move () => await greedy())?;
            started += 1;
        }
        return Ok();
    });
    let first = match (crashes.tryRecv()) {
        Some(crash) => crash.leakedBytes,
        null => 0,
    };
    let report = Isolation.report();
    return Ok(Many { crashes: report.crashes, leaked: report.leakedBytes, each: first });
}

/// Reads from the stream it owns, into a buffer of its own, for ever: parked in the read when its unit traps.
async fn reader(stream: borrow AsyncTcpStream, got: borrow Arc<AtomicU64>) -> Result<void, Fail> {
    var buffer = [0u8; 256];
    loop {
        match (await stream.read(mut buffer[..])) {
            Ok(n) => { got.get().fetchAdd(n as u64 + 1000); }
            Err(_) => { return Err(Fail.Other); }
        }
    }
    return Ok();
}

/// The unit: a reader parked on the socket, and a trap beside it.
async fn served(stream: AsyncTcpStream, got: Arc<AtomicU64>) -> Result<void, Fail> {
    return await Async.scope(Fail.NoMemory, (s) => {
        s.spawn(async () => await reader(stream, got));
        s.spawn(async () => {
            await nap(10)?;
            let n = trap(0);
            return Ok();
        });
    });
}

struct Sockets {
    crashes: u64;
    readAfter: u64;   // what the unit's reader counted after the trap: nothing
    wrote: bool;      // the peer's write after the trap went out
    end: u64;         // what the peer read once the owner shut the socket: 0 bytes, the end
}

/// A connection handed to an isolated child that traps: a write that arrives afterwards lands in the buffer the
/// unit left, and is read by nobody; the owner's closer ends the connection, and the peer sees it.
async fn sockets() -> Result<Sockets, Fail> {
    let listener = match (AsyncTcpListener.bind("127.0.0.1", 0)) {
        Ok(l) => l,
        Err(_) => { return Err(Fail.Other); },
    };
    let port = match (listener.port()) {
        Ok(p) => p,
        Err(_) => { return Err(Fail.Other); },
    };
    let peer = match (await AsyncTcpStream.connect("127.0.0.1", port)) {
        Ok(s) => s,
        Err(_) => { return Err(Fail.Other); },
    };
    let accepted = match (await listener.accept()) {
        Ok(s) => s,
        Err(_) => { return Err(Fail.Other); },
    };
    let closer = match (accepted.closer()) {
        Ok(c) => c,
        Err(_) => { return Err(Fail.Other); },
    };
    let got = match (Arc<AtomicU64>.create(AtomicU64.create(0))) {
        Ok(a) => a,
        Err(_) => { return Err(Fail.NoMemory); },
    };
    let seen = got.clone();
    let counted = AtomicU64.create(0);
    let wrote = AtomicU64.create(0);
    let end = AtomicU64.create(99);
    var handed = Vector<AsyncTcpStream>.create();
    match (handed.push(accepted)) {
        Ok() => {}
        Err(_) => { return Err(Fail.NoMemory); }
    }
    let r = await Async.tasks(4, Fail.NoMemory, async (set) => {
        let stream = handed.pop() ?? { return Err(Fail.Other); };
        let mine = got.clone();
        let id = await set.spawnIsolated(Fail.Crashed, async move () => await served(stream, mine))?;
        await nap(40)?; // the unit has trapped, its reader parked on the socket
        counted.store(set.crashes());
        // The peer writes: the kernel's read, if one is in flight, completes into the unit's buffer.
        match (await peer.write("after the trap".bytes())) {
            Ok() => { wrote.store(1); }
            Err(_) => {}
        }
        // And ends its side, as a client that has sent its request may: on macOS that made SHUT_RDWR refuse
        // (ENOTCONN) and send nothing, so the closer shuts each side on its own (ADR 0175 amendment 1).
        match (peer.shutdownWrite()) {
            Ok() => {}
            Err(_) => { wrote.store(0); }
        }
        await nap(20)?;
        // The owner ends the connection it gave away.
        match (closer.shutdown()) {
            Ok() => {}
            Err(_) => { return Err(Fail.Other); }
        }
        var buffer = [0u8; 16];
        match (await peer.read(mut buffer[..])) {
            Ok(n) => { end.store(n as u64); }
            Err(_) => { end.store(98); }
        }
        return Ok();
    });
    return Ok(Sockets { crashes: counted.load(), readAfter: seen.get().load(), wrote: wrote.load() == 1, end: end.load() });
}

fn main() {
    let how = Process.argument(1) ?? "";
    if (how == "many") {
        match (Async.run(many(10000))) {
            Ok(m) => {
                Console.writeLine(m.crashes);
                // Each held 64 KiB, as its allocator rounds it: all of it is still held, and little more.
                Console.writeLine(if (m.each >= 65536 && m.each < 131072) "one crash leaks what it held" else "one crash's count is off");
                Console.writeLine(if (m.leaked >= 10000 * 65536 && m.leaked < 10000 * 131072) "ten thousand leak ten thousand times that" else "the total is off");
            }
            Err(_) => { Console.writeLine("failed"); }
        }
        return;
    }
    if (how == "sockets") {
        match (Async.run(sockets())) {
            Ok(s) => {
                Console.writeLine(if (s.crashes == 1) "the unit trapped" else "no crash");
                Console.writeLine(if (s.wrote) "the peer wrote after it" else "the peer could not write");
                Console.writeLine(if (s.readAfter == 0) "nobody read it" else "the dead reader ran");
                Console.writeLine(if (s.end == 0) "the owner's closer ended the connection" else "the peer saw no end");
            }
            Err(_) => { Console.writeLine("failed"); }
        }
        return;
    }
    let plain = how == "plain";
    let which: u32 = if (how == "critical") 0 else if (how == "callback") 1 else if (how == "blocking") 2 else 3;
    match (Async.run(one(which, plain))) {
        Ok(crashes) => {
            Console.write("survived, crashes ");
            Console.writeLine(crashes);
        }
        Err(_) => { Console.writeLine("failed"); }
    }
}
'''

SURVIVE_LINES = [
    "ok   six isolated children, each numbered",
    "ok   the set's result is what a crashed child stands for",
    "ok   six crashes counted",
    "ok   their three siblings finished",
    "ok   the watcher was told of each, by its kind",
    "ok   a root that traps: one crash",
    "ok   none of the tasks under it ran again",
    "ok   a task under the root that traps: one crash",
    "ok   neither its root nor the other tasks ran again",
    "ok   a lock held at the trap poisons: the next two takers trap for it",
    "ok   recover() takes a poisoned lock, with the data as the trap left it",
    "ok   healed, lock() takes it again",
    "ok   trySpawnIsolated: three started, each numbered, one refused while the set was full",
    "ok   trySpawnIsolated: its child's trap is the set's crash",
    "ok   twelve crashes in all, and the program goes on",
]
MANY_LINES = ["10000", "one crash leaks what it held", "ten thousand leak ten thousand times that"]
SOCKET_LINES = ["the unit trapped", "the peer wrote after it", "nobody read it",
                "the owner's closer ended the connection"]
PANIC = "ceesafe panic: index out of bounds at "
DEADLINE = 240.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    windows = os.name == "nt"
    problems = []
    cases = 0
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")  # what a trapped task held is leaked: that is the rule

    def expect(what, got, wanted, detail=""):
        nonlocal cases
        cases += 1
        if got != wanted:
            problems.append(f"{what}: {got!r}, expected {wanted!r} {detail}")

    with tempfile.TemporaryDirectory(prefix="ceesafe-isolated-") as work:
        built = {}
        for stem, source in (("survive", SURVIVE), ("ends", ENDS)):
            path = os.path.join(work, stem + ".cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source)
            for label, flags in (("", []), ("-san", ["--cflag", "-fsanitize=address,undefined"])):
                exe = os.path.join(work, stem + label + (".exe" if windows else ""))
                b = subprocess.run([compiler, "build", path, "-o", exe] + flags, capture_output=True, text=True)
                if b.returncode == 0:
                    built[stem + label] = exe
                elif not flags:
                    problems.append(f"{stem}: build failed: {b.stderr.strip()[:800]}")

        def run(key, *words):
            return subprocess.run([built[key], *words], capture_output=True, text=True, timeout=DEADLINE,
                                  env=environment)

        # 1. What is left standing.
        for key in ("survive", "survive-san"):
            if key in built:
                r = run(key)
                expect(key, r.stdout.splitlines(), SURVIVE_LINES, f"(status {r.returncode}, {r.stderr.strip()[-300:]!r})")
                cases += 1
                named = [line for line in r.stderr.splitlines() if "ended alone)" in line]
                if len(named) != 12 or r.returncode != 0:
                    problems.append(f"{key}: {len(named)} panics named an isolated task, status {r.returncode}")
        r = subprocess.run([compiler, "run", "--interp", os.path.join(work, "survive.cee")], capture_output=True,
                           text=True, timeout=DEADLINE)
        expect("survive, interpreted", r.stdout.splitlines(), SURVIVE_LINES, f"({r.stderr.strip()[-300:]!r})")

        # 2. What still ends the process, and what does not.
        if "ends" in built:
            r = run("ends", "isolated")
            expect("isolated", r.stdout.splitlines(), ["survived, crashes 1"])
            cases += 1
            if r.returncode != 0 or PANIC not in r.stderr or "(isolated task 1: ended alone)" not in r.stderr:
                problems.append(f"isolated: status {r.returncode}, {r.stderr.strip()[:300]!r}")
            for how in ("critical", "callback", "blocking", "plain"):
                r = run("ends", how)
                cases += 1
                if r.returncode == 0 or r.stdout != "" or PANIC not in r.stderr or "isolated task" in r.stderr:
                    problems.append(f"{how}: the process went on: status {r.returncode}, wrote {r.stdout!r}, "
                                    f"{r.stderr.strip()[:300]!r}")
            r = run("ends", "many")
            expect("many", r.stdout.splitlines(), MANY_LINES, f"(status {r.returncode})")
        for key in ("ends", "ends-san"):
            if key in built:
                r = run(key, "sockets")
                expect(key + " sockets", r.stdout.splitlines(), SOCKET_LINES,
                       f"(status {r.returncode}, {r.stderr.strip()[-400:]!r})")

    for problem in problems:
        print(f"FAIL {problem}")
    print(f"isolated_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
