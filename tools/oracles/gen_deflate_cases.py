#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/deflate.cee: lib/std's Deflate, Zlib, Gzip, Inflater, Deflater and
Adler32 (ADR 0196 phase 1) against oracles outside the code under test: Python's `zlib` (and `gzip`), which writes
the streams the program decodes and says what each decodes to, or that it refuses it.

The cases:
1. zlib's streams of a few shapes and sizes, raw, zlib and gzip, at several levels and strategies, window bits 9 and
   15, with flushes mid-stream; each decoded whole, a byte at a time, and in uneven steps, and the output's length
   and CRC-32 (zlib.crc32) printed;
2. RFC 1951 streams written here bit by bit: stored blocks (empty, LEN/NLEN mismatch, cut short), fixed blocks with
   every length code and every distance code at their least and most extra bits, length 258 by code 284 and by 285,
   distances of exactly 32,768, overlapping copies of every distance from 1 to 9, length codes 286 and 287 and
   distance codes 30 and 31, a distance before the start; dynamic blocks with every refusal of ADR 0196 §2.5 (an
   incomplete or over-subscribed code, a single one-bit distance code accepted and a two-bit one refused, too many
   codes, a repeat with nothing to repeat or past the end, no end-of-block code, a distance with no distance code);
   every one is first decoded by zlib, whose verdict the expected line is;
3. RFC 1950: a bad header check, method and window, a preset dictionary (FDICT: `Unsupported` without it, decoded
   with `setWindow`, `ChecksumMismatch` with another), a wrong Adler-32, bytes after the stream;
4. RFC 1952: two members, FEXTRA, FNAME, FCOMMENT and FHCRC built here, a wrong header CRC, CRC-32 and ISIZE,
   reserved flags, bad magic, bytes after a member, an empty input;
5. the limit: exactly the output, one byte less, and 0;
6. round trips of lib/std's own encoder at every level and format, whole and streamed with sync flushes (the input
   is the oracle), and a sync flush decoded before the stream ends;
7. Adler32 (zlib.adler32).

The corpus program is short: the interpreter runs every corpus program, and it decodes some hundred kilobytes a
second. The long lists (every level, strategy and window, inputs around every block size, zlib decoding lib/std's
output at every level) are tools/codec_check.py's, compiled only.

usage: python3 tools/oracles/gen_deflate_cases.py   (formats with $CEESAFEC, default out/ceesafec)
"""

import os
import subprocess
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUTPUT = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "deflate.cee")

WORDS = [b"alpha", b"beta", b"gamma", b"delta", b"epsilon", b"zeta", b"eta", b"theta"]


def message(n, seed):
    """Byte k is (k * 167 + seed * 13 + (k >> 7)) mod 256, as the program's `message`."""
    return bytes((k * 167 + seed * 13 + (k >> 7)) & 0xFF for k in range(n))


def words(n, seed):
    """Words chosen by a linear congruential generator, as the program's `words`: compressible text."""
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += WORDS[(x >> 16) & 7]
        out += b" " if (x >> 8) & 3 else b"\n"
    return bytes(out[:n])


def zeros(n, seed):
    return bytes(n)


SHAPES = {"message": message, "words": words, "zeros": zeros}


# ---------------------------------------------------------------------------------------------------------------
# Writing RFC 1951 by hand.

def reverse(code, length):
    r = 0
    for _ in range(length):
        r = (r << 1) | (code & 1)
        code >>= 1
    return r


class Bits:
    """Bits written least significant first; Huffman codes most significant first (RFC 1951 §3.1.1)."""

    def __init__(self):
        self.value = 0
        self.count = 0
        self.out = bytearray()

    def put(self, value, n):
        self.value |= value << self.count
        self.count += n
        while self.count >= 8:
            self.out.append(self.value & 0xFF)
            self.value >>= 8
            self.count -= 8

    def code(self, code, length):
        self.put(reverse(code, length), length)

    def align(self):
        if self.count:
            self.put(0, 8 - self.count)

    def raw(self, data):
        assert self.count == 0
        self.out += data

    def done(self):
        self.align()
        return bytes(self.out)


LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
               195, 227, 258]
LENGTH_EXTRA = [0] * 8 + [1] * 4 + [2] * 4 + [3] * 4 + [4] * 4 + [5] * 4 + [0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
             4097, 6145, 8193, 12289, 16385, 24577]
DIST_EXTRA = [0, 0, 0, 0] + [k // 2 for k in range(2, 28)]


def length_code(length):
    for c in range(28, -1, -1):
        if length >= LENGTH_BASE[c]:
            if c == 28 and length != 258:
                continue
            return c, length - LENGTH_BASE[c]
    raise ValueError(length)


def dist_code(dist):
    for c in range(29, -1, -1):
        if dist >= DIST_BASE[c]:
            return c, dist - DIST_BASE[c]
    raise ValueError(dist)


def fixed_lit(bits, s):
    if s < 144:
        bits.code(0x30 + s, 8)
    elif s < 256:
        bits.code(0x190 + s - 144, 9)
    elif s < 280:
        bits.code(s - 256, 7)
    else:
        bits.code(0xC0 + s - 280, 8)


def stored(bits, data, final):
    bits.put(1 if final else 0, 1)
    bits.put(0, 2)
    bits.align()
    bits.raw(bytes([len(data) & 0xFF, len(data) >> 8, ~len(data) & 0xFF, (~len(data) >> 8) & 0xFF]))
    bits.raw(data)


def fixed(bits, symbols, final):
    """A fixed block: ('lit', b), ('match', length, dist) or ('code', literal/length symbol, extra value, extra
    bits, distance symbol or None, extra value, extra bits) for codes the encoder never writes."""
    bits.put(1 if final else 0, 1)
    bits.put(1, 2)
    for s in symbols:
        if s[0] == "lit":
            fixed_lit(bits, s[1])
        elif s[0] == "match":
            c, x = length_code(s[1])
            fixed_lit(bits, 257 + c)
            bits.put(x, LENGTH_EXTRA[c])
            d, y = dist_code(s[2])
            bits.code(d, 5)
            bits.put(y, DIST_EXTRA[d])
        else:
            _, ls, lx, ln, ds, dx, dn = s
            fixed_lit(bits, ls)
            bits.put(lx, ln)
            if ds is not None:
                bits.code(ds, 5)
                bits.put(dx, dn)
    fixed_lit(bits, 256)


def canonical(lengths):
    """Canonical codes (RFC 1951 §3.2.2) for `lengths`, most significant bit first."""
    counts = [0] * 16
    for length in lengths:
        if length:
            counts[length] += 1
    nxt = [0] * 16
    code = 0
    for b in range(1, 16):
        code = (code + counts[b - 1]) << 1
        nxt[b] = code
    codes = [0] * len(lengths)
    for s, length in enumerate(lengths):
        if length:
            codes[s] = nxt[length]
            nxt[length] += 1
    return codes


CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
# A complete code length code for all 19 symbols: 13 of 4 bits and 6 of 5.
CL_LENGTHS = [4] * 13 + [5] * 6


def dynamic(bits, lit, dist, symbols, final, sequence=None, cl=None, hlit=None, hdist=None):
    """A dynamic block with literal/length code lengths `lit` and distance code lengths `dist`, its code lengths
    written as `sequence` ((symbol, extra) pairs; by default each length as itself) in the code length code `cl`;
    then `symbols` as `fixed` takes them, in the block's codes."""
    cl = cl or CL_LENGTHS
    nlen = hlit if hlit is not None else len(lit)
    ndist = hdist if hdist is not None else len(dist)
    if sequence is None:
        sequence = [(x, 0) for x in list(lit) + list(dist)]
    bits.put(1 if final else 0, 1)
    bits.put(2, 2)
    bits.put(nlen - 257, 5)
    bits.put(ndist - 1, 5)
    bits.put(19 - 4, 4)
    for s in CL_ORDER:
        bits.put(cl[s], 3)
    cl_codes = canonical(cl)
    for s, x in sequence:
        bits.code(cl_codes[s], cl[s])
        bits.put(x, {16: 2, 17: 3, 18: 7}.get(s, 0))
    lit_codes = canonical(lit)
    dist_codes = canonical(dist)
    for s in symbols:
        if s[0] == "lit":
            bits.code(lit_codes[s[1]], lit[s[1]])
        elif s[0] == "match":
            c, x = length_code(s[1])
            bits.code(lit_codes[257 + c], lit[257 + c])
            bits.put(x, LENGTH_EXTRA[c])
            d, y = dist_code(s[2])
            bits.code(dist_codes[d], dist[d])
            bits.put(y, DIST_EXTRA[d])
    if lit[256]:
        bits.code(lit_codes[256], lit[256])


def lengths(n, given):
    v = [0] * n
    for s, length in given.items():
        v[s] = length
    return v


# ---------------------------------------------------------------------------------------------------------------
# The cases. A decoding case is (label, format, stream, limit, expected), the expected line's tail "ok <n> <crc>" or
# an error's name; a dictionary case adds the dictionary.

def zlib_stream(data, fmt, level=6, wbits=15, strategy=0, flush=None, zdict=None):
    wb = {"raw": -wbits, "zlib": wbits, "gzip": 16 + wbits}[fmt]
    if zdict is None:
        c = zlib.compressobj(level, zlib.DEFLATED, wb, 9, strategy)
    else:
        c = zlib.compressobj(level, zlib.DEFLATED, wb, 9, strategy, zdict)
    out = b""
    if flush is not None:
        step = max(1, len(data) // 3)
        for i in range(0, len(data), step):
            out += c.compress(data[i:i + step]) + c.flush(flush)
    else:
        out += c.compress(data)
    return out + c.flush()


def zlib_verdict(stream, fmt, limit, zdict=None):
    """What Python's zlib makes of `stream`: ('ok', bytes) or ('refused', reason). Bytes after a raw or zlib stream,
    which zlib leaves as `unused_data`, are a refusal here (ADR 0196 §2.2); gzip members are decoded in turn."""
    wb = {"raw": -15, "zlib": 15, "gzip": 31}[fmt]
    out = b""
    rest = stream
    if not rest:
        return "refused", "empty"
    while True:
        d = zlib.decompressobj(wb) if zdict is None else zlib.decompressobj(wb, zdict)
        try:
            out += d.decompress(rest)
        except zlib.error as e:
            return "refused", str(e)
        if not d.eof:
            return "refused", "cut short"
        rest = d.unused_data
        if not rest:
            break
        if fmt != "gzip":
            return "refused", "bytes after the stream"
    if len(out) > limit:
        return "refused", "limit"
    return "ok", out


def ok(data):
    return f"ok {len(data)} {zlib.crc32(data)}"


def decode_case(label, fmt, stream, limit=None, error=None, zdict=None):
    """A case whose expected line is zlib's verdict; `error` names our refusal, which zlib must agree is one."""
    verdict, value = zlib_verdict(stream, fmt, 1 << 30, zdict)
    if limit is None:
        limit = (len(value) if verdict == "ok" else 1 << 20) + 16
    if error is None:
        assert verdict == "ok", (label, value)
        assert len(value) <= limit
        return (label, fmt, stream, limit, ok(value))
    if error != "TooLarge":
        assert verdict == "refused", (label, "zlib accepts it")
    return (label, fmt, stream, limit, error)


def corpus_cases():
    cases = []
    # 1. zlib's streams: every format at three levels; raw deflate's strategies, window and flushes.
    for shape, n in [("words", 0), ("words", 1500), ("message", 300), ("zeros", 1000)]:
        data = SHAPES[shape](n, 1)
        for fmt, levels in (("raw", (0, 6, 9)), ("zlib", (1, 6)), ("gzip", (1, 6))):
            for level in levels:
                cases.append(decode_case(f"zlib {fmt} {shape} {n} level {level}", fmt, zlib_stream(data, fmt, level)))
        if shape == "words" and n > 0:
            for name, strat in (("filtered", zlib.Z_FILTERED), ("huffman", zlib.Z_HUFFMAN_ONLY), ("rle", zlib.Z_RLE),
                                ("fixed", zlib.Z_FIXED)):
                cases.append(decode_case(f"zlib raw {shape} {n} {name}", "raw", zlib_stream(data, "raw", 6, 15, strat)))
            cases.append(decode_case(f"zlib zlib {shape} {n} window 9", "zlib", zlib_stream(data, "zlib", 9, 9)))
            cases.append(decode_case(f"zlib raw {shape} {n} sync flushes", "raw",
                                     zlib_stream(data, "raw", 6, 15, 0, zlib.Z_SYNC_FLUSH)))
            cases.append(decode_case(f"zlib gzip {shape} {n} full flushes", "gzip",
                                     zlib_stream(data, "gzip", 6, 15, 0, zlib.Z_FULL_FLUSH)))
    cases += rfc1951_cases(short=True)
    cases += rfc1950_cases()
    cases += rfc1952_cases()
    cases += limit_cases()
    return cases


def rfc1951_cases(short):
    cases = []
    b = Bits()
    stored(b, b"", True)
    cases.append(decode_case("stored empty", "raw", b.done()))
    b = Bits()
    stored(b, b"abc", False)
    stored(b, words(300, 4), True)
    cases.append(decode_case("stored two blocks", "raw", b.done()))
    cases.append(decode_case("stored LEN/NLEN mismatch", "raw", bytes([1, 3, 0, 0xFC, 0xFE, 97, 98, 99]),
                             error="Malformed"))
    cases.append(decode_case("stored cut short", "raw", bytes([1, 5, 0, 0xFA, 0xFF, 97, 98]), error="Malformed"))
    cases.append(decode_case("block type 3", "raw", bytes([7, 0]), error="Malformed"))
    cases.append(decode_case("fixed empty", "raw", bytes([3, 0])))
    cases.append(decode_case("bytes after the final block", "raw", bytes([3, 0, 0]), error="Malformed"))
    cases.append(decode_case("no final block", "raw", bytes([2, 0]), error="Malformed"))
    cases.append(decode_case("empty input", "raw", b"", error="Malformed"))
    # CosmoStore's inflateRefusals (its src/s3/inflate_tests.cee), which lib/std's decoder takes over.
    cosmo = bytes.fromhex("edcac91180201404d15426354514d78f086ed16b7932883e76f5cbc16b2dbd1b55273b16b5766a2873dc64bb4fca"
                          "ef9eaafb5263dd17582c168bc562b158ecdf3e")
    cases.append(decode_case("CosmoStore dynamic, exactly the limit", "raw", cosmo, limit=2640))
    cases.append(decode_case("CosmoStore dynamic, a byte past the limit", "raw", cosmo, limit=2639, error="TooLarge"))
    cases.append(decode_case("CosmoStore stored, cut short", "raw", bytes.fromhex("01780087ff"), error="Malformed"))
    cases.append(decode_case("CosmoStore stored, wrong complement", "raw", bytes.fromhex("0178008800"),
                             error="Malformed"))
    cases.append(decode_case("CosmoStore dynamic, cut short", "raw", cosmo[:10], error="Malformed"))
    cases.append(decode_case("CosmoStore a distance before the start", "raw", bytes.fromhex("0302"), error="Malformed"))
    # Every length code at its least and most, at distance 1, and 258 by code 284 (227 + 31) and by 285.
    symbols = [("lit", 97)]
    for c in range(29):
        for x in sorted({0, (1 << LENGTH_EXTRA[c]) - 1}):
            symbols.append(("code", 257 + c, x, LENGTH_EXTRA[c], 0, 0, 0))
    b = Bits()
    fixed(b, symbols, True)
    cases.append(decode_case("every length code", "raw", b.done()))
    b = Bits()
    fixed(b, [("lit", 98), ("code", 284, 31, 5, 0, 0, 0), ("code", 285, 0, 0, 0, 0, 0)], True)
    cases.append(decode_case("length 258 by 284 and 285", "raw", b.done()))
    # Overlapping copies: every distance from 1 to 9, length 100.
    b = Bits()
    syms = [("lit", 48 + k) for k in range(9)]
    for d in range(1, 10):
        syms.append(("match", 100, d))
        syms.append(("lit", 65 + d))
    fixed(b, syms, True)
    cases.append(decode_case("overlapping copies", "raw", b.done()))
    # Every distance code at its least and most extra, after 32 KiB of history, and exactly 32,768.
    prefix = message(32768, 3) if not short else words(32768, 3)
    b = Bits()
    stored(b, prefix[:16384], False)
    stored(b, prefix[16384:], False)
    syms = []
    for c in range(30):
        for y in sorted({0, (1 << DIST_EXTRA[c]) - 1}):
            syms.append(("code", 257, 0, 0, c, y, DIST_EXTRA[c]))
    syms.append(("match", 258, 32768))
    fixed(b, syms, True)
    cases.append(decode_case("every distance code, 32768 back", "raw", b.done()))
    b = Bits()
    fixed(b, [("lit", 97), ("match", 3, 2)], True)
    cases.append(decode_case("distance before the start", "raw", b.done(), error="Malformed"))
    b = Bits()
    stored(b, words(100, 5), False)
    fixed(b, [("match", 3, 101)], True)
    cases.append(decode_case("distance one before the start", "raw", b.done(), error="Malformed"))
    for s in (286, 287):
        b = Bits()
        fixed(b, [("lit", 97), ("code", s, 0, 0, 0, 0, 0)], True)
        cases.append(decode_case(f"length code {s}", "raw", b.done(), error="Malformed"))
    for d in (30, 31):
        b = Bits()
        fixed(b, [("lit", 97), ("code", 257, 0, 0, d, 0, 0)], True)
        cases.append(decode_case(f"distance code {d}", "raw", b.done(), error="Malformed"))
    # Dynamic blocks.
    lit = lengths(258, {97: 1, 256: 2, 257: 2})
    b = Bits()
    dynamic(b, lit, [1], [("lit", 97), ("match", 3, 1)], True)
    cases.append(decode_case("dynamic, one one-bit distance code", "raw", b.done()))
    b = Bits()
    dynamic(b, lit, [2], [("lit", 97), ("match", 3, 1)], True)
    cases.append(decode_case("dynamic, one two-bit distance code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lengths(257, {256: 1}), [0], [], True)
    cases.append(decode_case("dynamic, only a one-bit end code", "raw", b.done()))
    b = Bits()
    dynamic(b, lengths(257, {256: 2}), [0], [], True)
    cases.append(decode_case("dynamic, only a two-bit end code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lengths(257, {97: 2, 256: 2}), [1], [("lit", 97)], True)
    cases.append(decode_case("dynamic, incomplete literal code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lengths(258, {97: 1, 256: 1, 257: 1}), [1], [("lit", 97)], True)
    cases.append(decode_case("dynamic, over-subscribed literal code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lengths(258, {97: 1, 255: 1}), [1], [("lit", 97)], True)
    cases.append(decode_case("dynamic, no end-of-block code", "raw", b.done(), error="Malformed"))
    lit2 = lengths(258, {97: 1, 256: 2, 98: 2})
    b = Bits()
    dynamic(b, lit2, [0], [("lit", 97), ("lit", 98), ("lit", 97)], True)
    cases.append(decode_case("dynamic, no distance codes, literals", "raw", b.done()))
    b = Bits()
    dynamic(b, lit, [0], [("lit", 97), ("match", 3, 1)], True)
    cases.append(decode_case("dynamic, a length with no distance code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lengths(286, {97: 1, 256: 1}), [1], [], True, hlit=287)
    cases.append(decode_case("dynamic, 287 literal/length codes", "raw", b.done()[:60], error="Malformed"))
    b = Bits()
    dynamic(b, lengths(257, {97: 1, 256: 1}), [1] + [0] * 31, [], True)
    cases.append(decode_case("dynamic, 32 distance codes", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lit, [1], [], True, cl=[2, 2] + [0] * 17, sequence=[])
    cases.append(decode_case("dynamic, incomplete code length code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lit, [1], [], True, cl=[1, 1, 1] + [0] * 16, sequence=[])
    cases.append(decode_case("dynamic, over-subscribed code length code", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lit, [1], [], True, sequence=[(16, 0)])
    cases.append(decode_case("dynamic, a repeat of nothing", "raw", b.done(), error="Malformed"))
    b = Bits()
    dynamic(b, lit, [1], [], True, sequence=[(18, 127), (18, 127), (18, 127)])
    cases.append(decode_case("dynamic, a repeat past the end", "raw", b.done(), error="Malformed"))
    seq = [(0, 0)] * 97 + [(1, 0)] + [(18, 127)] + [(17, 7)] + [(0, 0)] * 10 + [(2, 0), (2, 0)] + [(1, 0)]
    b = Bits()
    dynamic(b, lit, [1], [("lit", 97), ("match", 3, 1)], True, sequence=seq)
    cases.append(decode_case("dynamic, repeats 17 and 18", "raw", b.done()))
    seq = [(0, 0)] * 96 + [(1, 0), (1, 0), (16, 0)] + [(0, 0)] * 155 + [(2, 0), (2, 0), (1, 0)]
    lit3 = lengths(258, {96: 1, 97: 1, 98: 1, 99: 1, 100: 1, 256: 2, 257: 2})
    cases.append(decode_case("dynamic, over-subscribed by a repeat 16", "raw",
                             dyn_bytes(lit3, [1], [("lit", 97)], seq), error="Malformed"))
    return cases


def dyn_bytes(lit, dist, symbols, seq):
    b = Bits()
    dynamic(b, lit, dist, symbols, True, sequence=seq)
    return b.done()


def rfc1950_cases():
    cases = []
    data = words(500, 7)
    good = zlib_stream(data, "zlib")
    cases.append(decode_case("zlib header check", "zlib", bytes([0x78, 0x9D]) + good[2:], error="Malformed"))
    cases.append(decode_case("zlib method 7", "zlib", bytes([0x77, (31 - (0x77 * 256) % 31) % 31]) + good[2:],
                             error="Malformed"))
    cmf = 0x88  # window 2^16: CINFO 8
    flg = (31 - (cmf * 256) % 31) % 31
    cases.append(decode_case("zlib window 64 KiB", "zlib", bytes([cmf, flg]) + good[2:], error="Malformed"))
    bad = bytearray(good)
    bad[-1] ^= 1
    cases.append(decode_case("zlib wrong Adler-32", "zlib", bytes(bad), error="ChecksumMismatch"))
    cases.append(decode_case("zlib trailer cut short", "zlib", good[:-1], error="Malformed"))
    cases.append(decode_case("zlib byte after the stream", "zlib", good + b"\0", error="Malformed"))
    zdict = words(200, 9)
    with_dict = zlib_stream(words(600, 9), "zlib", 6, 15, 0, None, zdict)
    cases.append(decode_case("zlib preset dictionary, none given", "zlib", with_dict, error="Unsupported"))
    return cases


def gzip_header(flags, extra=b"", name=b"", comment=b"", hcrc_ok=True):
    h = bytearray([0x1F, 0x8B, 8, flags, 0x10, 0x20, 0x30, 0x40, 0, 3])
    if flags & 4:
        h += bytes([len(extra) & 0xFF, len(extra) >> 8]) + extra
    if flags & 8:
        h += name + b"\0"
    if flags & 16:
        h += comment + b"\0"
    if flags & 2:
        crc = zlib.crc32(bytes(h)) & 0xFFFF
        if not hcrc_ok:
            crc ^= 0x100
        h += bytes([crc & 0xFF, crc >> 8])
    return bytes(h)


def gzip_member(data, header, crc_delta=0, size_delta=0):
    body = zlib_stream(data, "raw")
    crc = (zlib.crc32(data) ^ crc_delta) & 0xFFFFFFFF
    size = (len(data) + size_delta) & 0xFFFFFFFF
    return header + body + crc.to_bytes(4, "little") + size.to_bytes(4, "little")


def rfc1952_cases():
    cases = []
    a = words(400, 11)
    c = message(300, 12)
    cases.append(decode_case("gzip two members", "gzip", gzip_member(a, gzip_header(0)) + gzip_member(c, gzip_header(0))))
    cases.append(decode_case("gzip FEXTRA FNAME FCOMMENT FHCRC", "gzip",
                             gzip_member(a, gzip_header(4 | 8 | 16 | 2 | 1, b"AB\x02\x00xy", b"name.txt", b"a comment"))))
    cases.append(decode_case("gzip wrong header CRC", "gzip",
                             gzip_member(a, gzip_header(2 | 8, name=b"n", hcrc_ok=False)), error="ChecksumMismatch"))
    cases.append(decode_case("gzip wrong CRC-32", "gzip", gzip_member(a, gzip_header(0), crc_delta=1),
                             error="ChecksumMismatch"))
    cases.append(decode_case("gzip wrong ISIZE", "gzip", gzip_member(a, gzip_header(0), size_delta=1),
                             error="ChecksumMismatch"))
    cases.append(decode_case("gzip reserved flag", "gzip", gzip_member(a, gzip_header(0x20)), error="Malformed"))
    m = bytearray(gzip_member(a, gzip_header(0)))
    m[1] = 0x8C
    cases.append(decode_case("gzip bad magic", "gzip", bytes(m), error="Malformed"))
    m = bytearray(gzip_member(a, gzip_header(0)))
    m[2] = 7
    cases.append(decode_case("gzip method 7", "gzip", bytes(m), error="Malformed"))
    cases.append(decode_case("gzip bytes after a member", "gzip", gzip_member(a, gzip_header(0)) + b"xyz",
                             error="Malformed"))
    cases.append(decode_case("gzip zeros after a member", "gzip", gzip_member(a, gzip_header(0)) + bytes(8),
                             error="Malformed"))
    cases.append(decode_case("gzip cut in the header", "gzip", gzip_header(8, name=b"abcdef")[:12], error="Malformed"))
    cases.append(decode_case("gzip empty input", "gzip", b"", error="Malformed"))
    return cases


def limit_cases():
    data = words(2000, 13)
    s = zlib_stream(data, "zlib", 6)
    return [
        decode_case("limit exactly the output", "zlib", s, limit=len(data)),
        decode_case("limit a byte short", "zlib", s, limit=len(data) - 1, error="TooLarge"),
        decode_case("limit 0, no output", "raw", bytes([3, 0]), limit=0),
        decode_case("limit 0, a byte", "zlib", zlib_stream(b"x", "zlib"), limit=0, error="TooLarge"),
    ]


def dictionary_cases():
    """(label, format, stream, dictionary given, limit, expected)."""
    zdict = words(200, 9)
    data = words(600, 9)
    z = zlib_stream(data, "zlib", 6, 15, 0, None, zdict)
    r = zlib_stream(data, "raw", 6, 15, 0, None, zdict)
    assert zlib.decompressobj(15, zdict).decompress(z) == data
    return [
        ("dictionary zlib", "zlib", z, zdict, 1000, ok(data)),
        ("dictionary zlib, another", "zlib", z, words(200, 10), 1000, "ChecksumMismatch"),
        ("dictionary raw", "raw", r, zdict, 1000, ok(data)),
        ("dictionary raw, none", "raw", r, None, 1000, "Malformed"),
    ]


# Round trips: (shape, n, seed, format); the program compresses at each level, whole and streamed.
ROUND_TRIPS = [("words", 500, 2, "raw"), ("message", 200, 3, "zlib"), ("zeros", 300, 4, "gzip"), ("words", 0, 1, "gzip")]
ADLER = [("message", 0, 1), ("message", 1, 1), ("message", 5551, 2), ("message", 5552, 3), ("message", 5553, 4),
         ("message", 20000, 5), ("zeros", 70000, 6), ("words", 3000, 7)]


PROGRAM_HEAD = '''// GENERATED by tools/oracles/gen_deflate_cases.py: edit that, not this file.
// lib/std's Deflate, Zlib, Gzip, Inflater, Deflater and Adler32 (ADR 0196 phase 1). Every expected line comes from an
// oracle outside the code under test: Python's zlib wrote the streams decoded here and decoded them first (an "ok"
// line has its output's length and CRC-32), or refused them, and a round trip's oracle is its input. Each stream is
// decoded whole, a byte at a time and in uneven steps. tools/codec_check.py runs the long lists, compiled only.

fn hexDigit(c: u8) -> u8 => if (c >= 97) c - 87 else c - 48;

/// The bytes of hex digits, in pieces.
fn unhex(pieces: Slice<StringView>) -> Vector<u8> {
    var v = Vector<u8>.create();
    for (p in pieces) {
        let b = p.bytes();
        var k: usize = 0;
        while (k + 1 < b.length) {
            v.push((hexDigit(b[k]) << 4) | hexDigit(b[k + 1]))!;
            k += 2;
        }
    }
    return v;
}

fn message(n: usize, seed: usize) -> Vector<u8> {
    var v = Vector<u8>.create();
    for (k in 0..n) {
        v.push(truncate<u8>(k * 167 + seed * 13 + (k >> 7)))!;
    }
    return v;
}

fn words(n: usize, seed: u32) -> Vector<u8> {
    let names = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
    var v = Vector<u8>.create();
    var x = seed;
    while (v.length < n) {
        x = (x.wrappingMul(1103515245).wrappingAdd(12345)) & 0x7FFFFFFF;
        v.extend(names[((x >> 16) & 7) as usize].bytes())!;
        v.push(if (((x >> 8) & 3) != 0) 32u8 else 10u8)!;
    }
    v.truncate(n);
    return v;
}

fn zeros(n: usize) -> Vector<u8> {
    var v = Vector<u8>.create();
    v.resize(n, 0)!;
    return v;
}

fn shape(name: StringView, n: usize, seed: u32) -> Vector<u8> {
    if (name == "message") { return message(n, seed as usize); }
    if (name == "words") { return words(n, seed); }
    return zeros(n);
}

fn formatOf(name: StringView) -> DeflateFormat {
    if (name == "zlib") { return DeflateFormat.Zlib; }
    if (name == "gzip") { return DeflateFormat.Gzip; }
    return DeflateFormat.Raw;
}

fn errorName(e: DecodeError) -> StringView => match (e) {
    DecodeError.Malformed => "Malformed",
    DecodeError.TooLarge => "TooLarge",
    DecodeError.Unsupported => "Unsupported",
    DecodeError.ChecksumMismatch => "ChecksumMismatch",
    DecodeError.OutOfMemory(_) => "OutOfMemory",
};

fn show(label: StringView, how: StringView, r: Result<void, DecodeError>, out: borrow Vector<u8>) {
    Console.write(label);
    Console.write(" ");
    Console.write(how);
    Console.write(" ");
    match (r) {
        Ok() => {
            Console.write("ok ");
            Console.write(out.length);
            Console.write(" ");
            Console.writeLine(Crc32.of(out.view()));
        }
        Err(e) => { Console.writeLine(errorName(e)); }
    }
}

fn whole(format: StringView, input: Slice<u8>, limit: usize, out: borrow mut Vector<u8>) -> Result<void, DecodeError> {
    if (format == "zlib") { return Zlib.decompress(input, limit, out); }
    if (format == "gzip") { return Gzip.decompress(input, limit, out); }
    return Deflate.decompress(input, limit, out);
}

/// A step, in a function with no effect: `Inflater.step` allocates nothing and never blocks.
@effects()
fn stepInto(inflater: borrow mut Inflater, input: Slice<u8>, out: SliceMut<u8>) -> Result<Progress, DecodeError> => inflater.step(input, out);

/// Decodes in steps of up to `stepIn` input bytes and `stepOut` output bytes, sizes varying from step to step.
fn steps(
    inflater: borrow mut Inflater,
    format: StringView,
    input: Slice<u8>,
    stepIn: usize,
    stepOut: usize,
    out: borrow mut Vector<u8>,
) -> Result<void, DecodeError> {
    var buffer = Vector<u8>.create();
    buffer.resize(stepOut, 0)?;
    var at: usize = 0;
    var k: usize = 0;
    while (true) {
        k += 1;
        let si = 1 + (stepIn * (k % 3)) / 2;
        let so = 1 + ((stepOut - 1) * ((k * 7) % 5 + 1)) / 5;
        let end = if (input.length - at < si) input.length else at + si;
        let chunk = input[at..end];
        let p = stepInto(inflater, chunk, mut buffer[0..so])?;
        out.extend(buffer[0..p.written])?;
        at += p.read;
        if (p.done) {
            if (at == input.length) { return Ok(); }
            if (format != "gzip") { return Err(DecodeError.Malformed); }
        } else if (p.read == 0 && p.written == 0 && end == input.length) {
            return Err(DecodeError.Malformed);
        }
    }
    return Ok();
}

fn decode(label: StringView, format: StringView, pieces: Slice<StringView>, limit: usize) {
    let input = unhex(pieces);
    var a = Vector<u8>.create();
    let ra = whole(format, input.view(), limit, mut a);
    show(label, "whole", ra, a);
    if (input.length <= 512) {
        var b = Vector<u8>.create();
        var ib = Inflater.create(formatOf(format), limit as u64)!;
        let rb = steps(mut ib, format, input.view(), 1, 1, mut b);
        show(label, "bytes", rb, b);
    }
    var c = Vector<u8>.create();
    var ic = Inflater.create(formatOf(format), limit as u64)!;
    let rc = steps(mut ic, format, input.view(), 37, 300, mut c);
    show(label, "steps", rc, c);
}

fn decodeWith(label: StringView, format: StringView, pieces: Slice<StringView>, dictionary: Slice<StringView>, limit: usize) {
    let input = unhex(pieces);
    let history = unhex(dictionary);
    var out = Vector<u8>.create();
    var inflater = Inflater.create(formatOf(format), limit as u64)!;
    if (history.length > 0) { inflater.setWindow(history.view()); }
    let r = steps(mut inflater, format, input.view(), 64, 128, mut out);
    show(label, "window", r, out);
}

fn same(a: Slice<u8>, b: Slice<u8>) -> bool => a == b;

fn succeeded(r: Result<void, DecodeError>) -> bool => match (r) {
    Ok() => true,
    Err(_) => false,
};

/// Format `f` at each level: compressed whole and decoded whole; streamed in pieces of 97 with a sync flush after every
/// third, decoded in steps.
fn roundTrip(name: StringView, n: usize, seed: u32, f: StringView) {
    let data = shape(name, n, seed);
    {
        var good: u32 = 0;
        for (level in 0u32..10) {
            var packed = Vector<u8>.create();
            if (f == "zlib") {
                Zlib.compress(data.view(), level, mut packed)!;
            } else if (f == "gzip") {
                Gzip.compress(data.view(), level, mut packed)!;
            } else {
                Deflate.compress(data.view(), level, mut packed)!;
            }
            var back = Vector<u8>.create();
            let r = whole(f, packed.view(), n, mut back);
            if (succeeded(r) && same(back.view(), data.view())) { good += 1; }
            var d = Deflater.create(formatOf(f), level)!;
            var streamed = Vector<u8>.create();
            var at: usize = 0;
            var pieces: usize = 0;
            while (at < n) {
                let end = if (n - at < 97) n else at + 97;
                d.update(data[at..end], mut streamed)!;
                at = end;
                pieces += 1;
                if (pieces % 3 == 0) { d.flush(mut streamed)!; }
            }
            d.finish(mut streamed)!;
            var again = Vector<u8>.create();
            var inflater = Inflater.create(formatOf(f), n as u64)!;
            let r2 = steps(mut inflater, f, streamed.view(), 50, 64, mut again);
            if (succeeded(r2) && same(again.view(), data.view())) { good += 1; }
        }
        Console.write("round trip ");
        Console.write(name);
        Console.write(" ");
        Console.write(n);
        Console.write(" ");
        Console.write(f);
        Console.write(": ");
        Console.write(good);
        Console.writeLine(" of 20");
    }
}

/// A sync flush: what was given before it decodes in full before the stream ends.
fn syncFlush() {
    let data = words(3000, 21);
    var d = Deflater.create(DeflateFormat.Raw, 6)!;
    var packed = Vector<u8>.create();
    d.update(data[0..1000], mut packed)!;
    d.flush(mut packed)!;
    let flushed = packed.length;
    d.update(data[1000..3000], mut packed)!;
    d.finish(mut packed)!;
    var inflater = Inflater.create(DeflateFormat.Raw, 3000)!;
    var buffer = [0u8; 4096];
    let first = packed[0..flushed];
    let p = inflater.step(first, mut buffer[..])!;
    Console.write("sync flush: ");
    Console.write(p.read == flushed && !p.done && p.written == 1000 && same(buffer[0..1000], data[0..1000]));
    let rest = packed[flushed..];
    let q = inflater.step(rest, mut buffer[1000..])!;
    Console.write(" ");
    Console.writeLine(q.done && q.read == rest.length && q.written == 2000 && same(buffer[0..3000], data.view()));
}

fn adler(name: StringView, n: usize, seed: u32) {
    let data = shape(name, n, seed);
    var a = Adler32.create();
    let half = n / 3;
    a.update(data[0..half]);
    a.update(data[half..]);
    Console.write("adler32 ");
    Console.write(name);
    Console.write(" ");
    Console.write(n);
    Console.write(" ");
    Console.write(Adler32.of(data.view()));
    Console.write(" ");
    Console.writeLine(a.finish());
}

fn main() {
    Console.write("adler32 123456789 ");
    Console.writeLine(Adler32.of("123456789".bytes()));
'''


def pieces(data):
    """`data` in hex, as an array of strings of at most 96 digits."""
    text = data.hex()
    parts = [text[k:k + 96] for k in range(0, len(text), 96)] or [""]
    return "[" + ", ".join(f'"{p}"' for p in parts) + "]"


def program(cases, dictionaries, round_trips, adlers):
    """The CeeSafe program and the lines it must print."""
    body = []
    lines = [f"adler32 123456789 {zlib.adler32(b'123456789')}"]
    for name, n, seed in adlers:
        body.append(f'    adler("{name}", {n}, {seed});')
        v = zlib.adler32(SHAPES[name](n, seed))
        lines.append(f"adler32 {name} {n} {v} {v}")
    for k, (label, fmt, stream, limit, expected) in enumerate(cases):
        body.append(f'    let s{k} = {pieces(stream)};')
        body.append(f'    decode("{label}", "{fmt}", s{k}[..], {limit});')
        for how in ("whole", "bytes", "steps") if len(stream) <= 512 else ("whole", "steps"):
            lines.append(f"{label} {how} {expected}")
    for k, (label, fmt, stream, zdict, limit, expected) in enumerate(dictionaries):
        body.append(f'    let w{k} = {pieces(stream)};')
        body.append(f'    let d{k} = {pieces(zdict or b"")};')
        body.append(f'    decodeWith("{label}", "{fmt}", w{k}[..], d{k}[..], {limit});')
        lines.append(f"{label} window {expected}")
    for name, n, seed, f in round_trips:
        body.append(f'    roundTrip("{name}", {n}, {seed}, "{f}");')
        lines.append(f"round trip {name} {n} {f}: 20 of 20")
    body.append("    syncFlush();")
    lines.append("sync flush: true true")
    text = PROGRAM_HEAD + "\n".join(body) + "\n}\n"
    text += "".join(f"// expect-stdout: {line}\n" for line in lines)
    return text, lines


def main():
    text, _ = program(corpus_cases(), dictionary_cases(), ROUND_TRIPS, ADLER)
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", OUTPUT], check=True)
    print(f"gen_deflate_cases: wrote {os.path.relpath(OUTPUT, ROOT)}")


if __name__ == "__main__":
    sys.exit(main())
