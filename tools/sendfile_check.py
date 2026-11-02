#!/usr/bin/env python3
"""Native checks of sendFile (ADR 0051) that the corpus cannot make: a file larger than 4 GiB. The file is sparse, so it
takes almost no disk space: this script writes marker bytes on both sides of the 4 GiB boundary and at the file's end,
and the expected FNV-1a of each range, the oracle, is computed here from the same bytes (zeros elsewhere). The program
is built with clang (GCC without clang) under ASan+UBSan and run with CEESAFE_ALLOC_REPORT=1; it must exit 0 with
live=0 and print each range's count and FNV-1a.

Ranges sent with sendFileAll to a reader on loopback:
1. across the 4 GiB boundary (offset 2^32 - 3000, 6000 bytes): a 64-bit offset, and a step that the kernel's offset
   moves across 2^32;
2. the file's last 5000 bytes, past 2^32 + 2^20;
3. past the end (the last 100 bytes and 50 more): InvalidInput, the 100 bytes sent.

usage: python3 tools/sendfile_check.py --compiler out/ceesafec [--no-sanitizers]
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
GIB4 = 1 << 32
SIZE = GIB4 + (1 << 20) + 12345
# (offset, bytes) written into the sparse file.
MARKERS = [
    (GIB4 - 3000, bytes((5 * i + 1) % 251 for i in range(3000))),
    (GIB4, bytes((11 * i + 7) % 253 for i in range(3000))),
    (SIZE - 5000, bytes((13 * i + 2) % 241 for i in range(5000))),
]
RANGES = [(GIB4 - 3000, 6000), (SIZE - 5000, 5000), (SIZE - 100, 150)]

PROGRAM = """
fn fnv(h: u32, bytes: Slice<u8>) -> u32 {
    var x = h;
    for (b in bytes) {
        x = truncate<u32>((x ^ (b as u32)) as u64 * 16777619);
    }
    return x;
}

async fn readToEnd(stream: borrow AsyncTcpStream) -> Result<u64, AsyncError> {
    var h: u32 = 2166136261;
    var got: u64 = 0;
    var buffer = [0u8; 4096];
    loop {
        let n = await stream.read(mut buffer[..])?;
        if (n == 0) {
            return Ok(got * 4294967296 + h as u64);
        }
        let read = buffer[0..n];
        h = fnv(h, read);
        got += n as u64;
    }
}

async fn range(
    listener: borrow AsyncTcpListener,
    port: u16,
    file: borrow File,
    offset: u64,
    length: u64,
    received: borrow mut u64,
) -> Result<u64, AsyncError> {
    let client = await AsyncTcpStream.connect("127.0.0.1", port)?;
    let server = await listener.accept()?;
    var code: u64 = 0;
    var got: u64 = 0;
    await Async.all(
        async () => {
            code = match (await client.sendFileAll(file, offset, length)) {
                Ok() => 0,
                Err(AsyncError.Io(IoError.InvalidInput)) => 1,
                Err(_) => 2,
            };
            match (client.shutdownWrite()) {
                Ok() => {}
                Err(e) => { return Err(AsyncError.Io(e)); }
            }
            return Ok();
        },
        async () => {
            got = await readToEnd(borrow server)?;
            return Ok();
        },
    )?;
    received = got;
    return Ok(code);
}

async fn run(file: borrow File) -> Result<[u64; 6], AsyncError> {
    var out = [0u64; 6];
    let listener = match (AsyncTcpListener.bind("127.0.0.1", 0)) {
        Ok(l) => l,
        Err(e) => { return Err(AsyncError.Io(e)); }
    };
    let port = match (listener.port()) {
        Ok(p) => p,
        Err(e) => { return Err(AsyncError.Io(e)); }
    };
@CALLS@
    return Ok(out);
}

fn main() {
    let path = match (Environment.variable("CEESAFE_SENDFILE_PATH")) {
        Some(p) => p,
        null => { Console.writeLine("no path"); return; }
    };
    let file = File.open(path)!;
    match (Async.run(run(borrow file))) {
        Ok(out) => {
            for (k in 0..3usize) {
                Console.write(out[2 * k]);
                Console.write(" ");
                Console.write(out[2 * k + 1] / 4294967296);
                Console.write(" ");
                Console.writeLine(out[2 * k + 1] % 4294967296);
            }
        }
        Err(AsyncError.Io(e)) => { Console.writeLine(e.message()); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
"""


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def file_bytes(offset, length):
    """The sparse file's bytes in [offset, offset + length), clipped at its end: zeros, with the markers."""
    end = min(offset + length, SIZE)
    out = bytearray(max(0, end - offset))
    for at, data in MARKERS:
        lo = max(at, offset)
        hi = min(at + len(data), end)
        if lo < hi:
            out[lo - offset:hi - offset] = data[lo - at:hi - at]
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--no-sanitizers", action="store_true",
                        help="build with the default C compiler, without ASan (a host without clang: Windows with cl)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    expected = []
    for offset, length in RANGES:
        sent = file_bytes(offset, length)
        expected.append(f"{0 if offset + length <= SIZE else 1} {len(sent)} {fnv(sent)}")
    calls = "\n".join(f"    out[{2 * k}] = await range(borrow listener, port, file, {o}, {n}, mut out[{2 * k + 1}])?;"
                      for k, (o, n) in enumerate(RANGES))
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-sendfile-") as work:
        data = os.path.join(work, "large")
        with open(data, "wb") as f:
            f.truncate(SIZE)  # sparse: only the markers take space
            for at, marker in MARKERS:
                f.seek(at)
                f.write(marker)
        source = os.path.join(work, "large.cee")
        open(source, "w").write(PROGRAM.replace("@CALLS@", calls))
        exe = os.path.join(work, "large-send")
        flags = ["--cc", SANITIZER_CC, "--cflag", "-fsanitize=address,undefined", "--cflag",
                 "-fno-sanitize-recover=all", "--cflag", "-O1"]
        b = subprocess.run([compiler, "build"] + ([] if args.no_sanitizers else flags) + [source, "-o", exe],
                           capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {b.stderr.strip()[-600:]}")
        else:
            if os.name == "nt" and not exe.endswith(".exe"):
                exe += ".exe"
            env = dict(os.environ, CEESAFE_ALLOC_REPORT="1", CEESAFE_SENDFILE_PATH=data)
            p = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
            m = ALLOC.search(p.stderr)
            got = p.stdout.strip().splitlines()
            if p.returncode != 0 or got != expected or not m or m.group(2) != "0":
                problems.append(f"status {p.returncode}, printed {got!r} (want {expected!r}), "
                                f"stderr {p.stderr.strip()[:300]!r}")
    print(f"sendfile_check: {len(RANGES)} ranges of a {SIZE}-byte sparse file; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
