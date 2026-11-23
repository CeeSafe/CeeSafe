#!/usr/bin/env python3
"""Snappy's raw block format (ADR 0196 phase 2), modelled from snappy's format_description.txt, for the oracles of
lib/std's `Snappy`: a decoder that says what a block decodes to or that it is refused, a writer of elements by hand,
and an encoder that writes every kind of element (literals with every length tag, copies with 1, 2 and 4 byte
offsets, of every length from 1 to 64, overlapping), which libsnappy's own encoder never does.

The model checks itself against the libraries where they are present (`python3 tools/oracles/snappy_model.py`):
libsnappy, the reference implementation, through its C API and `ctypes` (Homebrew's `snappy`, or a Linux package's
`libsnappy.so.1`), and `cramjam` (Rust's `snap`) if it can be imported. Neither is needed to run the checks: the
expected values the corpus holds were generated with them, and tools/codec_check.py uses whichever it finds.
"""

import ctypes
import ctypes.util
import os
import random
import sys

# ---------------------------------------------------------------------------------------------------------------
# The model.


def preamble(block):
    """(length, bytes the varint took), or None: at most five bytes, and at most 32 bits (libsnappy's
    Varint::Parse32WithLimit: the fifth byte below 16)."""
    value = 0
    for k in range(min(5, len(block))):
        b = block[k]
        if k == 4 and b >= 16:
            return None
        value |= (b & 127) << (7 * k)
        if b < 128:
            return value, k + 1
    return None


def elements(body, n):
    """The bytes `body`'s elements make, which must be exactly `n`, or None."""
    out = bytearray()
    at = 0
    while at < len(body):
        tag = body[at]
        at += 1
        kind = tag & 3
        if kind == 0:
            length = (tag >> 2) + 1
            if length > 60:
                extra = length - 60
                if len(body) - at < extra:
                    return None
                length = int.from_bytes(body[at:at + extra], "little") + 1
                at += extra
            if length > n - len(out) or length > len(body) - at:
                return None
            out += body[at:at + length]
            at += length
        else:
            if kind == 1:
                if len(body) - at < 1:
                    return None
                length = 4 + ((tag >> 2) & 7)
                offset = ((tag >> 5) << 8) | body[at]
                at += 1
            elif kind == 2:
                if len(body) - at < 2:
                    return None
                length = (tag >> 2) + 1
                offset = int.from_bytes(body[at:at + 2], "little")
                at += 2
            else:
                if len(body) - at < 4:
                    return None
                length = (tag >> 2) + 1
                offset = int.from_bytes(body[at:at + 4], "little")
                at += 4
            if offset == 0 or offset > len(out) or length > n - len(out):
                return None
            for _ in range(length):
                out.append(out[-offset])
    return bytes(out) if len(out) == n else None


def decode(block):
    """The bytes a block decodes to, or None if it is refused."""
    p = preamble(block)
    if p is None:
        return None
    n, size = p
    return elements(block[size:], n)


def verdict(block, limit):
    """What lib/std's Snappy.decompress must say: 'ok <length> <crc32>', 'TooLarge' or 'Malformed'. A stated length
    above the limit is TooLarge whatever follows it."""
    import zlib
    p = preamble(block)
    if p is None:
        return "Malformed"
    if p[0] > limit:
        return "TooLarge"
    out = decode(block)
    if out is None:
        return "Malformed"
    return f"ok {len(out)} {zlib.crc32(out)}"


# ---------------------------------------------------------------------------------------------------------------
# Writing elements by hand.


def varint(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128)
        n >>= 7
    out.append(n)
    return bytes(out)


def literal(data, extra=None):
    """A literal of `data`; `extra` forces that many bytes of length (1 to 4) where the tag alone would do."""
    n = len(data) - 1
    if extra is None:
        extra = 0 if n < 60 else 1 if n < 256 else 2 if n < 65536 else 3 if n < 1 << 24 else 4
    if extra == 0:
        return bytes([n << 2]) + data
    return bytes([(59 + extra) << 2]) + n.to_bytes(extra, "little") + data


def copy1(offset, length):
    assert 4 <= length <= 11 and offset < 2048
    return bytes([1 | ((length - 4) << 2) | ((offset >> 8) << 5), offset & 255])


def copy2(offset, length):
    assert 1 <= length <= 64 and offset < 65536
    return bytes([2 | ((length - 1) << 2)]) + offset.to_bytes(2, "little")


def copy4(offset, length):
    assert 1 <= length <= 64 and offset < 1 << 32
    return bytes([3 | ((length - 1) << 2)]) + offset.to_bytes(4, "little")


def variety(data, seed):
    """`data` as a block of every kind of element, chosen at random: matches found by a hash of three bytes, split
    into copies of random lengths (1 to 64) with any tag their offset fits, and literals of random lengths, some with
    more length bytes than they need."""
    rng = random.Random(seed)
    out = bytearray(varint(len(data)))
    last = {}
    pending = 0
    at = 0

    def flush_literal(end):
        nonlocal pending
        start = pending
        while start < end:
            n = min(end - start, rng.choice((1, 3, 59, 60, 61, 200, 256, 257, 4000, 70000)))
            piece = bytes(data[start:start + n])
            need = 0 if n - 1 < 60 else 1 if n - 1 < 256 else 2 if n - 1 < 65536 else 3
            extra = need if rng.random() < 0.8 else rng.randint(max(need, 1), 4)
            out.extend(literal(piece, extra))
            start += n
        pending = end

    while at < len(data):
        key = bytes(data[at:at + 3])
        cand = last.get(key) if len(key) == 3 else None
        if len(key) == 3:
            last[key] = at
        if cand is None:
            at += 1
            continue
        m = 0
        while at + m < len(data) and data[cand + m] == data[at + m]:
            m += 1
        flush_literal(at)
        offset = at - cand
        left = m
        while left > 0:
            n = min(left, rng.choice((1, 2, 3, 4, 7, 11, 12, 32, 63, 64)))
            choices = [copy4]
            if offset < 65536:
                choices.append(copy2)
            if 4 <= n <= 11 and offset < 2048:
                choices.append(copy1)
            out.extend(rng.choice(choices)(offset, n))
            left -= n
        for k in range(at + 1, min(at + m, len(data) - 2)):
            last[bytes(data[k:k + 3])] = k
        at += m
        pending = at
    flush_literal(len(data))
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------
# The libraries.

SNAPPY_OK = 0


class LibSnappy:
    """libsnappy's C API (snappy-c.h) through ctypes."""

    def __init__(self, path):
        lib = ctypes.CDLL(path)
        size_p = ctypes.POINTER(ctypes.c_size_t)
        lib.snappy_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, size_p]
        lib.snappy_uncompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, size_p]
        lib.snappy_max_compressed_length.argtypes = [ctypes.c_size_t]
        lib.snappy_max_compressed_length.restype = ctypes.c_size_t
        lib.snappy_uncompressed_length.argtypes = [ctypes.c_char_p, ctypes.c_size_t, size_p]
        lib.snappy_validate_compressed_buffer.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        self.lib = lib
        self.path = path

    def compress(self, data):
        n = ctypes.c_size_t(self.lib.snappy_max_compressed_length(len(data)))
        buf = ctypes.create_string_buffer(n.value or 1)
        assert self.lib.snappy_compress(data, len(data), buf, ctypes.byref(n)) == SNAPPY_OK
        return buf.raw[:n.value]

    def decompress(self, block):
        """The bytes, or None if libsnappy refuses the block (validated first, so a hostile length allocates
        nothing)."""
        if self.lib.snappy_validate_compressed_buffer(block, len(block)) != SNAPPY_OK:
            return None
        n = ctypes.c_size_t(0)
        if self.lib.snappy_uncompressed_length(block, len(block), ctypes.byref(n)) != SNAPPY_OK:
            return None
        buf = ctypes.create_string_buffer(n.value or 1)
        if self.lib.snappy_uncompress(block, len(block), buf, ctypes.byref(n)) != SNAPPY_OK:
            return None
        return buf.raw[:n.value]


def libsnappy():
    """libsnappy, or None if it is not installed."""
    candidates = [ctypes.util.find_library("snappy"), "/opt/homebrew/lib/libsnappy.dylib",
                  "/usr/local/lib/libsnappy.dylib", "/usr/lib/x86_64-linux-gnu/libsnappy.so.1",
                  "/usr/lib/aarch64-linux-gnu/libsnappy.so.1", "/usr/lib64/libsnappy.so.1", "/usr/lib/libsnappy.so.1"]
    for path in candidates:
        if path and (os.path.exists(path) or not os.path.isabs(path)):
            try:
                return LibSnappy(path)
            except OSError:
                pass
    return None


class Cramjam:
    """cramjam's raw snappy (Rust's `snap`)."""

    def __init__(self, module):
        self.m = module
        self.path = f"cramjam {module.__version__}"

    def compress(self, data):
        return bytes(self.m.snappy.compress_raw(data))

    def decompress(self, block):
        try:
            return bytes(self.m.snappy.decompress_raw(block))
        except Exception:  # noqa: BLE001  cramjam raises its own DecompressionError
            return None


def cramjam():
    try:
        import cramjam as m
    except ImportError:
        return None
    return Cramjam(m)


def oracles():
    return [o for o in (libsnappy(), cramjam()) if o is not None]


# ---------------------------------------------------------------------------------------------------------------
# The model against the libraries.


def hostile(rng, block):
    """A mutant of `block`."""
    b = bytearray(block)
    for _ in range(rng.randint(1, 4)):
        kind = rng.randrange(6)
        if not b or kind == 0:
            b.insert(rng.randint(0, len(b)), rng.randrange(256))
        elif kind == 1:
            b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
        elif kind == 2:
            b[rng.randrange(len(b))] = rng.randrange(256)
        elif kind == 3:
            del b[rng.randrange(len(b))]
        elif kind == 4:
            del b[rng.randrange(len(b)):]
        else:
            b[min(len(b) - 1, rng.randrange(6))] = rng.randrange(256)
    return bytes(b)


def self_check(count=20000):
    """The model's verdicts against each library's on libsnappy's blocks, the model's own and their mutants."""
    libs = oracles()
    if not libs:
        print("snappy_model: no library found (libsnappy, cramjam); nothing to check against")
        return 0
    rng = random.Random(1)
    disagree = {o.path: 0 for o in libs}
    blocks = 0
    for k in range(count):
        n = rng.choice((0, 1, 5, 64, 300, 5000))
        data = bytes(rng.choice(b"abcde") if rng.random() < 0.7 else rng.randrange(256) for _ in range(n))
        block = libs[0].compress(data) if k % 2 == 0 else variety(data, k)
        assert decode(block) == data, "the model misreads a block"
        for b in (block, hostile(rng, block)):
            blocks += 1
            ours = decode(b)
            for o in libs:
                if o.decompress(b) != ours:
                    disagree[o.path] += 1
                    if disagree[o.path] <= 5:
                        print(f"snappy_model: {o.path} disagrees on {b.hex()[:96]}: the model {'refuses' if ours is None else 'decodes'} it")
    for o in libs:
        print(f"snappy_model: {blocks} blocks, {o.path}: {disagree[o.path]} disagreements")
    return disagree[libs[0].path]


if __name__ == "__main__":
    sys.exit(1 if self_check() else 0)
