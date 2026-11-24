#!/usr/bin/env python3
"""The float methods of ADR 0218 are the system's libm, whatever C compiler built the program (ADR 0248).

One program calls every libm method (`exp` … `scaleB`, in f64 and f32) on the SQLite port's arguments (finding 50:
its sweeps of 400 and 200 rows, its special values, `pow(2, -1075)` among them) and on seeded random operands of
every magnitude. It is built once with each C compiler given (`--cc`, repeatable; the default is ceesafec's own
choice) and run under `ceesafec run --interp`, and every result is compared bit for bit:

1. with the system's libm, called through Python's ctypes: ucrtbase.dll on Windows, libSystem on macOS, libm
   elsewhere. A difference is a problem;
2. between the builds and the interpreter (the same comparison, transitively). On Windows the interpreter's
   differences are information, not problems: it calls the C library ceesafec was linked with (ADR 0248 §2.3);
3. for f64 results whose operands and result are finite, with Python's math module, which calls the same libm
   (CPython on Windows links ucrtbase.dll): a second witness, a difference is a problem;
4. on Windows with `cl` among the compilers, as information only, with a C program that calls the functions
   directly and is built by `cl` with its default static C runtime (the SDK's copy of the UCRT, what sqlite.org's
   sqlite3.exe has): how many of its results differ from ucrtbase.dll's on this machine.

NaN results agree whatever their payloads (type-system.md §35.3).

usage: python3 tools/libm_check.py --compiler out/ceesafec [--cc cl --cc gcc] [--count N]
"""

import argparse
import ctypes
import ctypes.util
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

# (method, C name, operands): operands 1, 2 or 3 floats, or "n" for scaleB's (float, i32).
METHODS = [
    ("exp", "exp", 1), ("exp2", "exp2", 1), ("expM1", "expm1", 1), ("ln", "log", 1), ("log2", "log2", 1),
    ("log10", "log10", 1), ("ln1p", "log1p", 1), ("sin", "sin", 1), ("cos", "cos", 1), ("tan", "tan", 1),
    ("asin", "asin", 1), ("acos", "acos", 1), ("atan", "atan", 1), ("sinh", "sinh", 1), ("cosh", "cosh", 1),
    ("tanh", "tanh", 1), ("asinh", "asinh", 1), ("acosh", "acosh", 1), ("atanh", "atanh", 1), ("cbrt", "cbrt", 1),
    ("logB", "logb", 1), ("pow", "pow", 2), ("atan2", "atan2", 2), ("hypot", "hypot", 2),
    ("remainder", "remainder", 2), ("truncatingRemainder", "fmod", 2), ("fma", "fma", 3), ("scaleB", "scalbn", "n"),
]
CODE = {m: k for k, (m, _, _) in enumerate(METHODS)}
ARITY = {m: a for m, _, a in METHODS}
CNAME = {m: c for m, c, _ in METHODS}

# Python's math functions that call the libm function of the same meaning for finite operands (CPython's
# mathmodule.c); remainder is CPython's own, exact like libm's.
PYTHON = {"exp": "exp", "expM1": "expm1", "ln": "log", "log2": "log2", "log10": "log10", "ln1p": "log1p",
          "sin": "sin", "cos": "cos", "tan": "tan", "asin": "asin", "acos": "acos", "atan": "atan", "sinh": "sinh",
          "cosh": "cosh", "tanh": "tanh", "asinh": "asinh", "acosh": "acosh", "atanh": "atanh", "pow": "pow",
          "atan2": "atan2", "remainder": "remainder", "truncatingRemainder": "fmod", "exp2": "exp2",
          "cbrt": "cbrt", "fma": "fma"}


def bits64(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def bits32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def from64(b):
    return struct.unpack("<d", struct.pack("<Q", b))[0]


def from32(b):
    return struct.unpack("<f", struct.pack("<I", b))[0]


def cases(count):
    """[(type, method, x bits, y bits, z bits, n)]."""
    out = []

    def f64(method, x, y=0.0, z=0.0, n=0):
        out.append(("f64", method, bits64(x), bits64(y), bits64(z), n))

    # The SQLite port's sweep (mathFunctionsAgreeWithTheShellOnManyArguments): x = (i * 7919 % 2003 - 1001) / 97.3.
    for i in range(1, 401):
        x = (i * 7919 % 2003 - 1001) / 97.3
        for m, a in (("sin", x), ("cos", x), ("tan", x), ("atan", x), ("exp", x / 4), ("asin", x / 11),
                     ("acos", x / 11), ("atanh", x / 11), ("asinh", x), ("ln", abs(x)), ("log10", abs(x)),
                     ("log2", abs(x)), ("sinh", x / 3), ("cosh", x / 3), ("tanh", x)):
            f64(m, a)
        f64("pow", abs(x), 0.37)
        f64("atan2", x, 1.3)
    # mathFunctionsOverTableRows: x = i * 0.37 - 40.
    for i in range(1, 201):
        x = i * 0.37 - 40
        for m, a in (("exp", x), ("sinh", x / 2), ("cosh", x / 2), ("tanh", x / 4), ("ln", x), ("log10", x),
                     ("log2", x)):
            f64(m, a)
        f64("atan2", x, 3.0)
        f64("pow", x, 1.5)
    # mathFunctionsOnSpecialValues and mathFunctionsOnOrdinaryValues.
    inf, tiny = math.inf, 5e-324
    for m, a in (("exp", 1.0), ("exp", -1.0), ("exp", 10.0), ("exp", 0.5), ("exp", -20.0), ("exp", 700.0),
                 ("exp", 710.0), ("exp", -746.0), ("exp", -740.0), ("exp", 709.7), ("exp", tiny), ("sinh", 710.0),
                 ("cosh", 710.0), ("sinh", 30.0), ("cosh", 30.0), ("tanh", 30.0), ("asinh", 1e10), ("acosh", 1e10),
                 ("acosh", 1.5), ("acosh", 3.0), ("atanh", 0.99), ("asin", 0.99), ("acos", 0.99), ("asin", 1.0),
                 ("acos", -1.0), ("atan", 1e10), ("ln", 1e300), ("log2", 1e-300), ("log10", 1e-5), ("ln", tiny),
                 ("log2", tiny), ("log10", 2.2250738585072014e-308), ("sin", tiny), ("sin", 1e22), ("cos", 1e22),
                 ("sin", 1e300), ("tan", 1e6), ("sin", 123456789.0), ("tan", -1e19), ("sin", inf), ("exp", -inf),
                 ("ln", 0.0), ("ln", -1.0), ("atanh", 1.0), ("tan", 1e-10), ("tan", math.pi / 4)):
        f64(m, a)
    for x, y in ((2.0, -1075.0), (2.0, -1074.0), (10.0, -320.0), (2.0, 1024.0), (10.0, 308.0), (7.0, 1 / 3.0),
                 (1.5, 2.5), (2.0, 0.5), (-8.0, 3.0), (-2.0, -3.0), (tiny, 0.5), (0.0, -1.0), (-8.0, 1.0 / 3),
                 (2.0, -inf), (0.5, inf), (-1.0, inf), (inf, -1.0), (-inf, 3.0)):
        f64("pow", x, y)
    for y, x in ((1.0, 2.0), (-1.0, -2.0), (3.0, -4.0), (0.0, -1.0), (0.0, -0.0), (1e300, 1e-300), (inf, -inf)):
        f64("atan2", y, x)
    # Seeded random operands of every magnitude, as tools/oracles/float_math_sweep.py makes them.
    rng = random.Random(20261125)
    for t, width, small in (("f64", 64, (1003, 1045, 52)), ("f32", 32, (112, 142, 23))):
        for m, _, _ in METHODS:
            for k in range(count):
                ops = []
                for _ in range(3):
                    if k % 3 == 0:
                        ops.append(rng.getrandbits(width))
                    else:
                        lo, hi, shift = small
                        ops.append((rng.getrandbits(1) << (width - 1)) | (rng.randrange(lo, hi) << shift)
                                   | rng.getrandbits(shift))
                out.append((t, m, ops[0], ops[1], ops[2], rng.randrange(-1200, 1200)))
    # f32 versions of the finding's arguments.
    for _, m, x, y, z, n in [c for c in out if c[0] == "f64" and c[1] in ("exp", "asin", "acos", "cosh", "tanh",
                                                                        "pow", "sinh", "atan2")][:2000]:
        xs, ys = from64(x), from64(y)
        if all(abs(v) < 3.4e38 or not math.isfinite(v) for v in (xs, ys)):
            out.append(("f32", m, bits32(xs), bits32(ys), bits32(0.0), n))
    return out


def program(all_cases):
    lines = []
    for t, u, width in (("f64", "u64", 16), ("f32", "u32", 8)):
        mine = [c for c in all_cases if c[0] == t]
        T = t.upper()
        lines.append(f"const M{T}: [u32; {len(mine)}] = [{', '.join(str(CODE[c[1]]) for c in mine)}];")
        for name, index in (("X", 2), ("Y", 3), ("Z", 4)):
            values = ", ".join(f"0x{c[index]:0{width}X}" for c in mine)
            lines.append(f"const {name}{T}: [{u}; {len(mine)}] = [{values}];")
        lines.append(f"const N{T}: [i32; {len(mine)}] = [{', '.join(str(c[5]) for c in mine)}];")
        lines.append(f"fn one{t}(m: u32, x: {t}, y: {t}, z: {t}, n: i32) -> {t} {{")
        lines.append("    return match (m) {")
        for k, (m, _, arity) in enumerate(METHODS):
            call = {1: "()", 2: "(y)", 3: "(y, z)", "n": "(n)"}[arity]
            lines.append(f"        {k} => x.{m}{call}," if k + 1 < len(METHODS) else f"        _ => x.{m}{call},")
        lines.append("    };")
        lines.append("}")
        lines.append(f"fn sweep{t}() {{")
        lines.append(f"    for (k in 0..{len(mine)}) {{")
        lines.append(f"        let r = one{t}(M{T}[k], {t}.fromBits(X{T}[k]), {t}.fromBits(Y{T}[k]), "
                     f"{t}.fromBits(Z{T}[k]), N{T}[k]);")
        lines.append("        Console.writeLine(r.bits());")
        lines.append("    }")
        lines.append("}")
    lines.append("fn main() {\n    sweepf64();\n    sweepf32();\n}")
    return "\n".join(lines) + "\n"


def c_program(all_cases):
    """The same calls in C, directly, for the static C runtime of `cl` (information only)."""
    lines = ["#include <math.h>", "#include <stdio.h>", "#include <stdint.h>", "#include <string.h>"]
    for t, ct, u, width, suffix in (("f64", "double", "uint64_t", 16, ""), ("f32", "float", "uint32_t", 8, "f")):
        mine = [c for c in all_cases if c[0] == t]
        T = t.upper()
        lines.append(f"static const unsigned M{T}[] = {{{', '.join(str(CODE[c[1]]) for c in mine)}}};")
        for name, index in (("X", 2), ("Y", 3), ("Z", 4)):
            lines.append(f"static const {u} {name}{T}[] = {{{', '.join(f'0x{c[index]:0{width}X}u' for c in mine)}}};")
        lines.append(f"static const int N{T}[] = {{{', '.join(str(c[5]) for c in mine)}}};")
        lines.append(f"static {ct} one{t}(unsigned m, {ct} x, {ct} y, {ct} z, int n) {{ switch (m) {{")
        for k, (m, c, arity) in enumerate(METHODS):
            args = {1: "x", 2: "x, y", 3: "x, y, z", "n": "x, n"}[arity]
            lines.append(f"    case {k}: return {c}{suffix}({args});")
        lines.append("    } return 0; }")
        lines.append(f"static void sweep{t}(void) {{ for (size_t k = 0; k < {len(mine)}; k++) {{")
        lines.append(f"    {ct} x, y, z, r; {u} b; memcpy(&x, &X{T}[k], sizeof x); memcpy(&y, &Y{T}[k], sizeof y);")
        lines.append(f"    memcpy(&z, &Z{T}[k], sizeof z); r = one{t}(M{T}[k], x, y, z, N{T}[k]);")
        lines.append("    memcpy(&b, &r, sizeof b); printf(\"%llu\\n\", (unsigned long long)b); } }")
    lines.append("int main(void) { sweepf64(); sweepf32(); return 0; }")
    return "\n".join(lines) + "\n"


def system_libm():
    if os.name == "nt":
        return ctypes.CDLL("ucrtbase"), "ucrtbase.dll"
    if sys.platform == "darwin":
        return ctypes.CDLL("/usr/lib/libSystem.B.dylib"), "libSystem"
    name = ctypes.util.find_library("m")
    return ctypes.CDLL(name), name or "libm"


def reference(all_cases):
    lib, label = system_libm()
    funcs = {}
    for m, c, arity in METHODS:
        for t, ctype, suffix in (("f64", ctypes.c_double, ""), ("f32", ctypes.c_float, "f")):
            name = c + suffix
            if not hasattr(lib, name) and hasattr(lib, "_" + name):
                name = "_" + name  # ucrtbase.dll's hypotf is _hypotf
            f = getattr(lib, name)
            f.restype = ctype
            f.argtypes = {1: [ctype], 2: [ctype] * 2, 3: [ctype] * 3, "n": [ctype, ctypes.c_int]}[arity]
            funcs[(m, t)] = f
    out = []
    for t, m, x, y, z, n in all_cases:
        conv, back = (from64, bits64) if t == "f64" else (from32, bits32)
        arity = ARITY[m]
        args = {1: (conv(x),), 2: (conv(x), conv(y)), 3: (conv(x), conv(y), conv(z)), "n": (conv(x), n)}[arity]
        out.append(back(funcs[(m, t)](*args)))
    return out, label


def python_math(case):
    """Python's math result's bits for an f64 case whose operands are finite, or None."""
    t, m, x, y, z, n = case
    name = PYTHON.get(m)
    if t != "f64" or name is None or not hasattr(math, name):
        return None
    args = {1: (from64(x),), 2: (from64(x), from64(y)), 3: (from64(x), from64(y), from64(z))}.get(ARITY[m])
    if args is None or not all(math.isfinite(a) for a in args) or (m == "atan2" and 0.0 in args):
        return None
    try:
        r = getattr(math, name)(*args)
    except (ValueError, OverflowError):
        return None
    return bits64(r) if math.isfinite(r) else None


def is_nan(b, t):
    if t == "f64":
        return (b >> 52) & 0x7FF == 0x7FF and b & ((1 << 52) - 1) != 0
    return (b >> 23) & 0xFF == 0xFF and b & ((1 << 23) - 1) != 0


def same(a, b, t):
    return a == b or (is_nan(a, t) and is_nan(b, t))


def describe(case):
    t, m, x, y, z, n = case
    conv = from64 if t == "f64" else from32
    ops = {1: [conv(x)], 2: [conv(x), conv(y)], 3: [conv(x), conv(y), conv(z)], "n": [conv(x), n]}[ARITY[m]]
    return f"{t} {m}({', '.join(repr(o) for o in ops)})"


def value(b, t):
    return repr(from64(b) if t == "f64" else from32(b))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cc", action="append", default=[], help="a C compiler to build with (repeatable)")
    parser.add_argument("--count", type=int, default=150, help="random operands per method and type")
    parser.add_argument("--no-interp", action="store_true")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    all_cases = cases(args.count)
    expected, label = reference(all_cases)
    problems = 0
    runs = []
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "main.cee")
        with open(source, "w", encoding="utf-8") as f:
            f.write(program(all_cases))
        for cc in args.cc or [None]:
            name = cc or "default"
            exe = os.path.join(work, f"libm-{os.path.basename(name)}" + (".exe" if os.name == "nt" else ""))
            command = [compiler, "build", source, "-o", exe] + (["--cc", cc] if cc else [])
            built = subprocess.run(command, capture_output=True, text=True)
            if built.returncode != 0:
                print(f"FAIL build with {name}: {(built.stderr or built.stdout).strip()[:1500]}")
                problems += 1
                continue
            ran = subprocess.run([exe], capture_output=True, text=True)
            runs.append((f"built with {name}", ran.stdout.split()))
        if not args.no_interp:
            ran = subprocess.run([compiler, "run", "--interp", source], capture_output=True, text=True)
            runs.append(("interpreted", ran.stdout.split()))
        static = None
        if os.name == "nt" and any(os.path.basename(c).lower() in ("cl", "cl.exe") for c in args.cc):
            c_source = os.path.join(work, "direct.c")
            with open(c_source, "w", encoding="utf-8") as f:
                f.write(c_program(all_cases))
            c_exe = os.path.join(work, "direct.exe")
            made = subprocess.run(["cl", "/nologo", "/std:c11", "/O2", c_source, f"/Fe:{c_exe}", f"/Fo:{work}\\"],
                                  capture_output=True, text=True, cwd=work)
            if made.returncode == 0:
                static = subprocess.run([c_exe], capture_output=True, text=True).stdout.split()
            else:
                print(f"note: the direct C program did not build with cl: {made.stdout.strip()[:400]}")
    print(f"libm: {label}; {len(all_cases)} results per run "
          f"({sum(1 for c in all_cases if c[0] == 'f64')} f64, {sum(1 for c in all_cases if c[0] == 'f32')} f32)")
    for what, got in runs:
        if len(got) != len(expected):
            print(f"FAIL {what}: {len(got)} results, expected {len(expected)}")
            problems += 1
            continue
        wrong = [k for k in range(len(expected)) if not same(int(got[k]), expected[k], all_cases[k][0])]
        known = what == "interpreted" and os.name == "nt"
        print(f"{'information: ' if known and wrong else ''}{what}: {len(wrong)} of {len(expected)} differ from "
              f"{label}" + (" (the C library ceesafec was linked with: ADR 0248 §2.3)" if known and wrong else ""))
        for k in wrong[:25]:
            t = all_cases[k][0]
            print(f"    {describe(all_cases[k])} = {value(int(got[k]), t)}, {label} {value(expected[k], t)}")
        if not known:
            problems += len(wrong)
    witnessed = 0
    disagree = 0
    for k, case in enumerate(all_cases):
        b = python_math(case)
        if b is None:
            continue
        witnessed += 1
        if b != expected[k]:
            disagree += 1
            if disagree <= 10:
                print(f"    Python's math: {describe(case)} = {value(b, 'f64')}, {label} {value(expected[k], 'f64')}")
    print(f"Python's math: {disagree} of {witnessed} f64 results differ from {label}")
    problems += disagree
    if static is not None:
        by = {}
        for k, case in enumerate(all_cases):
            if k < len(static) and not same(int(static[k]), expected[k], case[0]):
                by[f"{case[0]} {case[1]}"] = by.get(f"{case[0]} {case[1]}", 0) + 1
        detail = ", ".join(f"{k} {v}" for k, v in sorted(by.items(), key=lambda kv: -kv[1]))
        print(f"information: C built by cl with its static C runtime (the SDK's UCRT): {sum(by.values())} of "
              f"{len(expected)} differ from {label}" + (f" ({detail})" if detail else ""))
        k = next((k for k, c in enumerate(all_cases) if c[1] == "pow" and c[2] == bits64(2.0)
                  and c[3] == bits64(-1075.0)), None)
        if k is not None and k < len(static):
            print(f"information: pow(2, -1075) is {value(int(static[k]), 'f64')} with the static UCRT, "
                  f"{value(expected[k], 'f64')} with {label}")
    print(f"libm_check: {len(all_cases) * max(len(runs), 1)} results: {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
