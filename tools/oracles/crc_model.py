#!/usr/bin/env python3
"""An independent CRC model for tests of lib/std's Crc32c and Crc64Nvme (ADR 0075 §2.5).

The Rocksoft parameter model ("A Painless Guide to CRC Error Detection Algorithms", Williams, 1993, §15), computed
bit by bit, most significant bit first, with the polynomial in its normal (unreflected) form. Reflected input is
handled by reversing each byte before it is shifted in, and reflected output by reversing the register at the end. It
shares nothing with tools/gen_checksums.py, which builds reflected lookup tables (least significant bit first).

Each model is checked against its catalogued check value, the CRC of the ASCII bytes "123456789" (Greg Cook's CRC
catalogue: CRC-32/ISCSI and CRC-64/NVME), and against Python's zlib.crc32 for the CRC-32 (ISO-HDLC) parameters, when
this file is run.

usage: python3 tools/oracles/crc_model.py   (prints the check values; exits non-zero on a mismatch)
"""

import sys
import zlib


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


def crc32c(data):
    """CRC-32/ISCSI (CRC-32C): width 32, poly 0x1EDC6F41, init and xorout all ones, reflected."""
    return crc(data, 32, 0x1EDC6F41, 0xFFFFFFFF, True, True, 0xFFFFFFFF)


def crc64nvme(data):
    """CRC-64/NVME: width 64, poly 0xAD93D23594C93659, init and xorout all ones, reflected."""
    return crc(data, 64, 0xAD93D23594C93659, 0xFFFFFFFFFFFFFFFF, True, True, 0xFFFFFFFFFFFFFFFF)


def self_check():
    """The catalogue's check values, and zlib's CRC-32 through the same model: a failure raises."""
    check = b"123456789"
    assert crc32c(check) == 0xE3069283, hex(crc32c(check))
    assert crc64nvme(check) == 0xAE8B14860A799888, hex(crc64nvme(check))
    for n in (0, 1, 7, 64, 200):
        data = bytes((k * 151 + n) & 0xFF for k in range(n))
        assert crc(data, 32, 0x04C11DB7, 0xFFFFFFFF, True, True, 0xFFFFFFFF) == zlib.crc32(data), n


if __name__ == "__main__":
    self_check()
    print("CRC-32C(123456789) = 0x%08X" % crc32c(b"123456789"))
    print("CRC-64/NVME(123456789) = 0x%016X" % crc64nvme(b"123456789"))
    sys.exit(0)
