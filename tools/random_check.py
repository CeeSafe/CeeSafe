#!/usr/bin/env python3
"""Checks of lib/std's Random.fill (ADR 0060) that the corpus cannot make, since the corpus runs each program natively
and in the interpreter and the two answer different bytes:

1. natively, 1 MiB of bytes counted by value pass a chi-squared test against the uniform distribution (255 degrees of
   freedom; the bound 400 is exceeded by uniform bytes with probability below 10^-8), and two runs' first 16 bytes
   differ;
2. natively, the cost of one 4-byte fill (a WebSocket frame's mask), as the mean of 100 000 calls, is reported and must
   stay below 20 microseconds;
3. in the interpreter, the first 24 bytes are SplitMix64's from the fixed seed 0x2b7e151628aed2a6, computed here.

usage: python3 tools/random_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

NATIVE = """
fn main() {
    var big = Vector<u8>.create();
    for (k in 0..1048576usize) {
        big.push(0)!;
    }
    Random.fill(big.viewMut())!;
    var counts = [0u64; 256];
    for (x in big.view()) {
        counts[x as usize] += 1;
    }
    for (k in 0..256usize) {
        Console.write(counts[k]);
        Console.write(" ");
    }
    Console.writeLine("");
    var first = [0u8; 16];
    Random.fill(mut first[..])!;
    for (b in first) {
        Console.write(b);
        Console.write(" ");
    }
    Console.writeLine("");
    var mask = [0u8; 4];
    let start = Timer.now();
    for (k in 0..100000) {
        Random.fill(mut mask[..])!;
    }
    Console.writeLine(Timer.now() - start);
}
"""

INTERPRETED = """
fn main() {
    var first = [0u8; 24];
    Random.fill(mut first[0..5])!;
    Random.fill(mut first[5..24])!;
    for (b in first) {
        Console.write(b);
        Console.write(" ");
    }
    Console.writeLine("");
}
"""

MASK = (1 << 64) - 1


def splitmix_bytes(seed, count, calls):
    """The interpreter's sequence: each call takes whole 64-bit words, low byte first, and drops what is left over."""
    state, out = seed, []
    for length in calls:
        k = 0
        while k < length:
            state = (state + 0x9E3779B97F4A7C15) & MASK
            z = state
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
            z ^= z >> 31
            b = 0
            while b < 8 and k < length:
                out.append((z >> (8 * b)) & 0xFF)
                b += 1
                k += 1
    assert len(out) == count
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cost = None
    with tempfile.TemporaryDirectory(prefix="ceesafe-random-") as work:
        source = os.path.join(work, "native.cee")
        open(source, "w").write(NATIVE)
        exe = os.path.join(work, "native")
        b = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {b.stderr.strip()[-400:]}")
        else:
            if os.name == "nt" and not exe.endswith(".exe"):
                exe += ".exe"
            firsts = []
            for run in range(2):
                p = subprocess.run([exe], capture_output=True, text=True, timeout=300)
                lines = p.stdout.strip().splitlines()
                if p.returncode != 0 or len(lines) != 3:
                    problems.append(f"native run {run}: status {p.returncode}, printed {p.stdout[-200:]!r}")
                    continue
                counts = [int(x) for x in lines[0].split()]
                expected = 1048576 / 256
                chi = sum((c - expected) ** 2 / expected for c in counts)
                if len(counts) != 256 or sum(counts) != 1048576 or chi > 400:
                    problems.append(f"native run {run}: chi-squared {chi:.1f} over {len(counts)} values")
                firsts.append(lines[1])
                ms = int(lines[2])
                cost = ms * 1000.0 / 100000  # microseconds per call
                if cost > 20:
                    problems.append(f"native run {run}: a 4-byte fill costs {cost:.2f} us")
            if len(firsts) == 2 and firsts[0] == firsts[1]:
                problems.append(f"native: two runs began with the same 16 bytes {firsts[0]}")
        source = os.path.join(work, "interpreted.cee")
        open(source, "w").write(INTERPRETED)
        p = subprocess.run([compiler, "run", "--interp", source], capture_output=True, text=True, timeout=120)
        want = " ".join(str(x) for x in splitmix_bytes(0x2B7E151628AED2A6, 24, [5, 19]))
        if p.stdout.strip() != want:
            problems.append(f"interpreter: printed {p.stdout.strip()!r}, want {want!r}")
    shown = f"{cost:.3f} us per 4-byte fill" if cost is not None else "no cost measured"
    print(f"random_check: the native bytes' distribution, {shown}, the interpreter's sequence; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
