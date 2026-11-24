#!/usr/bin/env python3
"""The float methods of ADR 0218, built and interpreted, compared bit for bit (testing.md, "Interpreter agreement").

A libm's results are the platform's, so the corpus (run-pass/float-libm*.cee) can hold only results that every libm
gives, and checks the rest within some ulps. This sweep checks what the corpus cannot: on this machine, a program
built with ceesafec and the same program under `ceesafec run --interp` give the same bits for every method, on
seeded random operands of every magnitude (random bit patterns, NaNs and infinities included), in f64 and f32.

usage: python3 tools/oracles/float_math_sweep.py --ceesafec out/ceesafec [--count N]
Prints the number of results compared and every difference; exit status 1 if there is one.
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile

UNARY = ["exp", "exp2", "expM1", "ln", "log2", "log10", "ln1p", "sin", "cos", "tan", "asin", "acos", "atan", "sinh",
         "cosh", "tanh", "asinh", "acosh", "atanh", "cbrt", "logB", "trunc", "round", "roundEven"]
BINARY = ["pow", "atan2", "hypot", "remainder", "truncatingRemainder", "copySign"]


def operands64(rng, count):
    out = []
    for k in range(count):
        if k % 3 == 0:
            out.append(rng.getrandbits(64))  # any pattern: subnormals, huge values, NaNs, infinities
        else:
            # A small exponent, where the functions do their work.
            out.append((rng.getrandbits(1) << 63) | (rng.randrange(1003, 1045) << 52) | rng.getrandbits(52))
    return out


def operands32(rng, count):
    out = []
    for k in range(count):
        if k % 3 == 0:
            out.append(rng.getrandbits(32))
        else:
            out.append((rng.getrandbits(1) << 31) | (rng.randrange(112, 142) << 23) | rng.getrandbits(23))
    return out


def program(count):
    """Constant arrays of operands and one loop per type (a statement per result made a `main` that clang -O2
    took more than ten minutes over)."""
    rng = random.Random(20261124)
    lines = []
    for t, u, width, make in (("f64", "u64", 16, operands64), ("f32", "u32", 8, operands32)):
        for name in ("X", "Y", "Z"):
            values = ", ".join(f"0x{v:0{width}X}" for v in make(rng, count))
            lines.append(f"const {name}{t.upper()}: [{u}; {count}] = [{values}];")
        powers = ", ".join(str(rng.randrange(-1200, 1200)) for _ in range(count))
        lines.append(f"const N{t.upper()}: [i32; {count}] = [{powers}];")
        lines.append(f"fn sweep{t}() {{")
        lines.append(f"    for (k in 0..{count}) {{")
        lines.append(f"        let x = {t}.fromBits(X{t.upper()}[k]);")
        lines.append(f"        let y = {t}.fromBits(Y{t.upper()}[k]);")
        lines.append(f"        let z = {t}.fromBits(Z{t.upper()}[k]);")
        for m in UNARY:
            lines.append(f"        Console.writeLine(x.{m}().bits());")
        for m in BINARY:
            lines.append(f"        Console.writeLine(x.{m}(y).bits());")
        lines.append("        Console.writeLine(x.fma(y, z).bits());")
        lines.append(f"        Console.writeLine(x.scaleB(N{t.upper()}[k]).bits());")
        lines.append("    }")
        lines.append("}")
    lines.append("fn main() {\n    sweepf64();\n    sweepf32();\n}")
    return "\n".join(lines) + "\n"


def nan(bits, width):
    exponent = (1 << (11 if width == 64 else 8)) - 1
    shift = 52 if width == 64 else 23
    return (bits >> shift) & exponent == exponent and bits & ((1 << shift) - 1) != 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ceesafec", required=True)
    parser.add_argument("--count", type=int, default=150)
    args = parser.parse_args()
    cee = os.path.abspath(args.ceesafec)
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "sweep.cee")
        with open(source, "w", encoding="utf-8") as f:
            f.write(program(args.count))
        exe = os.path.join(work, "sweep")
        subprocess.run([cee, "build", "-o", exe, source], check=True, capture_output=True)
        built = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split("\n")
        interp = subprocess.run([cee, "run", "--interp", source], check=True, capture_output=True,
                                text=True).stdout.split("\n")
    per = len(UNARY) + len(BINARY) + 2
    differences = 0
    nans = 0
    for k, (a, b) in enumerate(zip(built, interp)):
        if a == b:
            continue
        width = 64 if k < args.count * per else 32
        if a and b and nan(int(a), width) and nan(int(b), width):
            nans += 1
            continue  # NaN bits are not part of the agreement (type-system.md §35.3)
        differences += 1
        print(f"line {k + 1}: built {a}, interpreted {b}")
    compared = len(built) - 1
    print(f"{compared} results compared ({args.count} operands per type), {differences} differences, "
          f"{nans} NaNs whose payloads differ")
    return 1 if differences or len(built) != len(interp) else 0


if __name__ == "__main__":
    sys.exit(main())
