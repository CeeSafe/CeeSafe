#!/usr/bin/env python3
"""Native checks of AsyncDns (ADR 0048) that the corpus cannot make: they need the runtime's test hook
CEESAFE_DNS_DELAY_MS (each lookup waits that long before the resolver), timing, and ASan. Each program is built with
clang (GCC without clang) under ASan+UBSan and run with CEESAFE_ALLOC_REPORT=1; each must exit 0 with live=0.

1. A running lookup cancelled by Async.timeout returns at once ("timed out"), well before its resolver would.
2. Eight lookups in a scope, cancelled while four run and four wait in the queue, all return at once.
3. Eight lookups at once with a 300 ms resolver take two rounds of four (the thread bound): at least 600 ms, and
   less than three rounds.
4. 100 lookups of localhost at once all resolve.

usage: python3 tools/dns_check.py --compiler out/ceesafec [--no-sanitizers]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fault_injection import SANITIZER_CC  # noqa: E402

ALLOC = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")

COMMON = """
fn outcome(r: Result<Vector<SocketAddress>, AsyncError>) -> u32 => match (r) {
    Ok(list) => if (list.length > 0) 1 else 9,
    Err(AsyncError.Cancelled) => 2,
    Err(AsyncError.TimedOut) => 3,
    Err(_) => 4,
};
"""

PROGRAMS = {
    "cancel-running": (COMMON + """
async fn run() -> Result<u32, AsyncError> {
    let r: Result<Vector<SocketAddress>, AsyncError> = await Async.timeout(
        50,
        AsyncError.TimedOut,
        async () => await AsyncDns.resolve("slow.invalid", 1),
    );
    return Ok(outcome(r));
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
""", "3", 2000, 0.0, 1.5),
    "cancel-queued": (COMMON + """
async fn one(name: StringView) -> Result<void, AsyncError> {
    let list = await AsyncDns.resolve(name, 1)?;
    return Ok();
}

async fn run() -> Result<u32, AsyncError> {
    let r = await Async.scope(AsyncError.TimedOut, (s) => {
        for (k in 0..8) {
            s.spawn(async () => await one("localhost"));
        }
        s.spawn(async () => {
            await Timer.sleep(50)?;
            return Err(AsyncError.TimedOut); // cancels the eight: four running, four queued
        });
    });
    return Ok(match (r) {
        Ok() => 1,
        Err(AsyncError.TimedOut) => 3,
        Err(_) => 4,
    });
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
""", "3", 2000, 0.0, 1.5),
    "thread-bound": (COMMON + """
async fn one(count: borrow mut u32) -> Result<void, AsyncError> {
    let list = await AsyncDns.resolve("localhost", 1)?;
    if (list.length > 0) {
        count += 1;
    }
    return Ok();
}

async fn run() -> Result<u32, AsyncError> {
    var a: u32 = 0;
    var b: u32 = 0;
    var c: u32 = 0;
    var d: u32 = 0;
    var e: u32 = 0;
    var f: u32 = 0;
    var g: u32 = 0;
    var h: u32 = 0;
    await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => await one(mut a));
        s.spawn(async () => await one(mut b));
        s.spawn(async () => await one(mut c));
        s.spawn(async () => await one(mut d));
        s.spawn(async () => await one(mut e));
        s.spawn(async () => await one(mut f));
        s.spawn(async () => await one(mut g));
        s.spawn(async () => await one(mut h));
    })?;
    return Ok(a + b + c + d + e + f + g + h);
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
""", "8", 300, 0.6, 0.9 + 0.6),
    "hundred": (COMMON + """
async fn one(count: borrow mut u32) -> Result<void, AsyncError> {
    for (k in 0..10) {
        let list = await AsyncDns.resolve("localhost", 1)?;
        if (list.length > 0) {
            count += 1;
        }
    }
    return Ok();
}

async fn run() -> Result<u32, AsyncError> {
    var total: u32 = 0;
    await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => await one(mut total));
    })?;
    var a: u32 = 0;
    var b: u32 = 0;
    var c: u32 = 0;
    var d: u32 = 0;
    var e: u32 = 0;
    var f: u32 = 0;
    var g: u32 = 0;
    var h: u32 = 0;
    var i: u32 = 0;
    await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => await one(mut a));
        s.spawn(async () => await one(mut b));
        s.spawn(async () => await one(mut c));
        s.spawn(async () => await one(mut d));
        s.spawn(async () => await one(mut e));
        s.spawn(async () => await one(mut f));
        s.spawn(async () => await one(mut g));
        s.spawn(async () => await one(mut h));
        s.spawn(async () => await one(mut i));
    })?;
    return Ok(total + a + b + c + d + e + f + g + h + i);
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
""", "100", 0, 0.0, 30.0),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--no-sanitizers", action="store_true",
                        help="build with the default C compiler, without ASan (a host without clang: Windows with cl)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-dns-") as work:
        for name, (source, expected, delay, at_least, at_most) in PROGRAMS.items():
            path = os.path.join(work, name + ".cee")
            open(path, "w").write(source)
            exe = os.path.join(work, name)
            flags = ["--cc", SANITIZER_CC, "--cflag", "-fsanitize=address,undefined", "--cflag",
                     "-fno-sanitize-recover=all", "--cflag", "-O1"]
            b = subprocess.run([compiler, "build"] + ([] if args.no_sanitizers else flags) + [path, "-o", exe],
                               capture_output=True, text=True)
            if b.returncode != 0:
                problems.append(f"{name}: build failed: {b.stderr.strip()[-400:]}")
                continue
            env = dict(os.environ, CEESAFE_ALLOC_REPORT="1")
            if delay > 0:
                env["CEESAFE_DNS_DELAY_MS"] = str(delay)
            start = time.monotonic()
            if os.name == "nt" and not exe.endswith(".exe"):
                exe += ".exe"
            p = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
            elapsed = time.monotonic() - start
            m = ALLOC.search(p.stderr)
            if p.returncode != 0 or p.stdout.strip() != expected or not m or m.group(2) != "0":
                problems.append(f"{name}: status {p.returncode}, printed {p.stdout.strip()!r} (want {expected}), "
                                f"stderr {p.stderr.strip()[:300]!r}")
            elif not at_least <= elapsed <= at_most:
                problems.append(f"{name}: took {elapsed:.2f} s, not {at_least}..{at_most} s")
    print(f"dns_check: {len(PROGRAMS)} programs; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
