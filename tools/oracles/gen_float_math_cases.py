#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/float-libm.cee and float-libm-f32.cee: the float methods of ADR 0218
(type-system.md §35.8) against Python's math module.

Python's math module is the oracle: CPython computes each function with the platform's C library, independently of
ceesafec, and refuses (ValueError, OverflowError) where C returns a special value. The expectations are of two kinds.

- Exact results, printed in full. These are the values C11 Annex F specifies (signed zeros, infinities, NaN, domain
  and pole errors) and the results that are exact in every IEEE libm (exp(0), pow(2, 10), hypot(3, 4), fmod, fma,
  scalbn, logb, rounding). Where Python refuses, the value is Annex F's, written in ANNEX_F below with its clause;
  where Python 3.9 has no such function (exp2, cbrt, fma, logb, round half away from zero), it is computed exactly
  by other means of Python's (math.ldexp, math.frexp, fractions, decimal), each named below.
- Inexact results, compared in the program: each must lie within ULPS units in the last place of the oracle's value
  (math.<f>, or mpmath where math has none). A libm's last bit differs between platforms (glibc, macOS, FreeBSD,
  the Windows C runtime), so the corpus checks that each method computes its function. The interpreter's agreement
  with the build, bit for bit, is the exact section's under --interp, and for inexact results ROADMAP §10's sweep
  (tools/oracles/float_math_sweep.py). On macOS arm64 (2026-11-24) the f64 results equal the oracle's in every bit but for 4 of 11 hypot
  cases (CPython's math.hypot is its own algorithm, not libm's) and 1 of 10 cbrt cases (the oracle is mpmath's
  correctly rounded value, since Python 3.9 has no math.cbrt), each 1 ulp away; of the f32 results (whose oracle is
  the f64 result rounded to f32, not libm's f functions), 4 are 1 ulp away.

usage: python3 tools/oracles/gen_float_math_cases.py   (formats with $CEESAFEC, default out/ceesafec; then review the diff)
"""

import decimal
import fractions
import math
import os
import random
import struct
import subprocess

import mpmath

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ULPS = 4
INF = math.inf
NAN = math.nan


def bits64(v):
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def to32(v):
    """`v` rounded to f32 (struct rounds to nearest even, and refuses only what rounds to an infinity)."""
    try:
        return struct.unpack("<f", struct.pack("<f", v))[0]
    except OverflowError:
        return math.copysign(INF, v)


def bits32(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def js(v, single=False):
    """`v` as CeeSafe prints it (type-system.md §35.7: the shortest digits, ECMAScript's layout), with a negative
    zero shown as `-0`, as the programs' `show` does."""
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "Infinity" if v > 0 else "-Infinity"
    if v == 0:
        return "-0" if math.copysign(1.0, v) < 0 else "0"
    sign = "-" if v < 0 else ""
    a = abs(v)
    for p in range(1, 18):
        s = "%.*e" % (p - 1, a)
        if (to32(float(s)) == a) if single else (float(s) == a):
            break
    mantissa, exponent = s.split("e")
    digits = mantissa.replace(".", "").rstrip("0")
    k = len(digits)
    n = int(exponent) + 1
    if k <= n <= 21:
        text = digits + "0" * (n - k)
    elif 0 < n <= 21:
        text = digits[:n] + "." + digits[n:]
    elif -6 < n <= 0:
        text = "0." + "0" * (-n) + digits
    else:
        e = n - 1
        text = digits[0] + ("." + digits[1:] if k > 1 else "") + "e" + ("+" if e >= 0 else "-") + str(abs(e))
    return sign + text


# What C11 Annex F gives where Python's math module raises instead (F.10): ((function, arguments), value), a list
# since a dict would take 0.0 and -0.0 as one key.
ANNEX_F = [
    (("exp", (1000.0,)), INF),              # F.10.3.1: overflow, +inf
    (("exp2", (1024.0,)), INF),             # F.10.3.2
    (("ln", (0.0,)), -INF),                 # F.10.3.7: log(±0) is -inf, a pole
    (("ln", (-0.0,)), -INF),
    (("ln", (-1.0,)), NAN),                 # log(x < 0) is NaN, a domain error
    (("log2", (0.0,)), -INF),               # F.10.3.10
    (("log2", (-1.0,)), NAN),
    (("log10", (0.0,)), -INF),              # F.10.3.8
    (("log10", (-INF,)), NAN),
    (("ln1p", (-1.0,)), -INF),              # F.10.3.9
    (("ln1p", (-2.0,)), NAN),
    (("sin", (INF,)), NAN),                 # F.10.1.6
    (("cos", (-INF,)), NAN),                # F.10.1.5
    (("tan", (INF,)), NAN),                 # F.10.1.7
    (("asin", (2.0,)), NAN),                # F.10.1.2
    (("acos", (-2.0,)), NAN),               # F.10.1.1
    (("acosh", (0.5,)), NAN),               # F.10.2.1
    (("atanh", (1.0,)), INF),               # F.10.2.3: a pole
    (("atanh", (-1.0,)), -INF),
    (("atanh", (2.0,)), NAN),
    (("sinh", (1000.0,)), INF),             # F.10.2.5: overflow
    (("cosh", (-1000.0,)), INF),            # F.10.2.4
    (("pow", (-8.0, 1.0 / 3.0)), NAN),      # F.10.4.4: a negative base and a non-integral exponent
    (("pow", (0.0, -1.0)), INF),            # pow(±0, odd y < 0) is ±inf
    (("pow", (-0.0, -1.0)), -INF),
    (("pow", (-0.0, -2.0)), INF),           # pow(±0, even y < 0) is +inf
    (("pow", (10.0, 400.0)), INF),          # overflow
    (("remainder", (1.0, 0.0)), NAN),       # F.10.7.2
    (("remainder", (INF, 1.0)), NAN),
    (("truncatingRemainder", (1.0, 0.0)), NAN),  # F.10.7.1: fmod(x, 0) and fmod(±inf, y) are NaN
    (("truncatingRemainder", (INF, 1.0)), NAN),
]


def fma_exact(x, y, z):
    """x * y + z rounded once: fractions are exact, and float(Fraction) rounds to nearest, ties to even."""
    if any(math.isnan(v) for v in (x, y, z)):
        return NAN
    if math.isinf(x) or math.isinf(y):
        return x * y + z  # inf * 0 is NaN, and inf + -inf
    if math.isinf(z):
        return z  # the exact product is finite, even where rounding it alone would overflow
    r = fractions.Fraction(x) * fractions.Fraction(y) + fractions.Fraction(z)
    if r == 0:
        # An exact zero sum is +0 in round-to-nearest, unless both addends are -0 (IEEE 754 §6.3).
        product_zero = x == 0 or y == 0
        product_negative = math.copysign(1, x) * math.copysign(1, y) < 0
        both_negative = product_zero and product_negative and z == 0 and math.copysign(1, z) < 0
        return -0.0 if both_negative else 0.0
    return float(r)


def logb(x):
    """C's logb from math.frexp: frexp gives x = m * 2^e with 0.5 <= |m| < 1, so logb is e - 1."""
    if math.isnan(x):
        return x
    if math.isinf(x):
        return INF
    if x == 0:
        return -INF  # F.10.3.11: a pole
    return float(math.frexp(x)[1] - 1)


def round_half_away(x):
    """round(): the decimal module's ROUND_HALF_UP is ties away from zero, on the exact value of x."""
    if math.isnan(x) or math.isinf(x) or abs(x) >= 2.0 ** 52:
        return x  # already integral
    r = float(decimal.Decimal(x).quantize(decimal.Decimal(1), rounding=decimal.ROUND_HALF_UP))
    return math.copysign(r, x) if r == 0 else r


def round_half_even(x):
    if math.isnan(x) or math.isinf(x):
        return x
    r = float(round(x))  # Python's round is ties to even, on the exact value
    return math.copysign(r, x) if r == 0 else r


def trunc(x):
    if math.isnan(x) or math.isinf(x):
        return x
    return math.copysign(float(math.trunc(x)), x)


def exp2(x):
    """Exact for the integral arguments used: 2^x by math.ldexp."""
    if math.isinf(x):
        return 0.0 if x < 0 else INF
    assert x == int(x)
    try:
        return math.ldexp(1.0, int(x))
    except OverflowError:
        return INF


def real_cbrt(x):
    """The real cube root (mpmath.cbrt gives the principal, complex, root of a negative number)."""
    if mpmath.isinf(x) or x == 0 or mpmath.isnan(x):
        return x
    return mpmath.sign(x) * mpmath.cbrt(abs(x))


def mp(f):
    """A function math lacks in Python 3.9, from mpmath at 200 bits, rounded once to a double."""
    def g(x):
        if x == 0 or math.isinf(x) or math.isnan(x):
            return x  # mpmath has no signed zero; cbrt keeps these as they are (C11 F.10.4.1)
        with mpmath.workprec(200):
            return float(f(mpmath.mpf(x)))
    return g


# The methods: name -> (the oracle, the number of float operands).
ORACLE = {
    "exp": (math.exp, 1), "exp2": (exp2, 1), "expM1": (math.expm1, 1), "ln": (math.log, 1),
    "log2": (math.log2, 1), "log10": (math.log10, 1), "ln1p": (math.log1p, 1), "sin": (math.sin, 1),
    "cos": (math.cos, 1), "tan": (math.tan, 1), "asin": (math.asin, 1), "acos": (math.acos, 1),
    "atan": (math.atan, 1), "sinh": (math.sinh, 1), "cosh": (math.cosh, 1), "tanh": (math.tanh, 1),
    "asinh": (math.asinh, 1), "acosh": (math.acosh, 1), "atanh": (math.atanh, 1),
    "cbrt": (mp(real_cbrt), 1), "logB": (logb, 1), "pow": (math.pow, 2), "atan2": (math.atan2, 2),
    "hypot": (math.hypot, 2), "remainder": (math.remainder, 2), "truncatingRemainder": (math.fmod, 2),
    "fma": (fma_exact, 3), "trunc": (trunc, 1), "round": (round_half_away, 1), "roundEven": (round_half_even, 1),
    "copySign": (math.copysign, 2),
}


def annex_f(name, args):
    """ANNEX_F's value for these operands, matched by their bits: in a Python dict, 0.0 and -0.0 are one key."""
    for (n, a), v in ANNEX_F:
        if n == name and len(a) == len(args) and all(bits64(p) == bits64(q) for p, q in zip(a, args)):
            return v
    return None


def oracle(name, args):
    special = annex_f(name, args)
    if special is not None:
        return special
    f, _ = ORACLE[name]
    return f(*args)


# Exact results: (method, float operands), and scaleB's (x, n) apart.
EXACT = [
    ("exp", (0.0,)), ("exp", (-0.0,)), ("exp", (INF,)), ("exp", (-INF,)), ("exp", (NAN,)), ("exp", (1000.0,)),
    ("exp", (-1000.0,)),
    ("exp2", (10.0,)), ("exp2", (-1074.0,)), ("exp2", (1024.0,)), ("exp2", (-INF,)),
    ("expM1", (0.0,)), ("expM1", (-0.0,)), ("expM1", (INF,)), ("expM1", (-INF,)), ("expM1", (1e-300,)),
    ("ln", (1.0,)), ("ln", (0.0,)), ("ln", (-0.0,)), ("ln", (-1.0,)), ("ln", (INF,)), ("ln", (NAN,)),
    ("log2", (1024.0,)), ("log2", (0.5,)), ("log2", (0.0,)), ("log2", (-1.0,)), ("log2", (INF,)),
    ("log10", (1000.0,)), ("log10", (1.0,)), ("log10", (0.0,)), ("log10", (-INF,)),
    ("ln1p", (0.0,)), ("ln1p", (-0.0,)), ("ln1p", (-1.0,)), ("ln1p", (-2.0,)), ("ln1p", (INF,)),
    ("sin", (0.0,)), ("sin", (-0.0,)), ("sin", (INF,)), ("sin", (NAN,)),
    ("cos", (0.0,)), ("cos", (-0.0,)), ("cos", (-INF,)),
    ("tan", (-0.0,)), ("tan", (INF,)),
    ("asin", (-0.0,)), ("asin", (1.0,)), ("asin", (2.0,)),
    ("acos", (1.0,)), ("acos", (-1.0,)), ("acos", (-2.0,)),
    ("atan", (-0.0,)), ("atan", (INF,)), ("atan", (-INF,)),
    ("sinh", (-0.0,)), ("sinh", (-INF,)), ("sinh", (1000.0,)),
    ("cosh", (0.0,)), ("cosh", (-INF,)), ("cosh", (-1000.0,)),
    ("tanh", (-0.0,)), ("tanh", (INF,)), ("tanh", (-INF,)),
    ("asinh", (-0.0,)), ("asinh", (-INF,)),
    ("acosh", (1.0,)), ("acosh", (0.5,)), ("acosh", (INF,)),
    ("atanh", (-0.0,)), ("atanh", (1.0,)), ("atanh", (-1.0,)), ("atanh", (2.0,)),
    # -64, not -27: glibc 2.39's cbrt(-27) is -3.0000000000000004, a last bit the platform may give (ADR 0218 §2.2).
    ("cbrt", (-0.0,)), ("cbrt", (-INF,)), ("cbrt", (-64.0,)), ("cbrt", (8.0,)),
    ("logB", (8.0,)), ("logB", (0.1,)), ("logB", (0.0,)), ("logB", (-0.0,)), ("logB", (-INF,)), ("logB", (NAN,)),
    ("logB", (5e-324,)), ("logB", (-1e308,)),
    ("pow", (2.0, 10.0)), ("pow", (NAN, 0.0)), ("pow", (1.0, NAN)), ("pow", (-8.0, 1.0 / 3.0)), ("pow", (0.0, -1.0)),
    ("pow", (-0.0, -1.0)), ("pow", (-0.0, -2.0)), ("pow", (-1.0, INF)), ("pow", (0.5, INF)), ("pow", (2.0, -INF)),
    ("pow", (-INF, 3.0)), ("pow", (-2.0, 3.0)), ("pow", (10.0, 400.0)), ("pow", (-0.0, 3.0)),
    ("atan2", (0.0, -0.0)), ("atan2", (-0.0, -0.0)), ("atan2", (0.0, 0.0)), ("atan2", (-0.0, 0.0)),
    ("atan2", (1.0, 0.0)), ("atan2", (INF, INF)), ("atan2", (-INF, -INF)), ("atan2", (1.0, -INF)),
    ("atan2", (-1.0, INF)),
    ("hypot", (3.0, 4.0)), ("hypot", (INF, NAN)), ("hypot", (NAN, -INF)), ("hypot", (-0.0, 0.0)),
    ("remainder", (5.0, 3.0)), ("remainder", (7.0, 2.0)), ("remainder", (-0.0, 1.0)), ("remainder", (1.0, 0.0)),
    ("remainder", (INF, 1.0)), ("remainder", (1.0, INF)),
    ("truncatingRemainder", (5.0, 3.0)), ("truncatingRemainder", (-5.0, 3.0)), ("truncatingRemainder", (5.5, 2.0)),
    ("truncatingRemainder", (1.0, 0.0)), ("truncatingRemainder", (INF, 1.0)), ("truncatingRemainder", (1.0, INF)),
    ("truncatingRemainder", (-0.0, 1.0)),
    ("fma", (2.0, 3.0, 4.0)), ("fma", (0.1, 10.0, -1.0)), ("fma", (INF, 0.0, 1.0)), ("fma", (1e308, 10.0, -INF)),
    ("fma", (-0.0, 1.0, -0.0)), ("fma", (1.0 + 2.0 ** -52, 1.0 - 2.0 ** -53, -1.0)),
]
ROUNDED = [2.5, -2.5, 0.5, -0.5, 1.5, -1.5, 0.49999999999999994, -0.4, 0.7, -0.0, 4503599627370495.5,
           4503599627370497.0, -4503599627370495.5, 1e300, INF, -INF, NAN]
for name in ("trunc", "round", "roundEven"):
    EXACT += [(name, (v,)) for v in ROUNDED]
EXACT += [("copySign", (1.0, -0.0)), ("copySign", (-2.0, 3.0)), ("copySign", (0.0, -1.0)), ("copySign", (INF, -0.0)),
          ("copySign", (-0.0, 0.0))]
SCALEB = [(1.0, 1023), (1.0, 1024), (1.0, -1074), (1.0, -1075), (3.0, -1075), (-0.0, 5), (INF, -2000), (0.75, 2),
          (5e-324, 1074)]


def scaleb(x, n):
    try:
        return math.ldexp(x, n)
    except OverflowError:
        return math.copysign(INF, x)


def label(name, args):
    def a(v):
        return js(v)
    return f"{a(args[0])}.{name}({', '.join(a(v) for v in args[1:])})"


def literal(v):
    return f"f64.fromBits(0x{bits64(v):016X})"


# Inexact results: arguments in each function's domain, seeded, plus notable ones.
def inexact_cases():
    rng = random.Random(20261124)

    def u(lo, hi, k=8):
        return [rng.uniform(lo, hi) for _ in range(k)]

    cases = []
    one = {
        "exp": u(-700, 700) + [1.0, -1.0, 0.5, 1e-10], "exp2": u(-1000, 1000) + [0.5, -0.5],
        "expM1": u(-40, 40) + [1e-10, -1e-5], "ln": [rng.uniform(0, 1e300) for _ in range(6)] + u(0, 2) + [2.0],
        "log2": u(1e-300, 1e300) + [3.0, 0.1], "log10": u(0, 1e6) + [2.0, 0.5], "ln1p": u(-0.99, 100) + [1e-10],
        "sin": u(-10, 10) + [1e6, 3.141592653589793, 1e22], "cos": u(-10, 10) + [1e6, 1.5707963267948966],
        "tan": u(-1.5, 1.5) + [1e6, 1.0], "asin": u(-1, 1) + [0.5], "acos": u(-1, 1) + [0.5],
        "atan": u(-100, 100) + [1.0, 1e-8], "sinh": u(-700, 700) + [1e-8, 0.5], "cosh": u(-700, 700) + [0.5],
        "tanh": u(-20, 20) + [0.5, 1e-8], "asinh": u(-1e6, 1e6) + [0.5], "acosh": u(1, 1e6) + [1.5],
        "atanh": u(-0.999, 0.999) + [0.5], "cbrt": u(-1e9, 1e9) + [2.0, 1e-300],
    }
    for name, xs in one.items():
        for x in xs:
            cases.append((name, (x,)))
    two = {
        "pow": [(rng.uniform(0, 10), rng.uniform(-30, 30)) for _ in range(8)] + [(2.0, 0.5), (-2.0, 7.0), (10.0, -3.0)],
        "atan2": [(rng.uniform(-10, 10), rng.uniform(-10, 10)) for _ in range(8)] + [(1.0, 1.0)],
        "hypot": [(rng.uniform(-1e3, 1e3), rng.uniform(-1e3, 1e3)) for _ in range(8)] + [(1e300, 1e300), (1.0, 1e-20)],
    }
    for name, pairs in two.items():
        for p in pairs:
            cases.append((name, p))
    return cases


def check_oracle(name, args, expected):
    """The oracle's inexact value against mpmath's correctly rounded one: the platform's libm is near it."""
    mpf = {"exp": mpmath.exp, "exp2": lambda x: mpmath.power(2, x), "expM1": mpmath.expm1, "ln": mpmath.log,
           "log2": lambda x: mpmath.log(x, 2), "log10": mpmath.log10, "ln1p": mpmath.log1p, "sin": mpmath.sin,
           "cos": mpmath.cos, "tan": mpmath.tan, "asin": mpmath.asin, "acos": mpmath.acos, "atan": mpmath.atan,
           "sinh": mpmath.sinh, "cosh": mpmath.cosh, "tanh": mpmath.tanh, "asinh": mpmath.asinh,
           "acosh": mpmath.acosh, "atanh": mpmath.atanh, "cbrt": real_cbrt, "pow": mpmath.power,
           "atan2": mpmath.atan2, "hypot": mpmath.hypot}[name]
    with mpmath.workprec(300):
        exact = float(mpf(*[mpmath.mpf(a) for a in args]))
    assert abs(bits64(exact) - bits64(expected)) <= 1, (name, args, expected, exact)


def exp2_inexact(x):
    return math.pow(2.0, x)  # libm's pow, as exp2 for a non-integral x


HEAD = """// The float methods of ADR 0218 (type-system.md §35.8) on {T}. GENERATED by tools/oracles/gen_float_math_cases.py
// from Python's math module: edit the generator. First the exact results (C11 Annex F's special values, signed zeros,
// and results exact in every IEEE libm), printed in full; then results compared within {U} ulps of the oracle, since
// a libm's last bit is the platform's.
"""

COMMON = """
struct Tally {
    cases: u32;
    failed: u32;
}

fn show(x: {T}) {
    if (x == 0.0 && x.bits() != 0) {
        Console.write("-0");
    } else {
        Console.write(x);
    }
}

fn line(label: StringView, x: {T}) {
    Console.write(label);
    Console.write(" = ");
    show(x);
    Console.writeLine("");
}

/// The bits of `b` in the order of the values they stand for, so that neighbours differ by one.
fn ordered(b: {U}) -> {U} => if (b >= {SIGN}) {SIGN} - (b - {SIGN}) else {SIGN} + b;

fn near(t: borrow mut Tally, label: StringView, x: {T}, expected: {U}) {
    t.cases += 1;
    let e = {T}.fromBits(expected);
    var ok = x != x && e != e;
    if (x == x && e == e) {
        let a = ordered(x.bits());
        let b = ordered(expected);
        ok = (if (a > b) a - b else b - a) <= {ULPS};
    }
    if (!ok) {
        t.failed += 1;
        Console.write(label);
        Console.write(" = ");
        show(x);
        Console.write(", expected ");
        show(e);
        Console.writeLine("");
    }
}

fn done(t: borrow mut Tally, name: StringView) {
    Console.write(name);
    Console.write(": ");
    Console.write(t.cases - t.failed);
    Console.write(" of ");
    Console.write(t.cases);
    Console.writeLine(" within {ULPS} ulps");
    t.cases = 0;
    t.failed = 0;
}
"""


def write(path, source):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source)
    subprocess.run([os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec")), "fmt", path], check=True)
    print(f"wrote {os.path.relpath(path, ROOT)}")


def f64_program():
    out, body = [], []
    for name, args in EXACT:
        v = oracle(name, args)
        lab = label(name, args)
        out.append(f"{lab} = {js(v)}")
        call = f"{literal(args[0])}.{name}({', '.join(literal(a) for a in args[1:])})"
        body.append(f'    line("{lab}", {call});')
    for x, n in SCALEB:
        v = scaleb(x, n)
        lab = f"{js(x)}.scaleB({n})"
        out.append(f"{lab} = {js(v)}")
        body.append(f'    line("{lab}", {literal(x)}.scaleB({n}));')
    body.append("    var t = Tally { cases: 0, failed: 0 };")
    last = None
    for name, args in inexact_cases():
        if last is not None and name != last:
            body.append(f'    done(mut t, "{last}");')
            out.append(f"{last}: {count[last]} of {count[last]} within {ULPS} ulps")
        last = name
        v = exp2_inexact(args[0]) if name == "exp2" else oracle(name, args)
        check_oracle(name, args, v)
        call = f"{literal(args[0])}.{name}({', '.join(literal(a) for a in args[1:])})"
        body.append(f'    near(mut t, "{label(name, args)}", {call}, 0x{bits64(v):016X});')
    body.append(f'    done(mut t, "{last}");')
    out.append(f"{last}: {count[last]} of {count[last]} within {ULPS} ulps")
    return out, body


count = {}
for _name, _args in inexact_cases():
    count[_name] = count.get(_name, 0) + 1


# f32: the methods call libm's f functions (expf …). The oracle is the double result rounded to f32, which is within
# an f32 ulp of the exact value.
EXACT32 = [("exp", (0.0,)), ("exp", (-INF,)), ("exp", (100.0,)), ("ln", (1.0,)), ("ln", (-0.0,)), ("ln", (-1.0,)),
           ("sin", (-0.0,)), ("cos", (0.0,)), ("pow", (2.0, 10.0)), ("pow", (-0.0, -1.0)),
           ("hypot", (3.0, 4.0)), ("remainder", (7.0, 2.0)), ("truncatingRemainder", (-5.0, 3.0)),
           ("fma", (0.1, 10.0, -1.0)), ("logB", (0.1,)), ("cbrt", (-27.0,)), ("asin", (2.0,)), ("atanh", (-1.0,))]
ANNEX_F.append((("exp", (100.0,)), INF))  # overflows f32: expf(100) is +inf
for _name in ("trunc", "round", "roundEven"):
    EXACT32 += [(_name, (v,)) for v in [2.5, -2.5, 0.5, -0.5, 1.5, -0.4, 8388607.5, 8388609.0, 0.49999997, INF, NAN]]
EXACT32 += [("copySign", (1.0, -0.0)), ("copySign", (-2.0, 3.0))]
SCALEB32 = [(1.0, 127), (1.0, 128), (1.0, -149), (1.0, -150), (3.0, -150)]


def oracle32(name, args):
    args = tuple(to32(a) for a in args)
    if name == "fma":
        return to32(fma_exact(*args))  # an f32 product is exact in f64, so one rounding of the exact sum to f32
    if name == "exp" and args == (100.0,):
        return INF
    v = oracle(name, args)
    return to32(v)


def literal32(v):
    return f"f32.fromBits(0x{bits32(v):08X})"


def f32_program():
    out, body = [], []
    for name, args in EXACT32:
        args = tuple(to32(a) for a in args)
        v = oracle32(name, args)
        lab = f"{js(args[0], True)}.{name}({', '.join(js(a, True) for a in args[1:])})"
        out.append(f"{lab} = {js(v, True)}")
        call = f"{literal32(args[0])}.{name}({', '.join(literal32(a) for a in args[1:])})"
        body.append(f'    line("{lab}", {call});')
    for x, n in SCALEB32:
        try:
            v = to32(math.ldexp(x, n))
        except OverflowError:
            v = INF
        if abs(v) > 3.4028234663852886e38:
            v = INF
        lab = f"{js(x, True)}.scaleB({n})"
        out.append(f"{lab} = {js(v, True)}")
        body.append(f'    line("{lab}", {literal32(x)}.scaleB({n}));')
    body.append("    var t = Tally { cases: 0, failed: 0 };")
    # atan2f(±0, -0) is ±π (F.10.1.4), but which f32 near π is the libm's: macOS's is 3.1415925, below π, and
    # glibc's the nearest, 3.1415927.
    for y in (0.0, -0.0):
        v = to32(math.atan2(y, -0.0))
        body.append(f'    near(mut t, "{js(y, True)}.atan2(-0)", {literal32(y)}.atan2({literal32(-0.0)}), '
                    f"0x{bits32(v):08X});")
    body.append('    done(mut t, "atan2 of zeros");')
    out.append(f"atan2 of zeros: 2 of 2 within {ULPS} ulps")
    rng = random.Random(20261125)
    domains = {"exp": (-80, 80), "ln": (0, 1e30), "log2": (0, 1e30), "log10": (0, 1e30), "sin": (-10, 10),
               "cos": (-10, 10), "tan": (-1.5, 1.5), "asin": (-1, 1), "acos": (-1, 1), "atan": (-100, 100),
               "sinh": (-80, 80), "cosh": (-80, 80), "tanh": (-10, 10), "asinh": (-1e6, 1e6), "acosh": (1, 1e6),
               "atanh": (-0.99, 0.99), "cbrt": (-1e9, 1e9), "expM1": (-20, 20), "ln1p": (-0.9, 100)}
    for name, (lo, hi) in domains.items():
        for _ in range(6):
            x = to32(rng.uniform(lo, hi))
            v = to32(oracle(name, (x,)))
            body.append(f'    near(mut t, "{js(x, True)}.{name}()", {literal32(x)}.{name}(), 0x{bits32(v):08X});')
        body.append(f'    done(mut t, "{name}");')
        out.append(f"{name}: 6 of 6 within {ULPS} ulps")
    for name in ("pow", "atan2", "hypot"):
        for _ in range(6):
            x, y = to32(rng.uniform(0.1, 10)), to32(rng.uniform(-10, 10))
            v = to32(oracle(name, (x, y)))
            body.append(f'    near(mut t, "{js(x, True)}.{name}({js(y, True)})", {literal32(x)}.{name}({literal32(y)}), '
                        f"0x{bits32(v):08X});")
        body.append(f'    done(mut t, "{name}");')
        out.append(f"{name}: 6 of 6 within {ULPS} ulps")
    return out, body


def program(t, u, sign, out, body):
    expect = "\n".join(f"// expect-stdout: {line}" for line in out)
    common = (COMMON.replace("{T}", t).replace("{U}", u).replace("{SIGN}", sign)
              .replace("{ULPS}", str(ULPS)))
    return (HEAD.replace("{T}", t).replace("{U}", str(ULPS)) + expect + "\n" + common + "\nfn main() {\n" +
            "\n".join(body) + "\n}\n")


def main():
    corpus = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass")
    out, body = f64_program()
    write(os.path.join(corpus, "float-libm.cee"), program("f64", "u64", "0x8000000000000000", out, body))
    out, body = f32_program()
    write(os.path.join(corpus, "float-libm-f32.cee"), program("f32", "u32", "0x80000000", out, body))


if __name__ == "__main__":
    main()
