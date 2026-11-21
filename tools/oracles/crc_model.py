#!/usr/bin/env python3
"""An independent CRC model for tests of lib/std's Crc32, Crc32c and Crc64Nvme (ADRs 0075 §2.5 and 0194 §2.3).

The Rocksoft parameter model ("A Painless Guide to CRC Error Detection Algorithms", Williams, 1993, §15), computed
bit by bit, most significant bit first, with the polynomial in its normal (unreflected) form. Reflected input is
handled by reversing each byte before it is shifted in, and reflected output by reversing the register at the end. It
shares nothing with tools/gen_checksums.py, which builds reflected lookup tables (least significant bit first).

Each model is checked against its catalogued check value, the CRC of the ASCII bytes "123456789" (Greg Cook's CRC
catalogue: CRC-32/ISO-HDLC, CRC-32/ISCSI and CRC-64/NVME), and against Python's zlib.crc32 for the CRC-32 (ISO-HDLC)
parameters, when this file is run.

It also computes the constants of the runtime's x86-64 CRC-32 folding (ADR 0194 §2.3; Gopal et al., Intel, "Fast CRC
Computation for Generic Polynomials Using PCLMULQDQ", 2009) by polynomial arithmetic over GF(2), and checks them
against the `CEE_CRC32_*` lines of runtime/hosted/ceesafe_rt_io.c.

usage: python3 tools/oracles/crc_model.py   (prints the check values; exits non-zero on a mismatch)
"""

import os
import re
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def reflect(value, width):
    out = 0
    for _ in range(width):
        out = (out << 1) | (value & 1)
        value >>= 1
    return out


def crc(data, width, poly, init, refin, refout, xorout):
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    register = init
    for byte in data:
        if refin:
            byte = reflect(byte, 8)
        register ^= byte << (width - 8)
        for _ in range(8):
            if register & top:
                register = ((register << 1) ^ poly) & mask
            else:
                register = (register << 1) & mask
    if refout:
        register = reflect(register, width)
    return register ^ xorout


def crc32(data):
    """CRC-32/ISO-HDLC (IEEE, zlib's): width 32, poly 0x04C11DB7, init and xorout all ones, reflected."""
    return crc(data, 32, 0x04C11DB7, 0xFFFFFFFF, True, True, 0xFFFFFFFF)


def crc32c(data):
    """CRC-32/ISCSI (CRC-32C): width 32, poly 0x1EDC6F41, init and xorout all ones, reflected."""
    return crc(data, 32, 0x1EDC6F41, 0xFFFFFFFF, True, True, 0xFFFFFFFF)


def crc64nvme(data):
    """CRC-64/NVME: width 64, poly 0xAD93D23594C93659, init and xorout all ones, reflected."""
    return crc(data, 64, 0xAD93D23594C93659, 0xFFFFFFFFFFFFFFFF, True, True, 0xFFFFFFFFFFFFFFFF)


def poly_mod(exponent, poly):
    """x^exponent mod poly, poly with its top bit set."""
    r = 1 << exponent
    width = poly.bit_length()
    while r.bit_length() >= width:
        r ^= poly << (r.bit_length() - width)
    return r


def poly_div(a, b):
    """The quotient of the polynomials a / b."""
    q = 0
    while a.bit_length() >= b.bit_length():
        shift = a.bit_length() - b.bit_length()
        q |= 1 << shift
        a ^= b << shift
    return q


def folding_constants():
    """The reflected CRC-32's folding constants: x^d mod P bit-reflected and shifted left one, for d = 4·128 ± 32
    (K1, K2), 128 ± 32 (K3, K4) and 64 (K5); MU, floor(x^64 / P), and POLY, P, each reflected over 33 bits."""
    p = 0x104C11DB7
    k = {name: reflect(poly_mod(d, p), 32) << 1
         for name, d in (("K1", 4 * 128 + 32), ("K2", 4 * 128 - 32), ("K3", 128 + 32), ("K4", 128 - 32), ("K5", 64))}
    k["MU"] = reflect(poly_div(1 << 64, p), 33)
    k["POLY"] = reflect(p, 33)
    return k


def check_runtime_constants():
    """The runtime's `#define CEE_CRC32_<name> 0x…LL` lines against folding_constants(): a mismatch raises."""
    path = os.path.join(ROOT, "runtime", "hosted", "ceesafe_rt_io.c")
    with open(path, encoding="utf-8") as f:
        found = dict((m.group(1), int(m.group(2), 16))
                     for m in re.finditer(r"#define CEE_CRC32_([A-Z0-9]+) 0x([0-9A-F]+)LL", f.read()))
    want = folding_constants()
    assert found == want, (found, want)


def self_check():
    """The catalogue's check values, and zlib's CRC-32 through the same model: a failure raises."""
    check = b"123456789"
    assert crc32(check) == 0xCBF43926, hex(crc32(check))
    assert crc32c(check) == 0xE3069283, hex(crc32c(check))
    assert crc64nvme(check) == 0xAE8B14860A799888, hex(crc64nvme(check))
    for n in (0, 1, 7, 64, 200):
        data = bytes((k * 151 + n) & 0xFF for k in range(n))
        assert crc32(data) == zlib.crc32(data), n
    check_runtime_constants()


if __name__ == "__main__":
    self_check()
    print("CRC-32(123456789) = 0x%08X" % crc32(b"123456789"))
    print("CRC-32C(123456789) = 0x%08X" % crc32c(b"123456789"))
    print("CRC-32 folding constants: " + ", ".join("%s 0x%X" % kv for kv in folding_constants().items()))
    print("CRC-64/NVME(123456789) = 0x%016X" % crc64nvme(b"123456789"))
    sys.exit(0)
