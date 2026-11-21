#!/usr/bin/env python3
"""Checks of lib/std's deflate codecs (ADR 0196 §4.2, phase 1) that the corpus cannot make: the corpus runs each
program in the interpreter too, so tests/reference/tests/corpus/run-pass/deflate.cee is short. Here, compiled only:

1. the long lists: zlib's streams (Python's zlib, the oracle) of six shapes of data at sizes around every block size
   (1 to 4 bytes, 258, 16 Ki symbols, 32 KiB, 64 KiB, 200 KB), raw, zlib and gzip, at every level, with every
   strategy (Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED), window bits 9 to 15 and sync and full flushes; each decoded
   whole and in steps, its length and CRC-32 compared with zlib's output; the hand-written RFC 1950, 1951 and 1952
   cases of tools/oracles/gen_deflate_cases.py at full size; and every prefix of a few streams, each refused;
2. lib/std's encoder at every level and format, whole and streamed with sync flushes, decoded by zlib to the same
   bytes, with its size reported against zlib's;
3. Adler32 against zlib.adler32, fed whole and in pieces;
4. everything above run with the runtime's allocation report: no allocation may be live at the end;
5. tools/fault_sweep.py's allocation sweep over Inflater.create, Deflater.create, the whole-buffer calls and a step:
   every allocation failure comes back as an error (OutOfMemory) and nothing leaks;
6. with --fuzz-seconds N, the mutational fuzzer tests/fuzz/deflate (ADR 0196 §4.3) built with ASan and UBSan and run
   for N seconds, with the allocation report; it prints how many inputs it tried.

usage: python3 tools/codec_check.py --compiler out/ceesafec [--quick] [--fuzz-seconds N] [--fuzz-seed S]
"""

import argparse
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "oracles"))
import gen_deflate_cases as g  # noqa: E402
from fault_injection import SANITIZER_CC  # noqa: E402  clang, or GCC where there is none

ROOT = os.path.dirname(HERE)

FORMATS = {"raw": 0, "zlib": 1, "gzip": 2}
STATUS = {0: "ok", 1: "Malformed", 2: "TooLarge", 3: "Unsupported", 4: "ChecksumMismatch", 5: "OutOfMemory"}
REPORT = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")


def shapes(n, rng):
    yield "random", bytes(rng.getrandbits(8) for _ in range(n))
    yield "zeros", bytes(n)
    yield "words", g.words(n, 5)
    yield "message", g.message(n, 6)
    c = bytearray()
    while len(c) < n:
        c += b"%d,warehouse/part-%05d.parquet,%032x,STANDARD\n" % (rng.randrange(10 ** 9), rng.randrange(10 ** 5),
                                                                   rng.getrandbits(128))
    yield "csv", bytes(c[:n])
    r = bytearray()
    while len(r) < n:
        r += bytes([rng.randrange(4)]) * rng.randrange(1, 300)
    yield "runs", bytes(r[:n])


def record(op, fmt, level, limit, a, b, data):
    return struct.pack("<BBBBQIII", op, FORMATS[fmt], level, 0, limit, a, b, len(data)) + data


def decode_cases(quick):
    """(label, format, stream, limit, expected, step in, step out)."""
    rng = random.Random(196)
    sizes = [0, 1, 2, 3, 4, 257, 258, 259, 1000, 16383, 16384, 16385, 32768, 65535, 65536, 65537, 200000]
    if quick:
        sizes = [0, 1, 258, 16385, 65537]
    cases = []
    for n in sizes:
        steps = (1, 1) if n <= 16385 else (4093, 1021)
        for shape, data in shapes(n, rng):
            expected = g.ok(data)
            limit = n
            for fmt in FORMATS:
                for level in range(10):
                    s = g.zlib_stream(data, fmt, level)
                    cases.append((f"{shape} {n} {fmt} level {level}", fmt, s, limit, expected) + steps)
                for name, strat in (("filtered", zlib.Z_FILTERED), ("huffman", zlib.Z_HUFFMAN_ONLY),
                                    ("rle", zlib.Z_RLE), ("fixed", zlib.Z_FIXED)):
                    for level in (1, 6):
                        s = g.zlib_stream(data, fmt, level, 15, strat)
                        cases.append((f"{shape} {n} {fmt} {name} {level}", fmt, s, limit, expected) + steps)
                for wbits in (range(9, 16) if fmt == "zlib" else (9, 12)):
                    s = g.zlib_stream(data, fmt, 9, wbits)
                    cases.append((f"{shape} {n} {fmt} window {wbits}", fmt, s, limit, expected) + steps)
                for name, flush in (("sync", zlib.Z_SYNC_FLUSH), ("full", zlib.Z_FULL_FLUSH)):
                    s = g.zlib_stream(data, fmt, 6, 15, 0, flush)
                    cases.append((f"{shape} {n} {fmt} {name} flushes", fmt, s, limit, expected) + steps)
    hand = g.rfc1951_cases(short=False) + g.rfc1950_cases() + g.rfc1952_cases() + g.limit_cases()
    for label, fmt, stream, limit, expected in hand:
        cases.append((label, fmt, stream, limit, expected, 1, 1))
        cases.append((label + " (steps)", fmt, stream, limit, expected, 513, 129))
    # Every prefix of a few streams: each is cut short, so refused (zlib agrees).
    for fmt in FORMATS:
        for data in (g.words(700, 8), g.message(300, 9)):
            s = g.zlib_stream(data, fmt, 6)
            for k in range(len(s)):
                verdict, _ = g.zlib_verdict(s[:k], fmt, 1 << 30)
                assert verdict == "refused"
                cases.append((f"{fmt} prefix {k} of {len(s)}", fmt, s[:k], len(data), "Malformed", 3, 7))
    return cases


def encode_inputs(quick):
    rng = random.Random(1950)
    sizes = [0, 1, 2, 3, 258, 1000, 16384, 65536, 65537, 200000] if not quick else [0, 3, 1000, 65537]
    return [(f"{shape} {n}", data) for n in sizes for shape, data in shapes(n, rng)]


def build(compiler, source, exe, flags):
    p = subprocess.run([compiler, "build", source, "-o", exe] + flags, capture_output=True, text=True)
    if p.returncode != 0:
        print(f"codec_check: the build failed:\n{p.stdout}{p.stderr}")
        return False
    return True


SWEEP = '''// Written by tools/codec_check.py: an allocation sweep (tools/fault_sweep.py) over lib/std's deflate codecs.
fn main() {
    var consistent = true;
    var data = Vector<u8>.create();
    for (k in 0..3000) {
        match (data.push(truncate<u8>((k * 7) % 13 + (k / 100)))) {
            Ok() => {}
            Err(_) => {
                Console.writeLine("consistent");
                return;
            }
        }
    }
    for (level in [0u32, 1, 6]) {
        var packed = Vector<u8>.create();
        match (Gzip.compress(data.view(), level, mut packed)) {
            Ok() => {
                var back = Vector<u8>.create();
                match (Gzip.decompress(packed.view(), 3000, mut back)) {
                    Ok() => { if (back.view() != data.view()) { consistent = false; } }
                    Err(e) => {
                        match (e) {
                            DecodeError.OutOfMemory(_) => { if (back.length != 0) { consistent = false; } }
                            _ => { consistent = false; }
                        }
                    }
                }
                match (Inflater.create(DeflateFormat.Gzip, 3000)) {
                    Ok(inflater) => {
                        var i = inflater;
                        var buffer = [0u8; 4096];
                        match (i.step(packed.view(), mut buffer[..])) {
                            Ok(p) => { if (!p.done || buffer[0..p.written] != data.view()) { consistent = false; } }
                            Err(_) => { consistent = false; }
                        }
                    }
                    Err(_) => {}
                }
            }
            Err(_) => {}
        }
        match (Deflater.create(DeflateFormat.Zlib, level)) {
            Ok(deflater) => {
                var d = deflater;
                var out = Vector<u8>.create();
                let a = d.update(data.view(), mut out);
                let b = d.flush(mut out);
                let c = d.finish(mut out);
            }
            Err(_) => {}
        }
    }
    Console.writeLine(if (consistent) "consistent" else "inconsistent");
}
'''


def fuzz(compiler, work, seconds, seed):
    """Builds tests/fuzz/deflate with the sanitizers and runs it; the number of problems."""
    exe = os.path.join(work, "fuzz_deflate")
    package = os.path.join(ROOT, "tests", "fuzz", "deflate")
    p = subprocess.run([compiler, "build", "-o", exe, "--cc", SANITIZER_CC, "--cflag", "-O1", "--cflag", "-g",
                        "--cflag", "-fsanitize=address,undefined", "--cflag", "-fno-sanitize-recover=all"],
                       cwd=package, capture_output=True, text=True)
    if p.returncode != 0:
        print(f"codec_check: the fuzzer's build failed:\n{p.stdout}{p.stderr}")
        return 1
    env = dict(os.environ)
    env.update({"CEESAFE_FUZZ_SECONDS": str(seconds), "CEESAFE_FUZZ_SEED": str(seed), "CEESAFE_ALLOC_REPORT": "1"})
    ran = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=seconds + 600)
    out = (ran.stdout + ran.stderr).strip()
    m = REPORT.search(out)
    if ran.returncode != 0 or not m or m.group(2) != "0":
        print(f"codec_check: the fuzzer failed (exit status {ran.returncode}):\n{out[-4000:]}")
        return 1
    print("codec_check: " + [line for line in out.splitlines() if line.startswith("fuzz_deflate:")][-1])
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--quick", action="store_true", help="fewer sizes")
    parser.add_argument("--fuzz-seconds", type=int, default=0, help="run the fuzzer this long (0: not at all)")
    parser.add_argument("--fuzz-seed", type=int, default=1)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = 0
    dcases = decode_cases(args.quick)
    einputs = encode_inputs(args.quick)
    adlers = [g.message(n, 3) for n in (0, 1, 5551, 5552, 5553, 11104, 100000)] + [bytes([255]) * 1000000]
    with tempfile.TemporaryDirectory(prefix="ceesafe-codecs-") as work:
        exe = os.path.join(work, "codec_check")
        if not build(compiler, os.path.join(HERE, "codec_check.cee"), exe, []):
            return 1
        blob = bytearray()
        for label, fmt, stream, limit, expected, a, b in dcases:
            blob += record(1, fmt, 0, limit, a, b, stream)
        for label, data in einputs:
            for fmt in FORMATS:
                for level in range(10):
                    piece, every = (7, 5) if len(data) < 1000 else (1000, 7)
                    blob += record(2, fmt, level, 0, piece, every, data)
        for data in adlers:
            blob += record(3, "raw", 0, 0, 0, 0, data)
        cases_path = os.path.join(work, "cases.bin")
        results_path = os.path.join(work, "results.bin")
        with open(cases_path, "wb") as f:
            f.write(blob)
        env = dict(os.environ)
        env["CEESAFE_ALLOC_REPORT"] = "1"
        ran = subprocess.run([exe, cases_path, results_path], capture_output=True, text=True, env=env, timeout=3600)
        if ran.returncode != 0:
            print(f"codec_check: exit status {ran.returncode}:\n{ran.stdout[-2000:]}{ran.stderr[-2000:]}")
            return 1
        m = REPORT.search(ran.stderr + ran.stdout)
        if not m or m.group(2) != "0":
            problems += 1
            print(f"codec_check: the allocation report: {m.group(0) if m else 'none'}")
        with open(results_path, "rb") as f:
            res = f.read()
        at = 0
        wrong = 0
        for label, fmt, stream, limit, expected, a, b in dcases:
            for how in ("whole", "steps"):
                status, length, crc = struct.unpack_from("<BQI", res, at)
                at += 13
                got = f"ok {length} {crc}" if status == 0 else STATUS[status]
                if got != expected:
                    wrong += 1
                    if wrong <= 10:
                        print(f"codec_check: {label} ({how}): expected {expected}, got {got}")
        ours = [0] * 10
        theirs = [0] * 10
        for label, data in einputs:
            for fmt in FORMATS:
                wb = {"raw": -15, "zlib": 15, "gzip": 31}[fmt]
                for level in range(10):
                    for how in ("whole", "streamed"):
                        (n,) = struct.unpack_from("<I", res, at)
                        packed = res[at + 4:at + 4 + n]
                        at += 4 + n
                        try:
                            back = zlib.decompress(packed, wb)
                        except zlib.error as e:
                            back = str(e)
                        if back != data:
                            wrong += 1
                            if wrong <= 10:
                                print(f"codec_check: our {fmt} level {level} of {label} ({how}): zlib reads "
                                      f"{back if isinstance(back, str) else len(back)}")
                        if how == "whole" and fmt == "raw":
                            ours[level] += n
                            theirs[level] += len(g.zlib_stream(data, "raw", level))
        for data in adlers:
            v1, v2 = struct.unpack_from("<II", res, at)
            at += 8
            if v1 != zlib.adler32(data) or v2 != v1:
                wrong += 1
                print(f"codec_check: Adler32 of {len(data)} bytes: {v1} and {v2}, zlib {zlib.adler32(data)}")
        problems += wrong
        print("codec_check: lib/std's raw deflate output against zlib's, all inputs: " +
              ", ".join(f"{lv} {ours[lv] / max(1, theirs[lv]):.3f}" for lv in range(10)))
        # The allocation sweep.
        sweep = os.path.join(work, "codec_sweep.cee")
        with open(sweep, "w", encoding="utf-8", newline="\n") as f:
            f.write(SWEEP)
        swept = subprocess.run([sys.executable, os.path.join(HERE, "fault_sweep.py"), "--compiler", compiler, sweep],
                               capture_output=True, text=True)
        tail = (swept.stdout + swept.stderr).strip().splitlines()
        if swept.returncode != 0:
            problems += 1
            print("codec_check: the allocation sweep failed:\n" + "\n".join(tail[-20:]))
        sweep_line = tail[-1] if tail else ""
        if args.fuzz_seconds > 0:
            problems += fuzz(compiler, work, args.fuzz_seconds, args.fuzz_seed)
    encodes = len(einputs) * 3 * 10 * 2
    print(f"codec_check: {len(dcases)} streams decoded whole and in steps, {encodes} encodings read back by zlib, "
          f"{len(adlers)} Adler-32s, leak check and allocation sweep ({sweep_line}): {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
