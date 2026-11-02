#!/usr/bin/env python3
"""Native checks of AsyncUdpSocket (ADR 0050) that the corpus cannot make, since the interpreter reports no ECN bits and
sends no segments in one call: ECN bits sent and seen on IPv4 and IPv6, a datagram sent from a chosen local address, a
segmented send of more segments than one call takes, and a receive cancelled in flight. The program is built with clang
(GCC without clang) under ASan+UBSan and run with CEESAFE_ALLOC_REPORT=1 (and again with CEESAFE_REACTOR_URING=1 on
Linux, and CEESAFE_REACTOR_POLL=1); it must exit 0 with live=0 and print the expected lines, computed here.

usage: python3 tools/udp_check.py --compiler out/ceesafec [--no-sanitizers]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fault_injection import SANITIZER_CC  # noqa: E402

ALLOC = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")
SEGMENTS = 100
SEGMENT = 100

PROGRAM = """
fn io<T>(r: Result<T, IoError>) -> Result<T, AsyncError> => match (r) {
    Ok(v) => Ok(v),
    Err(e) => Err(AsyncError.Io(e)),
};

fn add(out: borrow mut Vector<u64>, value: u64) -> Result<void, AsyncError> {
    match (out.push(value)) {
        Ok() => { return Ok(); }
        Err(_) => { return Err(AsyncError.Io(IoError.OutOfMemory)); }
    }
}

/// The ECN bits each datagram carried, sent with 1, 2 and 3, on `six` or IPv4.
async fn ecn(out: borrow mut Vector<u64>, six: bool) -> Result<void, AsyncError> {
    var options = UdpOptions.defaults();
    options.receiveEcn = true;
    let here = if (six) SocketAddresses.v6Loopback(0) else SocketAddresses.v4Loopback(0);
    let a = io(AsyncUdpSocket.bind(here, options))?;
    let b = io(AsyncUdpSocket.bind(here, UdpOptions.defaults()))?;
    let to = io(a.localAddress())?;
    let data = [9u8; 20];
    var buffer = [0u8; 64];
    for (bits in 1..4u8) {
        let message = UdpMessage { parts: IoParts.of(data[..]), to: to, from: null, ecn: bits, segmentSize: 0 };
        match (await b.sendMsg(borrow message)) {
            Ok(_) => {
                let got = await a.recvMsg(mut buffer[..])?;
                add(out, got.ecn as u64)?;
            }
            Err(AsyncError.Io(IoError.InvalidInput)) => { add(out, 99)?; } // a sender may not set CE (Windows)
            Err(e) => { return Err(e); }
        }
    }
    // Without the ECN bits asked for, none are reported.
    let plain = UdpMessage { parts: IoParts.of(data[..]), to: io(b.localAddress())?, from: null, ecn: 2, segmentSize: 0 };
    await a.sendMsg(borrow plain)?;
    let got = await b.recvMsg(mut buffer[..])?;
    add(out, got.ecn as u64)?;
    return Ok();
}

/// A datagram from a chosen source address: the peer sees that address.
async fn source(out: borrow mut Vector<u64>) -> Result<void, AsyncError> {
    let a = io(AsyncUdpSocket.bind(SocketAddresses.v4Loopback(0), UdpOptions.defaults()))?;
    let b = io(AsyncUdpSocket.bind(SocketAddresses.v4Any(0), UdpOptions.defaults()))?;
    let data = [1u8, 2, 3];
    let message = UdpMessage {
        parts: IoParts.of(data[..]),
        to: io(a.localAddress())?,
        from: Some(SocketAddresses.v4Loopback(0)),
        ecn: 0,
        segmentSize: 0,
    };
    await b.sendMsg(borrow message)?;
    var buffer = [0u8; 64];
    let got = await a.recvFrom(mut buffer[..])?;
    add(out, match (got.from) {
        SocketAddress.V4(bytes, _) => if (bytes[0] == 127 && bytes[3] == 1) 1 else 0,
        _ => 0,
    })?;
    return Ok();
}

/// @SEGMENTS@ segments of @SEGMENT@ bytes in one sendMsg: its count, then each datagram's length and first byte.
async fn segments(out: borrow mut Vector<u64>) -> Result<void, AsyncError> {
    var options = UdpOptions.defaults();
    options.recvBuffer = 1048576;
    let a = io(AsyncUdpSocket.bind(SocketAddresses.v4Loopback(0), options))?;
    let b = io(AsyncUdpSocket.bind(SocketAddresses.v4Loopback(0), UdpOptions.defaults()))?;
    var data = Vector<u8>.create();
    for (i in 0..@TOTAL@u32) {
        match (data.push(truncate<u8>(i / @SEGMENT@))) {
            Ok() => {}
            Err(_) => { return Err(AsyncError.Io(IoError.OutOfMemory)); }
        }
    }
    let all = data.view();
    let message = UdpMessage { parts: IoParts.of(all), to: io(a.localAddress())?, from: null, ecn: 0, segmentSize: @SEGMENT@ };
    let sent = await b.sendMsg(borrow message)?;
    add(out, sent as u64)?;
    var buffer = [0u8; 4096];
    var right: u64 = 0;
    for (k in 0..@SEGMENTS@u32) {
        let got = await a.recvFrom(mut buffer[..])?;
        if (got.length == @SEGMENT@ && buffer[0] as u32 == k) {
            right += 1;
        }
    }
    add(out, right)?;
    return Ok();
}

/// A receive with nothing coming, cancelled by a deadline: TimedOut, with nothing left behind.
async fn cancelled(out: borrow mut Vector<u64>) -> Result<void, AsyncError> {
    let a = io(AsyncUdpSocket.bind(SocketAddresses.v4Loopback(0), UdpOptions.defaults()))?;
    var buffer = [0u8; 64];
    let r: Result<usize, AsyncError> = await Async.timeout(20, AsyncError.TimedOut, async () => {
        let got = await a.recvFrom(mut buffer[..])?;
        return Ok(got.length);
    });
    add(out, match (r) {
        Err(AsyncError.TimedOut) => 1,
        _ => 0,
    })?;
    return Ok();
}

async fn run() -> Result<Vector<u64>, AsyncError> {
    var out = Vector<u64>.create();
    await ecn(mut out, false)?;
    await ecn(mut out, true)?;
    await source(mut out)?;
    await segments(mut out)?;
    await cancelled(mut out)?;
    return Ok(out);
}

fn main() {
    match (Async.run(run())) {
        Ok(out) => {
            for (v in out) {
                Console.writeLine(v);
            }
        }
        Err(AsyncError.Io(e)) => { Console.writeLine(e.message()); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
"""


def expected():
    lines = []
    # Windows refuses a sender's CE (3) as InvalidInput; Linux and macOS send it.
    ce = "99" if os.name == "nt" else "3"
    for _ in range(2):  # IPv4, IPv6
        lines += ["1", "2", ce, "0"]
    lines.append("1")
    lines.append(str(SEGMENTS * SEGMENT))
    lines.append(str(SEGMENTS))
    lines.append("1")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--no-sanitizers", action="store_true",
                        help="build with the default C compiler, without ASan (a host without clang: Windows with cl)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    want = expected()
    source = (PROGRAM.replace("@SEGMENTS@", str(SEGMENTS)).replace("@SEGMENT@", str(SEGMENT))
              .replace("@TOTAL@", str(SEGMENTS * SEGMENT)))
    problems = []
    runs = 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-udp-") as work:
        path = os.path.join(work, "udp.cee")
        open(path, "w").write(source)
        exe = os.path.join(work, "udp")
        flags = ["--cc", SANITIZER_CC, "--cflag", "-fsanitize=address,undefined", "--cflag",
                 "-fno-sanitize-recover=all", "--cflag", "-O1"]
        b = subprocess.run([compiler, "build"] + ([] if args.no_sanitizers else flags) + [path, "-o", exe],
                           capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {b.stderr.strip()[-600:]}")
        else:
            if os.name == "nt" and not exe.endswith(".exe"):
                exe += ".exe"
            modes = [("default", {}), ("poll", {"CEESAFE_REACTOR_POLL": "1"})]
            if sys.platform.startswith("linux"):
                modes.append(("io_uring", {"CEESAFE_REACTOR_URING": "1"}))
            for mode, extra in modes:
                runs += 1
                env = dict(os.environ, CEESAFE_ALLOC_REPORT="1", **extra)
                p = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
                m = ALLOC.search(p.stderr)
                got = p.stdout.strip().splitlines()
                if p.returncode != 0 or got != want or not m or m.group(2) != "0":
                    problems.append(f"{mode}: status {p.returncode}, printed {got!r} (want {want!r}), "
                                    f"stderr {p.stderr.strip()[:300]!r}")
    print(f"udp_check: ECN, a source address, {SEGMENTS} segments, a cancelled receive; {runs} reactors; "
          f"{len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
