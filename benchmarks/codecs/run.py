#!/usr/bin/env python3
"""lib/std's deflate and snappy codecs against the system's zlib and libsnappy (ADR 0196 §4.4), on the same machine.

Three inputs of 32 MiB, made here from a fixed seed: an inventory-shaped CSV with a random 128-bit hex column (ADR
0196 §1's), a Parquet-like column of 64-bit integers (sorted timestamps, little-endian), and text of words. Each is
compressed by Python's zlib at levels 1, 6 and 9 into zlib streams that both decoders read (main.cee, lib/std's
Zlib.decompress; main.c, zlib's uncompress), and both encoders compress it at those levels (Zlib.compress, zlib's
compress2). Every figure is the best of --runs runs, in MB/s (10^6 bytes of the uncompressed data per second);
sizes are the compressed bytes. Then snappy, where libsnappy is installed (snappy.c, built with --snappy-prefix's
include and lib directories): both decoders read libsnappy's block of each input (Snappy.decompress;
snappy_uncompress), and both encoders compress it (Snappy.compress; snappy_compress). Then zstd, where libzstd is
installed (zstd.c, with --zstd-prefix): both decoders read libzstd's frames of each input at levels 1, 3 and 19
(Zstd.decompress; ZSTD_decompressDCtx), and both encoders compress each input at levels 1, 3, 6 and 9
(Zstd.compress; ZSTD_compressCCtx). The results are recorded in guide/performance.md.

usage: python3 benchmarks/codecs/run.py [--ceesafec out/ceesafec] [--cc cc] [--runs 3] [--size 33554432]
                                        [--snappy-prefix /opt/homebrew] [--zstd-prefix /opt/homebrew]
                                        [--only zlib|snappy|zstd]
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
sys.path.insert(0, os.path.join(ROOT, "tools", "oracles"))
import snappy_model  # noqa: E402  libsnappy through ctypes, for the blocks both decoders read
import zstd_model  # noqa: E402  libzstd through ctypes, for the frames both decoders read


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
    parser.add_argument("--snappy-prefix", default="/opt/homebrew" if os.path.isdir("/opt/homebrew") else "/usr")
    parser.add_argument("--zstd-prefix", default="/opt/homebrew" if os.path.isdir("/opt/homebrew") else "/usr")
    parser.add_argument("--only", choices=("zlib", "snappy", "zstd"))
    args = parser.parse_args()
    rng = random.Random(196)
    with tempfile.TemporaryDirectory(prefix="ceesafe-codecs-bench-") as work:
        cee = os.path.join(work, "cee")
        c = os.path.join(work, "c")
        b = subprocess.run([args.ceesafec, "build", os.path.join(HERE, "main.cee"), "-o", cee], capture_output=True,
                           text=True)
        if b.returncode != 0:
            sys.exit(f"codecs: the CeeSafe build failed:\n{b.stdout}{b.stderr}")
        inputs = [(name, make(args.size, rng)) for name, make in (("CSV", csv), ("integers", integers), ("text", text))]
        paths = {}
        for name, data in inputs:
            paths[name] = os.path.join(work, name)
            with open(paths[name], "wb") as f:
                f.write(data)
        if args.only not in ("snappy", "zstd"):
            subprocess.run([args.cc, "-O2", os.path.join(HERE, "main.c"), "-o", c, "-lz"], check=True)
            print(f"zlib {zlib.ZLIB_RUNTIME_VERSION} (Python's, for the streams); best of {args.runs}; MB/s of "
                  f"uncompressed data")
            print("| input | level | zlib size | lib/std size | inflate zlib | inflate lib/std | deflate zlib | "
                  "deflate lib/std |")
            print("|---|---:|---:|---:|---:|---:|---:|---:|")
            for name, data in inputs:
                path = paths[name]
                for level in (1, 6, 9):
                    stream = zlib.compress(data, level)
                    zpath = f"{path}.{level}.zlib"
                    with open(zpath, "wb") as f:
                        f.write(stream)
                    zi, _ = run([c, "inflate", zpath, str(len(data)), str(args.runs)])
                    ci, _ = run([cee, "inflate", zpath, str(len(data)), str(args.runs)])
                    zd, zsize = run([c, "deflate", path, str(level), str(args.runs)])
                    cd, csize = run([cee, "deflate", path, str(level), str(args.runs)])
                    print(f"| {name} | {level} | {zsize:,} | {csize:,} | {zi:,} | {ci:,} | {zd:,} | {cd:,} |",
                          flush=True)
        lib = snappy_model.libsnappy()
        if args.only == "zlib":
            return 0
        if lib is None:
            print("codecs: no libsnappy, so no snappy table")
        elif args.only != "zstd":
            snappy_table(args, cee, work, inputs, paths, lib)
        if args.only == "snappy":
            return 0
        zlib_ = zstd_model.libzstd()
        if zlib_ is None:
            print("codecs: no libzstd, so no zstd table")
            return 0
        z = os.path.join(work, "zstd")
        subprocess.run([args.cc, "-O2", os.path.join(HERE, "zstd.c"), "-o", z, f"-I{args.zstd_prefix}/include",
                        f"-L{args.zstd_prefix}/lib", "-lzstd"], check=True)
        print(f"libzstd {zlib_.version} ({zlib_.path}); best of {args.runs}; MB/s of uncompressed data")
        print("| input | level | size | decode libzstd | decode lib/std | lib/std / libzstd |")
        print("|---|---:|---:|---:|---:|---:|")
        for name, data in inputs:
            path = paths[name]
            for level in (1, 3, 19):
                zpath = f"{path}.{level}.zst"
                with open(zpath, "wb") as f:
                    f.write(zlib_.compress(data, level))
                zi, zsize = run([z, "unzstd", zpath, str(len(data)), str(args.runs)])
                ci, _ = run([cee, "unzstd", zpath, str(len(data)), str(args.runs)])
                print(f"| {name} | {level} | {zsize:,} | {zi:,} | {ci:,} | {ci / zi:.0%} |", flush=True)
        print(f"libzstd {zlib_.version}; best of {args.runs}; MB/s of uncompressed data; sizes in bytes")
        print("| input | level | libzstd size | lib/std size | lib/std / libzstd | encode libzstd | encode lib/std | "
              "lib/std / libzstd |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|")
        for name, data in inputs:
            path = paths[name]
            for level in (1, 3, 6, 9):
                zd, zsize = run([z, "zstd", path, str(level), str(args.runs)])
                cd, csize = run([cee, "zstd", path, str(level), str(args.runs)])
                print(f"| {name} | {level} | {zsize:,} | {csize:,} | {csize / zsize:.3f} | {zd:,} | {cd:,} | "
                      f"{cd / zd:.0%} |", flush=True)
    return 0


def snappy_table(args, cee, work, inputs, paths, lib):
    """libsnappy's and lib/std's snappy on each input: sizes, and decoding and encoding speeds."""
    s = os.path.join(work, "snappy")
    subprocess.run([args.cc, "-O2", os.path.join(HERE, "snappy.c"), "-o", s, f"-I{args.snappy_prefix}/include",
                    f"-L{args.snappy_prefix}/lib", "-lsnappy"], check=True)
    print(f"libsnappy ({lib.path}); best of {args.runs}; MB/s of uncompressed data")
    print("| input | libsnappy size | lib/std size | decode libsnappy | decode lib/std | encode libsnappy | "
          "encode lib/std |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for name, data in inputs:
        path = paths[name]
        spath = f"{path}.snappy"
        with open(spath, "wb") as f:
            f.write(lib.compress(data))
        zi, _ = run([s, "unsnappy", spath, str(len(data)), str(args.runs)])
        ci, _ = run([cee, "unsnappy", spath, str(len(data)), str(args.runs)])
        zd, zsize = run([s, "snappy", path, "0", str(args.runs)])
        cd, csize = run([cee, "snappy", path, "0", str(args.runs)])
        print(f"| {name} | {zsize:,} | {csize:,} | {zi:,} | {ci:,} | {zd:,} | {cd:,} |", flush=True)


if __name__ == "__main__":
    sys.exit(main())
