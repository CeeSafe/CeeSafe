#!/usr/bin/env python3
"""A cancellation wakes only the tasks it reaches (bug B5, type-system.md §40.7), measured: 100 tasks wait on idle
sockets while 200 races resolve, each cancelling its loser, a timer. With CEESAFE_CANCEL_REPORT=1 the runtime prints how
many tasks cancellations woke (`woken=`). Each race's cancellation reaches its loser only, so the count is at most one
per race (200); before the fix every cancellation woke every idle reader too (about 100 × 200). Run natively, on the
host's reactor and on poll().

usage: python3 tools/wake_check.py --compiler out/ceesafec
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

PROGRAM = """
async fn reader(s: borrow AsyncTcpStream) -> Result<void, AsyncError> {
    var buffer = [0u8; 16];
    loop {
        let n = await s.read(mut buffer[..])?;
        if (n == 0) {
            return Ok();
        }
    }
}

async fn tick(ms: u64) -> Result<u32, AsyncError> {
    await Timer.sleep(ms)?;
    return Ok(0);
}

async fn races(clients: borrow Vector<AsyncTcpStream>) -> Result<void, AsyncError> {
    for (k in 0..200) {
        let r: Result<Raced<u32>, AsyncError> = await Async.race(async () => await tick(0), async () => await tick(100000));
        let raced = r?;
    }
    for (c in clients.view()) {
        match (c.shutdownWrite()) {
            Ok() => {}
            Err(e) => { return Err(AsyncError.Io(e)); }
        }
    }
    return Ok();
}

async fn run() -> Result<u32, AsyncError> {
    let listener = match (AsyncTcpListener.bind("127.0.0.1", 0)) {
        Ok(l) => l,
        Err(e) => { return Err(AsyncError.Io(e)); },
    };
    let port = match (listener.port()) {
        Ok(p) => p,
        Err(e) => { return Err(AsyncError.Io(e)); },
    };
    var clients = Vector<AsyncTcpStream>.create();
    var servers = Vector<AsyncTcpStream>.create();
    for (k in 0..100) {
        clients.push(await AsyncTcpStream.connect("127.0.0.1", port)?)!;
        servers.push(await listener.accept()?)!;
    }
    await Async.scope(AsyncError.Cancelled, (s) => {
        for (k in 0..servers.length) {
            s.spawn(async () => await reader(servers[k]));
        }
        s.spawn(async () => await races(borrow clients));
    })?;
    return Ok(1);
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
"""

WOKEN = re.compile(r"woken=(\d+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-wake-") as work:
        path = os.path.join(work, "wake.cee")
        open(path, "w").write(PROGRAM)
        exe = os.path.join(work, "wake")
        b = subprocess.run([os.path.abspath(args.compiler), "build", path, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            sys.exit(f"wake_check: build failed: {b.stderr[-600:]}")
        if os.name == "nt":
            exe += ".exe"
        for label, extra in (("reactor", {}), ("poll", {"CEESAFE_REACTOR_POLL": "1"})):
            env = dict(os.environ, CEESAFE_CANCEL_REPORT="1", **extra)
            p = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
            counts = WOKEN.findall(p.stderr)
            if p.returncode != 0 or p.stdout.strip() != "1" or not counts:
                problems.append(f"{label}: status {p.returncode}, printed {p.stdout.strip()!r}, {p.stderr.strip()[:200]!r}")
            elif int(counts[-1]) > 200:
                problems.append(f"{label}: cancellations woke {counts[-1]} tasks, more than the 200 losers")
            else:
                print(f"wake_check: {label}: {counts[-1]} tasks woken by 200 cancellations")
    print(f"wake_check: {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
