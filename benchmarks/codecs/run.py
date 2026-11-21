#!/usr/bin/env python3
"""lib/std's deflate codecs against the system's zlib (ADR 0196 §4.4), on the same machine.

Three inputs of 32 MiB, made here from a fixed seed: an inventory-shaped CSV with a random 128-bit hex column (ADR
0196 §1's), a Parquet-like column of 64-bit integers (sorted timestamps, little-endian), and text of words. Each is
compressed by Python's zlib at levels 1, 6 and 9 into zlib streams that both decoders read (main.cee, lib/std's
Zlib.decompress; main.c, zlib's uncompress), and both encoders compress it at those levels (Zlib.compress, zlib's
compress2). Every figure is the best of --runs runs, in MB/s (10^6 bytes of the uncompressed data per second);
sizes are the compressed bytes. The results are recorded in guide/performance.md.

usage: python3 benchmarks/codecs/run.py [--ceesafec out/ceesafec] [--cc cc] [--runs 3] [--size 33554432]
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


def csv(n, rng):
    out = bytearray(b"bucket,key,size,last_modified,e_tag,storage_class,version_id\n")
    classes = [b"STANDARD", b"STANDARD_IA", b"GLACIER"]
    i = 0
    while len(out) < n:
        i += 1
        out += (b"analytics-prod,warehouse/events/date=2026-%02d-%02d/part-%05d-%08x.parquet,%d,"
                b"2026-%02d-%02dT%02d:%02d:%02d.000Z,%032x,%s,%016x\n") % (
            rng.randrange(1, 13), rng.randrange(1, 29), i % 100000, rng.getrandbits(32), rng.randrange(10 ** 3, 10 ** 9),
            rng.randrange(1, 13), rng.randrange(1, 29), rng.randrange(24), rng.randrange(60), rng.randrange(60),
            rng.getrandbits(128), rng.choice(classes), rng.getrandbits(64))
    return bytes(out[:n])


def integers(n, rng):
    out = bytearray()
    t = 1_763_700_000_000_000
    while len(out) < n:
        t += rng.choice((0, 1000, 1000, 2000, rng.randrange(1, 1 << 20)))
        out += struct.pack("<q", t)
    return bytes(out[:n])


def text(n, rng):
    words = [b"the", b"of", b"and", b"a", b"to", b"in", b"is", b"storage", b"object", b"bucket", b"table", b"page",
             b"compression", b"metadata", b"snapshot", b"partition", b"manifest", b"column", b"row", b"value"]
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words)
        out += b"." if rng.randrange(12) == 0 else b" "
        if rng.randrange(80) == 0:
            out += b"\n"
    return bytes(out[:n])


def run(argv):
    p = subprocess.run(argv, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"codecs: {' '.join(argv)} failed:\n{p.stdout}{p.stderr}")
    speed, size = p.stdout.split()
    return int(speed), int(size)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ceesafec", default=os.path.join(ROOT, "out", "ceesafec"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--size", type=int, default=32 << 20)
    args = parser.parse_args()
    rng = random.Random(196)
    with tempfile.TemporaryDirectory(prefix="ceesafe-codecs-bench-") as work:
        cee = os.path.join(work, "cee")
        c = os.path.join(work, "c")
        b = subprocess.run([args.ceesafec, "build", os.path.join(HERE, "main.cee"), "-o", cee], capture_output=True,
                           text=True)
        if b.returncode != 0:
            sys.exit(f"codecs: the CeeSafe build failed:\n{b.stdout}{b.stderr}")
        subprocess.run([args.cc, "-O2", os.path.join(HERE, "main.c"), "-o", c, "-lz"], check=True)
        print(f"zlib {zlib.ZLIB_RUNTIME_VERSION} (Python's, for the streams); best of {args.runs}; MB/s of "
              f"uncompressed data")
        print("| input | level | zlib size | lib/std size | inflate zlib | inflate lib/std | deflate zlib | "
              "deflate lib/std |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|")
        for name, make in (("CSV", csv), ("integers", integers), ("text", text)):
            data = make(args.size, rng)
            path = os.path.join(work, name)
            with open(path, "wb") as f:
                f.write(data)
            for level in (1, 6, 9):
                stream = zlib.compress(data, level)
                zpath = f"{path}.{level}.zlib"
                with open(zpath, "wb") as f:
                    f.write(stream)
                zi, _ = run([c, "inflate", zpath, str(len(data)), str(args.runs)])
                ci, _ = run([cee, "inflate", zpath, str(len(data)), str(args.runs)])
                zd, zsize = run([c, "deflate", path, str(level), str(args.runs)])
                cd, csize = run([cee, "deflate", path, str(level), str(args.runs)])
                print(f"| {name} | {level} | {zsize:,} | {csize:,} | {zi:,} | {ci:,} | {zd:,} | {cd:,} |", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
