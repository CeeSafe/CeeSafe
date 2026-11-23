#!/usr/bin/env python3
"""Checks of lib/std's codecs (ADR 0196 §4.2: deflate, zlib and gzip in phase 1, snappy in phase 2) that the corpus
cannot make: the corpus runs each program in the interpreter too, so tests/reference/tests/corpus/run-pass/deflate.cee
and snappy.cee are short. Here, compiled only:

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
6. with --fuzz-seconds N, the mutational fuzzers tests/fuzz/deflate and tests/fuzz/snappy (ADR 0196 §4.3) built with
   ASan and UBSan and run for N seconds each, with the allocation report; each prints how many inputs it tried;
7. snappy: blocks written by libsnappy and by cramjam where they are installed, and by tools/oracles/snappy_model.py
   (every kind of element) always, of the six shapes at sizes around the 64 KiB fragment up to 1 MiB, with limits
   at, below and above the length; the hand-written refusals of tools/oracles/gen_snappy_cases.py; every prefix of a
   few blocks; and mutants of them all. The expected verdict is the model's, and each library's is compared with it
   (libsnappy must agree; cramjam's differences are counted and printed). lib/std's own blocks of every input are
   read back by the model and the libraries, and their size is reported against libsnappy's; Snappy.length against
   the model's preamble.

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
import gen_snappy_cases as gs  # noqa: E402
import snappy_model as sm  # noqa: E402
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


def snappy_decode_cases(libs, quick):
    """(label, block, limit, expected, mutant): the expected verdict is the model's."""
    rng = random.Random(2)
    sizes = [0, 1, 2, 3, 4, 14, 15, 16, 17, 100, 1000, 16384, 65535, 65536, 65537, 131072, 200000, 1 << 20]
    if quick:
        sizes = [0, 1, 15, 1000, 65537]
    blocks = []
    for n in sizes:
        for shape, data in shapes(n, rng):
            for lib in libs:
                blocks.append((f"{lib.path} {shape} {n}", lib.compress(data), data))
            blocks.append((f"model {shape} {n}", sm.variety(data, n), data))
    cases = []
    for label, block, data in blocks:
        n = len(data)
        cases.append((label, block, n, sm.verdict(block, n), False))
        if n > 0 and n % 3 == 0:
            cases.append((label + " limit - 1", block, n - 1, "TooLarge", False))
        if n % 5 == 1:
            cases.append((label + " limit + 1000", block, n + 1000, sm.verdict(block, n + 1000), False))
    for label, block, limit, refused in gs.hand_cases():
        cases.append(("hand " + label, block, limit, sm.verdict(block, limit), False))
    for label, block, data in blocks:
        if len(data) in (100, 1000) and ("words" in label or "runs" in label):
            for k in range(len(block)):
                cases.append((f"{label} prefix {k}", block[:k], len(data), sm.verdict(block[:k], len(data)), True))
    seeds = [b for b in blocks if len(b[2]) <= 65537]
    for k in range(3000 if quick else 30000):
        label, block, data = seeds[rng.randrange(len(seeds))]
        mutant = sm.hostile(rng, block)
        limit = len(data) if rng.random() < 0.7 else rng.randrange(0, 2 * len(data) + 2)
        cases.append((f"{label} mutant {k}", mutant, limit, sm.verdict(mutant, limit), True))
    return cases


def snappy_encode_inputs(quick):
    rng = random.Random(1951)
    sizes = [0, 1, 2, 3, 4, 14, 15, 16, 17, 258, 1000, 65535, 65536, 65537, 200000, 1 << 20]
    if quick:
        sizes = [0, 15, 1000, 65537]
    return [(f"{shape} {n}", data) for n in sizes for shape, data in shapes(n, rng)]


def snappy_oracles(libs, cases):
    """Each library's verdict on every case's block against the model's: (agreements, disagreements) per library."""
    tally = {}
    for lib in libs:
        agree = differ = 0
        for label, block, limit, expected, mutant in cases:
            if sm.decode(block) == lib.decompress(block):
                agree += 1
            else:
                differ += 1
                if differ <= 3:
                    print(f"codec_check: {lib.path} and the model differ on {label}")
        tally[lib.path] = (agree, differ)
    return tally


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


SWEEP = '''// Written by tools/codec_check.py: an allocation sweep (tools/fault_sweep.py) over lib/std's codecs.
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
    var sp = Vector<u8>.create();
    match (Snappy.compress(data.view(), 0, mut sp)) {
        Ok() => {
            var back = Vector<u8>.create();
            match (Snappy.decompress(sp.view(), 3000, mut back)) {
                Ok() => { if (back.view() != data.view()) { consistent = false; } }
                Err(e) => {
                    match (e) {
                        DecodeError.OutOfMemory(_) => { if (back.length != 0) { consistent = false; } }
                        _ => { consistent = false; }
                    }
                }
            }
        }
        Err(_) => { if (sp.length != 0) { consistent = false; } }
    }
    Console.writeLine(if (consistent) "consistent" else "inconsistent");
}
'''


def fuzz(compiler, work, seconds, seed, codec):
    """Builds tests/fuzz/<codec> with the sanitizers and runs it; the number of problems."""
    exe = os.path.join(work, "fuzz_" + codec)
    package = os.path.join(ROOT, "tests", "fuzz", codec)
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
    print("codec_check: " + [line for line in out.splitlines() if line.startswith(f"fuzz_{codec}:")][-1])
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
    slibs = sm.oracles()
    scases = snappy_decode_cases(slibs, args.quick)
    sinputs = snappy_encode_inputs(args.quick)
    stally = snappy_oracles(slibs, scases)
    for lib in slibs:
        agree, differ = stally[lib.path]
        print(f"codec_check: snappy, {lib.path} against the model: {agree} blocks agree, {differ} differ")
        if isinstance(lib, sm.LibSnappy):
            problems += differ  # the reference implementation; cramjam's differences are reported only
    if not slibs:
        print("codec_check: snappy, no library installed (libsnappy, cramjam): the model is the only oracle")
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
        for label, block, limit, expected, mutant in scases:
            blob += record(4, "raw", 0, limit, 0, 0, block)
        for label, data in sinputs:
            blob += record(5, "raw", 6, 0, 0, 0, data)
        for label, block, limit, expected, mutant in scases:
            blob += record(6, "raw", 0, 0, 0, 0, block)
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
        swrong = 0
        STATUS6 = dict(STATUS)
        STATUS6[6] = "output disturbed"
        for label, block, limit, expected, mutant in scases:
            status, length, crc = struct.unpack_from("<BQI", res, at)
            at += 13
            got = f"ok {length} {crc}" if status == 0 else STATUS6[status]
            if got != expected:
                swrong += 1
                if swrong <= 10:
                    print(f"codec_check: snappy {label}: expected {expected}, got {got}")
        sours = stheirs = same = 0
        for label, data in sinputs:
            (n,) = struct.unpack_from("<I", res, at)
            packed = bytes(res[at + 4:at + 4 + n])
            at += 4 + n
            readers = [("the model", sm.decode(packed))] + [(lib.path, lib.decompress(packed)) for lib in slibs]
            for who, back in readers:
                if back != data:
                    swrong += 1
                    if swrong <= 10:
                        print(f"codec_check: our snappy block of {label}: {who} reads "
                              f"{'nothing' if back is None else len(back)}")
            if n > 32 + len(data) + len(data) // 6:
                swrong += 1
                print(f"codec_check: our snappy block of {label} is {n} bytes, above the bound")
            sours += n
            if slibs:
                reference = slibs[0].compress(data)
                stheirs += len(reference)
                same += reference == packed
        for label, block, limit, expected, mutant in scases:
            present, value = struct.unpack_from("<BQ", res, at)
            at += 9
            p = sm.preamble(block)
            if (p is None) != (present == 0) or (p is not None and p[0] != value):
                swrong += 1
                if swrong <= 10:
                    print(f"codec_check: Snappy.length of {label}: {value if present else 'none'}, model {p}")
        wrong += swrong
        if slibs:
            print(f"codec_check: lib/std's snappy output against {slibs[0].path}'s: {sours / max(1, stheirs):.4f} "
                  f"of its size, {same} of {len(sinputs)} blocks byte for byte the same")
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
            for codec in ("deflate", "snappy"):
                problems += fuzz(compiler, work, args.fuzz_seconds, args.fuzz_seed, codec)
    encodes = len(einputs) * 3 * 10 * 2
    mutants = sum(1 for c in scases if c[4])
    print(f"codec_check: {len(dcases)} streams decoded whole and in steps, {encodes} encodings read back by zlib, "
          f"{len(adlers)} Adler-32s; snappy: {len(scases)} blocks decoded ({mutants} of them prefixes and mutants), "
          f"{len(sinputs)} encodings read back by the model{' and ' + str(len(slibs)) + ' libraries' if slibs else ''}"
          f"; leak check and allocation sweep ({sweep_line}): {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
