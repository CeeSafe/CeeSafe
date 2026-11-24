#!/usr/bin/env python3
"""Zstandard's frame format (RFC 8878; ADR 0196 phase 3), modelled from the RFC, for the oracles of lib/std's `Zstd`
and `ZstdDecoder`: a decoder that says what a frame decodes to or why it is refused, XXH64, a writer of frames and
blocks by hand (raw and RLE blocks, compressed blocks of raw or RLE literals and sequences in RLE mode, whose fields
are written bit by bit), and libzstd through `ctypes`.

Where the RFC leaves a decoder room, libzstd's streaming decoder (`ZSTD_decompressStream`, the one whose memory is
bounded by the window) decides, and the model follows it:
- a block whose content (a raw or RLE block's bytes, a compressed block's input or output) is larger than
  Block_Maximum_Size, the smaller of the window and 128 KiB, is refused. libzstd's one-call `ZSTD_decompress`
  accepts raw and RLE blocks of up to 2^21 - 1 bytes;
- Huffman codes may be 12 bits long (libzstd's HUF_TABLELOG_MAX), though the RFC says 11;
- a compressed literals section with four streams has at least 6 literals, and its header needs 5 bytes in the block;
- a sequences section with no sequences has nothing after its count;
- an offset does not reach before the frame's first byte (no dictionary);
- a reserved bit in the frame header, a reserved block type, and a reserved field of the modes byte are Malformed.
libzstd's stream refuses an RLE block of exactly 128 KiB, which the RFC allows and its one-call decoder reads: the
model reads it (`compare` says "once").

Its own decisions, as lib/std's (ADR 0196 §2):
- it is stricter than libzstd's stream in three places (STRICTER below): each Huffman stream must be used up
  exactly, an offset may reach at most the window back, as RFC 8878 says, and a compressed block of 0 bytes is
  refused, as libzstd's one-call decoder refuses it;
- an empty input is Malformed (libzstd returns no bytes); an input of skippable frames alone decodes to nothing;
- a dictionary ID other than 0 is Unsupported; a window above 2^31 bytes (libzstd's limit) or the stream's window
  limit is TooLarge;
- a content size above the limit is TooLarge, and one above 32,768 times the bytes after the header is Malformed
  (an RLE block of 4 bytes writes at most 128 KiB), both before anything is allocated;
- sequences run as they are decoded, so a refusal comes from the first sequence that has one: its literals must be
  there, it must fit the block's maximum (Malformed) and then the content size (Malformed) or the limit (TooLarge),
  and its offset must be valid; the bit stream's exact end is checked after the last sequence.

The model checks itself against libzstd where it is present (`python3 tools/oracles/zstd_model.py`): Homebrew's
`zstd`, or a Linux system's `libzstd.so.1`, which every recent Debian and Ubuntu has (dpkg uses it).
"""

import ctypes
import ctypes.util
import os
import random
import struct
import sys
import zlib

MAGIC = 0xFD2FB528
BLOCK_MAX = 1 << 17
WINDOW_MAX = 1 << 31


class Refused(Exception):
    """A refusal: `kind` is Malformed, TooLarge, Unsupported or ChecksumMismatch."""

    def __init__(self, kind, why):
        super().__init__(why)
        self.kind = kind
        self.why = why


def malformed(why):
    raise Refused("Malformed", why)


# ---------------------------------------------------------------------------------------------------------------
# XXH64 (xxhash's specification).

P1 = 11400714785074694791
P2 = 14029467366897019727
P3 = 1609587929392839161
P4 = 9650029242287828579
P5 = 2870177450012600261
M64 = (1 << 64) - 1


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _round(acc, lane):
    acc = (acc + lane * P2) & M64
    return (_rotl(acc, 31) * P1) & M64


def _merge(acc, v):
    acc ^= _round(0, v)
    return (acc * P1 + P4) & M64


def xxh64(data, seed=0):
    n = len(data)
    at = 0
    if n >= 32:
        v = [(seed + P1 + P2) & M64, (seed + P2) & M64, seed & M64, (seed - P1) & M64]
        while n - at >= 32:
            for k in range(4):
                v[k] = _round(v[k], struct.unpack_from("<Q", data, at + 8 * k)[0])
            at += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M64
        for k in range(4):
            h = _merge(h, v[k])
    else:
        h = (seed + P5) & M64
    h = (h + n) & M64
    while n - at >= 8:
        h ^= _round(0, struct.unpack_from("<Q", data, at)[0])
        h = (_rotl(h, 27) * P1 + P4) & M64
        at += 8
    if n - at >= 4:
        h ^= (struct.unpack_from("<I", data, at)[0] * P1) & M64
        h = (_rotl(h, 23) * P2 + P3) & M64
        at += 4
    while at < n:
        h ^= (data[at] * P5) & M64
        h = (_rotl(h, 11) * P1) & M64
        at += 1
    h ^= h >> 33
    h = (h * P2) & M64
    h ^= h >> 29
    h = (h * P3) & M64
    h ^= h >> 32
    return h


# ---------------------------------------------------------------------------------------------------------------
# Bit streams.


def highbit(x):
    return x.bit_length() - 1


class Forward:
    """Bits read from the least significant of the first byte on; zeros past the end (FSE_readNCount reads a
    padded copy), and `used()` says how many bytes the bits read took."""

    def __init__(self, data):
        self.data = data
        self.bit = 0

    def read(self, n):
        v = 0
        for k in range(n):
            at = self.bit + k
            byte = self.data[at >> 3] if (at >> 3) < len(self.data) else 0
            v |= ((byte >> (at & 7)) & 1) << k
        self.bit += n
        return v

    def peek(self, n):
        save = self.bit
        v = self.read(n)
        self.bit = save
        return v

    def used(self):
        return (self.bit + 7) >> 3


class Backward:
    """A stream read from its end: the last byte's highest 1 bit marks where it starts (a last byte of 0 is
    Malformed), and the bits below it are read from the most significant down. Reading past the stream's first bit
    gives zeros and counts as overflow."""

    def __init__(self, data):
        if len(data) == 0:
            malformed("an empty bit stream")
        last = data[-1]
        if last == 0:
            malformed("a bit stream whose last byte is 0")
        self.value = int.from_bytes(data, "little")
        self.left = 8 * (len(data) - 1) + highbit(last)  # bits not yet read

    def read(self, n):
        if n == 0:
            return 0
        if self.left >= n:
            self.left -= n
            return (self.value >> self.left) & ((1 << n) - 1)
        # Past the start: the missing low bits are zeros.
        v = (self.value & ((1 << max(self.left, 0)) - 1)) << (n - max(self.left, 0)) if self.left > 0 else 0
        self.left -= n
        return v

    def overflowed(self):
        return self.left < 0

    def finished(self):
        return self.left == 0


# ---------------------------------------------------------------------------------------------------------------
# FSE.


def read_ncount(data, max_symbol, max_log):
    """An FSE table description (RFC 8878 §4.1.1, libzstd's FSE_readNCount): (normalized counts, accuracy log,
    bytes read)."""
    if len(data) == 0:
        malformed("no FSE table description")
    bits = Forward(data)
    log = bits.read(4) + 5
    if log > max_log:
        malformed("an FSE accuracy log above the maximum")
    remaining = (1 << log) + 1
    threshold = 1 << log
    nbits = log + 1
    norm = []
    previous0 = False
    while remaining > 1 and len(norm) <= max_symbol:
        if previous0:
            n0 = len(norm)
            while bits.peek(2) == 3:
                n0 += 3
                bits.read(2)
            n0 += bits.read(2)
            if n0 > max_symbol:
                malformed("FSE zeros past the last symbol")
            while len(norm) < n0:
                norm.append(0)
            if len(norm) > max_symbol:
                break
        low = bits.peek(nbits - 1)
        maximum = (2 * threshold - 1) - remaining
        if low < maximum:
            count = low
            bits.read(nbits - 1)
        else:
            count = bits.peek(nbits)
            if count >= threshold:
                count -= maximum
            bits.read(nbits)
        count -= 1
        remaining -= abs(count)
        norm.append(count)
        previous0 = count == 0
        if remaining < threshold:
            if remaining <= 1:
                break
            nbits = highbit(remaining) + 1
            threshold = 1 << (nbits - 1)
        if len(norm) >= max_symbol + 1:
            break
    if remaining != 1:
        malformed("FSE probabilities that do not add up")
    if len(norm) > max_symbol + 1:
        malformed("FSE symbols past the maximum")
    used = bits.used()
    if used > len(data):
        malformed("an FSE table description cut short")
    return norm, log, used


def fse_table(norm, log):
    """The decoding table: (symbol, bits, baseline) per state."""
    size = 1 << log
    symbols = [0] * size
    high = size - 1
    for s, p in enumerate(norm):
        if p == -1:
            symbols[high] = s
            high -= 1
    step = (size >> 1) + (size >> 3) + 3
    pos = 0
    for s, p in enumerate(norm):
        for _ in range(max(p, 0)):
            symbols[pos] = s
            pos = (pos + step) & (size - 1)
            while pos > high:
                pos = (pos + step) & (size - 1)
    if pos != 0:
        malformed("an FSE table that does not fill")
    counters = [1 if p == -1 else max(p, 0) for p in norm]
    table = []
    for state in range(size):
        s = symbols[state]
        x = counters[s]
        counters[s] += 1
        nb = log - highbit(x)
        table.append((s, nb, (x << nb) - size))
    return table


# The predefined distributions (RFC 8878 §3.1.1.3.2.2).
LL_DEFAULT = [4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
              -1, -1, -1, -1]
ML_DEFAULT = [1, 4, 3] + [2] * 6 + [1] * 37 + [-1] * 7
OF_DEFAULT = [1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1]

LL_BASE = list(range(16)) + [16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384,
                             32768, 65536]
LL_BITS = [0] * 16 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
ML_BASE = list(range(3, 35)) + [35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195,
                                16387, 32771, 65539]
ML_BITS = [0] * 32 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
LL_MAX, ML_MAX, OF_MAX = 35, 52, 31
LL_LOG, ML_LOG, OF_LOG = 9, 9, 8


# ---------------------------------------------------------------------------------------------------------------
# Huffman.


def read_weights(data):
    """A Huffman tree description (RFC 8878 §4.2.1): (weights of every symbol, the implied last one included; bytes
    read)."""
    if len(data) == 0:
        malformed("no Huffman tree description")
    header = data[0]
    if header >= 128:
        n = header - 127
        size = (n + 1) // 2
        if size + 1 > len(data):
            malformed("Huffman weights cut short")
        weights = []
        for k in range(n):
            b = data[1 + k // 2]
            weights.append(b >> 4 if k % 2 == 0 else b & 15)
        used = size + 1
    else:
        if header + 1 > len(data):
            malformed("compressed Huffman weights cut short")
        body = data[1:1 + header]
        norm, log, hsize = read_ncount(body, 255, 6)
        table = fse_table(norm, log)
        bits = Backward(body[hsize:])
        s1 = bits.read(log)
        s2 = bits.read(log)
        weights = []
        states = [s1, s2]
        turn = 0
        while True:
            if len(weights) > 253:
                malformed("too many Huffman weights")
            sym, nb, base = table[states[turn]]
            weights.append(sym)
            states[turn] = base + bits.read(nb)
            if bits.overflowed():
                weights.append(table[states[1 - turn]][0])
                break
            turn = 1 - turn
        used = header + 1
    if any(w > 12 for w in weights):
        malformed("a Huffman weight above 12")
    total = sum((1 << w) >> 1 for w in weights)
    if total == 0:
        malformed("Huffman weights all 0")
    log = highbit(total) + 1
    if log > 12:
        malformed("a Huffman code longer than 12 bits")
    rest = (1 << log) - total
    if rest & (rest - 1):
        malformed("Huffman weights whose last is not a power of 2")
    weights.append(highbit(rest) + 1)
    if sum(1 for w in weights if w == 1) < 2 or sum(1 for w in weights if w == 1) % 2:
        malformed("Huffman weights with an odd number of the longest codes")
    return weights, log, used


def huffman_table(weights, log):
    """A table of 2^log entries, (symbol, bits), indexed by the next `log` bits."""
    table = [None] * (1 << log)
    # Codes are assigned by weight, lowest weight (longest code) first, and within a weight by symbol.
    position = 0
    for w in range(1, log + 1):
        for s, ws in enumerate(weights):
            if ws == w:
                span = (1 << (w - 1))
                for k in range(span):
                    table[position + k] = (s, log + 1 - w)
                position += span
    return table


def huffman_stream(data, table, log, n):
    bits = Backward(data)
    out = bytearray()
    for _ in range(n):
        save = bits.left
        sym, nb = table[bits.read(log)]
        bits.left = save - nb
        out.append(sym)
    if not bits.finished():
        malformed("a Huffman stream not used up exactly")
    return out


# ---------------------------------------------------------------------------------------------------------------
# Frames.


class Stats:
    """What the decoded frames used, for coverage."""

    def __init__(self):
        self.seen = {}

    def add(self, what):
        self.seen[what] = self.seen.get(what, 0) + 1


class FrameState:
    def __init__(self, window):
        self.window = window
        self.block_max = min(window, BLOCK_MAX)
        self.huffman = None
        self.tables = [None, None, None]  # LL, OF, ML: the last used, for Repeat mode
        self.reps = [1, 4, 8]


def frame_header(data, at):
    """(header fields dict, where the blocks begin)."""
    if len(data) - at < 5:
        malformed("a frame header cut short")
    fhd = data[at + 4]
    fcs_flag = fhd >> 6
    single = (fhd >> 5) & 1
    checksum = (fhd >> 2) & 1
    dict_flag = fhd & 3
    if fhd & 8:
        malformed("the frame header's reserved bit")
    size = 1 + (0 if single else 1) + [0, 1, 2, 4][dict_flag] + [single, 2, 4, 8][fcs_flag]
    if len(data) - (at + 4) < size:
        malformed("a frame header cut short")
    p = at + 5
    window = None
    if not single:
        wd = data[p]
        p += 1
        log = 10 + (wd >> 3)
        window = (1 << log) + ((1 << log) >> 3) * (wd & 7)
        if log > 31:
            raise Refused("TooLarge", "a window above 2^31")
    dict_id = int.from_bytes(data[p:p + [0, 1, 2, 4][dict_flag]], "little")
    p += [0, 1, 2, 4][dict_flag]
    fcs = None
    nfcs = [single, 2, 4, 8][fcs_flag]
    if nfcs:
        fcs = int.from_bytes(data[p:p + nfcs], "little")
        if nfcs == 2:
            fcs += 256
        p += nfcs
    if single:
        window = fcs
    return {"window": window, "dict": dict_id, "fcs": fcs, "checksum": checksum, "single": single}, p


def literals(block, st, stats):
    """(literals, bytes read) of a compressed block's literals section."""
    b0 = block[0]
    kind = b0 & 3
    sf = (b0 >> 2) & 3
    if kind < 2:
        if sf in (0, 2):
            n, h = b0 >> 3, 1
        elif sf == 1:
            if len(block) < 2:
                malformed("a literals header cut short")
            n, h = (b0 >> 4) + (block[1] << 4), 2
        else:
            if len(block) < 3:
                malformed("a literals header cut short")
            n, h = (b0 >> 4) + (block[1] << 4) + (block[2] << 12), 3
        if n > st.block_max:
            malformed("more literals than a block holds")
        if kind == 0:
            stats.add("literals raw")
            if h + n > len(block):
                malformed("raw literals cut short")
            return bytes(block[h:h + n]), h + n
        stats.add("literals RLE")
        if h + 1 > len(block):
            malformed("RLE literals cut short")
        return bytes([block[h]]) * n, h + 1
    if len(block) < 5:
        malformed("a compressed block too short for its literals header")
    streams = 1 if sf == 0 else 4
    if sf < 2:
        v = int.from_bytes(block[0:3], "little")
        n, c, h = (v >> 4) & 1023, (v >> 14) & 1023, 3
    elif sf == 2:
        v = int.from_bytes(block[0:4], "little")
        n, c, h = (v >> 4) & 16383, (v >> 18) & 16383, 4
    else:
        v = int.from_bytes(block[0:5], "little")
        n, c, h = (v >> 4) & 262143, (v >> 22) & 262143, 5
    if n > st.block_max:
        malformed("more literals than a block holds")
    if streams == 4 and n < 6:
        malformed("four Huffman streams of fewer than 6 literals")
    if h + c > len(block):
        malformed("compressed literals cut short")
    body = block[h:h + c]
    if kind == 2:
        weights, log, used = read_weights(body)
        st.huffman = (huffman_table(weights, log), log)
        stats.add("literals Huffman, %d stream%s" % (streams, "s" if streams > 1 else ""))
    else:
        if st.huffman is None:
            malformed("treeless literals with no table before")
        used = 0
        stats.add("literals treeless, %d stream%s" % (streams, "s" if streams > 1 else ""))
    table, log = st.huffman
    data = body[used:]
    if streams == 1:
        return bytes(huffman_stream(data, table, log, n)), h + c
    if len(data) < 10:
        malformed("four Huffman streams in fewer than 10 bytes")
    s1, s2, s3 = struct.unpack_from("<HHH", data, 0)
    total = len(data) - 6
    if s1 + s2 + s3 > total:
        malformed("Huffman stream sizes past the section")
    s4 = total - s1 - s2 - s3
    seg = (n + 3) // 4
    if 3 * seg > n:
        malformed("too few literals for four streams")
    out = bytearray()
    at = 6
    for k, size in enumerate((s1, s2, s3, s4)):
        count = seg if k < 3 else n - 3 * seg
        out += huffman_stream(data[at:at + size], table, log, count)
        at += size
    return bytes(out), h + c


def seq_table(kind, data, at, which, st, stats):
    """The decoding table of one sequence field: (table, bytes read). Entries are (symbol, bits, baseline)."""
    name = ("literal lengths", "offsets", "match lengths")[which]
    maximum = (LL_MAX, OF_MAX, ML_MAX)[which]
    if kind == 0:
        stats.add("sequences predefined " + name)
        default = (LL_DEFAULT, OF_DEFAULT, ML_DEFAULT)[which]
        t = fse_table(default, (6, 5, 6)[which])
    elif kind == 1:
        stats.add("sequences RLE " + name)
        if at >= len(data):
            malformed("an RLE sequence table cut short")
        if data[at] > maximum:
            malformed("an RLE sequence symbol past the maximum")
        return [(data[at], 0, 0)], 1
    elif kind == 2:
        stats.add("sequences FSE " + name)
        norm, log, used = read_ncount(data[at:], maximum, (LL_LOG, OF_LOG, ML_LOG)[which])
        t = fse_table(norm, log)
        st.tables[which] = t
        return t, used
    else:
        stats.add("sequences repeat " + name)
        if st.tables[which] is None:
            malformed("a repeated sequence table with none before")
        return st.tables[which], 0
    st.tables[which] = t
    return t, 0


class Bound:
    """How much more a frame may write: up to its content size if it states one (past it is Malformed), else up to
    the limit (past it is TooLarge)."""

    def __init__(self, end, kind):
        self.end = end
        self.kind = kind

    def check(self, out, block_start, block_max, n):
        """Refuses writing `n` more bytes at the end of `out`: past the block's maximum first, then past the end."""
        if len(out) - block_start + n > block_max:
            malformed("a block larger than its maximum")
        if len(out) + n > self.end:
            raise Refused(self.kind, "output past the content size" if self.kind == "Malformed" else
                          "output past the limit")


def sequences(block, lits, out, frame_start, st, bound, stats):
    """Decodes the sequences section at the start of `block` and runs each sequence as it is decoded (as lib/std
    does, so a refusal comes from the first sequence that has one), then the literals after the last; appends the
    block's output to `out`. For each sequence: its literals must be there, it must fit the block and the bound, and
    its offset must reach neither before the frame nor past the window. The bit stream must be used up exactly,
    which is checked after the last sequence and before the last literals."""
    start = len(out)
    if len(block) < 1:
        malformed("no sequences section")
    b0 = block[0]
    if b0 < 128:
        n, at = b0, 1
    elif b0 < 255:
        if len(block) < 2:
            malformed("a sequence count cut short")
        n, at = ((b0 - 128) << 8) + block[1], 2
    else:
        if len(block) < 3:
            malformed("a sequence count cut short")
        n, at = block[1] + (block[2] << 8) + 0x7F00, 3
    lp = 0
    if n == 0:
        if len(block) != at:
            malformed("bytes after a sequences section of no sequences")
    else:
        if at >= len(block):
            malformed("no modes byte")
        modes = block[at]
        at += 1
        if modes & 3:
            malformed("the modes byte's reserved bits")
        tables = []
        for which, shift in ((0, 6), (1, 4), (2, 2)):
            kind = (modes >> shift) & 3
            t, used = seq_table(kind, block, at, which, st, stats)
            if kind == 1:
                st.tables[which] = t
            at += used
            tables.append(t)
        stats.add("sequences %s" % ("one" if n == 1 else "several"))
        bits = Backward(block[at:])
        logs = [highbit(len(t)) for t in tables]
        state = [bits.read(logs[0]), bits.read(logs[1]), bits.read(logs[2])]  # LL, OF, ML
        reps = st.reps
        for k in range(n):
            ll_sym = tables[0][state[0]][0]
            of_sym = tables[1][state[1]][0]
            ml_sym = tables[2][state[2]][0]
            ov = (1 << of_sym) + bits.read(of_sym)
            ml = ML_BASE[ml_sym] + bits.read(ML_BITS[ml_sym])
            ll = LL_BASE[ll_sym] + bits.read(LL_BITS[ll_sym])
            if ov > 3:
                offset = ov - 3
                reps[:] = [offset, reps[0], reps[1]]
            else:
                stats.add("repeat offsets")
                idx = ov - 1 + (1 if ll == 0 else 0)
                if idx == 0:
                    offset = reps[0]
                elif idx == 1:
                    offset = reps[1]
                    reps[:] = [offset, reps[0], reps[2]]
                elif idx == 2:
                    offset = reps[2]
                    reps[:] = [offset, reps[0], reps[1]]
                else:
                    offset = reps[0] - 1
                    reps[:] = [offset, reps[0], reps[1]]
            if lp + ll > len(lits):
                malformed("a sequence past the literals")
            bound.check(out, start, st.block_max, ll + ml)
            out += lits[lp:lp + ll]
            lp += ll
            if offset == 0 or offset > len(out) - frame_start:
                malformed("an offset before the frame")
            if offset > st.window:
                malformed("an offset past the window")
            for _ in range(ml):
                out.append(out[-offset])
            if k < n - 1:
                for which in (0, 2, 1):
                    sym, nb, base = tables[which][state[which]]
                    state[which] = base + bits.read(nb)
        if not bits.finished():
            malformed("a sequences bit stream not used up exactly")
    bound.check(out, start, st.block_max, len(lits) - lp)
    out += lits[lp:]


def decode(data, limit=1 << 62, window_limit=WINDOW_MAX, stats=None):
    """The bytes all of `data`'s frames decode to; raises Refused."""
    stats = stats or Stats()
    if len(data) == 0:
        malformed("no frame")
    out = bytearray()
    at = 0
    while at < len(data):
        if len(data) - at < 4:
            malformed("a magic number cut short")
        magic = struct.unpack_from("<I", data, at)[0]
        if magic & 0xFFFFFFF0 == 0x184D2A50:
            stats.add("skippable frame")
            if len(data) - at < 8:
                malformed("a skippable frame cut short")
            size = struct.unpack_from("<I", data, at + 4)[0]
            if len(data) - at - 8 < size:
                malformed("a skippable frame cut short")
            at += 8 + size
            continue
        if magic != MAGIC:
            malformed("not a frame")
        h, at = frame_header(data, at)
        stats.add("frame" + (" checksum" if h["checksum"] else "") + (" size" if h["fcs"] is not None else "") +
                  (" single segment" if h["single"] else ""))
        if h["dict"] != 0:
            raise Refused("Unsupported", "a dictionary")
        if h["window"] > window_limit:
            raise Refused("TooLarge", "a window above the window limit")
        frame_start = len(out)
        if h["fcs"] is not None:
            if h["fcs"] > limit - len(out):
                raise Refused("TooLarge", "a content size above the limit")
            if h["fcs"] > (len(data) - at) * 32768:
                malformed("a content size no frame of this size can have")
            bound = Bound(frame_start + h["fcs"], "Malformed")
        else:
            bound = Bound(limit, "TooLarge")
        st = FrameState(h["window"])
        while True:
            if len(data) - at < 3:
                malformed("a block header cut short")
            bh = int.from_bytes(data[at:at + 3], "little")
            at += 3
            last = bh & 1
            kind = (bh >> 1) & 3
            size = bh >> 3
            if kind == 3:
                malformed("a reserved block type")
            if size > st.block_max:
                malformed("a block larger than its maximum")
            if kind == 0:
                stats.add("block raw")
                bound.check(out, len(out), st.block_max, size)
                if len(data) - at < size:
                    malformed("a raw block cut short")
                out += data[at:at + size]
                at += size
            elif kind == 1:
                stats.add("block RLE")
                bound.check(out, len(out), st.block_max, size)
                if len(data) - at < 1:
                    malformed("an RLE block cut short")
                out += bytes([data[at]]) * size
                at += 1
            else:
                stats.add("block compressed")
                if len(data) - at < size:
                    malformed("a compressed block cut short")
                block = data[at:at + size]
                at += size
                if size == 0:
                    malformed("an empty compressed block")
                if size < 2:
                    malformed("a compressed block too short")
                lits, used = literals(block, st, stats)
                sequences(block[used:], lits, out, frame_start, st, bound, stats)
            if last:
                break
        if h["fcs"] is not None and len(out) - frame_start != h["fcs"]:
            malformed("output that disagrees with the content size")
        if h["checksum"]:
            if len(data) - at < 4:
                malformed("a checksum cut short")
            want = struct.unpack_from("<I", data, at)[0]
            at += 4
            if xxh64(bytes(out[frame_start:])) & 0xFFFFFFFF != want:
                raise Refused("ChecksumMismatch", "the content checksum")
    return bytes(out)


def verdict(data, limit, window_limit=WINDOW_MAX):
    """What lib/std must say of `data` with `limit`: 'ok <length> <crc32>' or the refusal's kind."""
    try:
        out = decode(data, limit, window_limit)
    except Refused as e:
        return e.kind
    if len(out) > limit:
        return "TooLarge"
    return f"ok {len(out)} {zlib.crc32(out)}"


def try_decode(data, limit=1 << 62, window_limit=WINDOW_MAX):
    """The bytes, or None if refused."""
    try:
        return decode(data, limit, window_limit)
    except Refused:
        return None


# ---------------------------------------------------------------------------------------------------------------
# Writing frames by hand.


def frame_head(window_log=None, fcs=None, checksum=False, dict_id=None, single=False, fcs_bytes=None,
               window_mantissa=0, reserved=False):
    """A frame's magic and header. `fcs_bytes` forces the content size's field width (0, 1, 2, 4 or 8)."""
    if fcs_bytes is None:
        if fcs is None:
            fcs_bytes = 0
        elif single and fcs < 256:
            fcs_bytes = 1
        elif 256 <= fcs < 65536 + 256:
            fcs_bytes = 2
        elif fcs < (1 << 32):
            fcs_bytes = 4
        else:
            fcs_bytes = 8
    fcs_flag = {0: 0, 1: 0, 2: 1, 4: 2, 8: 3}[fcs_bytes]
    dict_bytes = 0 if dict_id is None else (1 if dict_id < 256 else 2 if dict_id < 65536 else 4)
    dict_flag = {0: 0, 1: 1, 2: 2, 4: 3}[dict_bytes]
    fhd = (fcs_flag << 6) | ((1 if single else 0) << 5) | ((1 if checksum else 0) << 2) | dict_flag
    if reserved:
        fhd |= 8
    out = bytearray(struct.pack("<IB", MAGIC, fhd))
    if not single:
        out.append(((window_log - 10) << 3) | window_mantissa)
    if dict_bytes:
        out += dict_id.to_bytes(dict_bytes, "little")
    if fcs_bytes:
        out += (fcs - (256 if fcs_bytes == 2 else 0)).to_bytes(fcs_bytes, "little")
    return bytes(out)


def block_head(last, kind, size):
    return (int(last) | (kind << 1) | (size << 3)).to_bytes(3, "little")


def raw_block(data, last=True):
    return block_head(last, 0, len(data)) + data


def rle_block(byte, n, last=True):
    return block_head(last, 1, n) + bytes([byte])


def skippable(payload, nibble=0):
    return struct.pack("<II", 0x184D2A50 | nibble, len(payload)) + payload


def checksum_of(data):
    return struct.pack("<I", xxh64(data) & 0xFFFFFFFF)


class BitWriter:
    """Fields in the order a decoder reads them, written backward: the stream's last field first."""

    def __init__(self):
        self.fields = []

    def put(self, value, n):
        assert 0 <= value < (1 << n) or n == 0
        self.fields.append((value, n))

    def bytes(self):
        acc = 0
        pos = 0
        for value, n in reversed(self.fields):
            acc |= value << pos
            pos += n
        acc |= 1 << pos
        pos += 1
        return acc.to_bytes((pos + 7) // 8, "little")


def literals_raw(lits):
    n = len(lits)
    if n < 32:
        return bytes([n << 3]) + lits
    if n < 4096:
        return (0 | (1 << 2) | (n << 4)).to_bytes(2, "little") + lits
    return (0 | (3 << 2) | (n << 4)).to_bytes(3, "little") + lits


def literals_rle(byte, n):
    if n < 32:
        return bytes([1 | (n << 3), byte])
    if n < 4096:
        return (1 | (1 << 2) | (n << 4)).to_bytes(2, "little") + bytes([byte])
    return (1 | (3 << 2) | (n << 4)).to_bytes(3, "little") + bytes([byte])


def code_of(value, base, bits):
    """The code whose range holds `value`: (code, extra bits)."""
    for c in range(len(base) - 1, -1, -1):
        if value >= base[c]:
            assert value - base[c] < (1 << bits[c]), (value, c)
            return c, value - base[c]
    raise ValueError(value)


def sequences_rle(seqs, ll_code=None, of_code=None, ml_code=None):
    """A sequences section in RLE mode for all three fields: every sequence's codes must be the same (each is the
    first sequence's unless given); the extra bits carry the values. `seqs` are (literal length, offset value, match
    length), the offset value as coded (1 to 3 repeats, else the offset + 3)."""
    n = len(seqs)
    if n < 128:
        head = bytes([n])
    elif n < 0x7F00:
        head = bytes([128 + (n >> 8), n & 255])
    else:
        head = bytes([255]) + (n - 0x7F00).to_bytes(2, "little")
    if n == 0:
        return head
    codes = []
    w = BitWriter()
    lc = ll_code if ll_code is not None else code_of(seqs[0][0], LL_BASE, LL_BITS)[0]
    oc = of_code if of_code is not None else highbit(seqs[0][1])
    mc = ml_code if ml_code is not None else code_of(seqs[0][2], ML_BASE, ML_BITS)[0]
    for ll, ov, ml in seqs:
        w.put(ov - (1 << oc), oc)
        w.put(ml - ML_BASE[mc], ML_BITS[mc])
        w.put(ll - LL_BASE[lc], LL_BITS[lc])
    codes = bytes([(1 << 6) | (1 << 4) | (1 << 2), lc, oc, mc])
    return head + codes + w.bytes()


def huffman_codes(weights):
    """Each symbol's (code, bits) for weights (the implied last one included), as the decoder's table assigns them."""
    total = sum((1 << w) >> 1 for w in weights)
    log = highbit(total)
    codes = {}
    position = 0
    for w in range(1, log + 1):
        for s, ws in enumerate(weights):
            if ws == w:
                span = 1 << (w - 1)
                nbits = log + 1 - w
                codes[s] = (position >> (w - 1), nbits)
                position += span
    return codes


def huffman_stream_bytes(lits, codes, pad_bits=0):
    """A backward stream of `lits`' codes; `pad_bits` extra zero bits before the end mark leave it not used up."""
    w = BitWriter()
    for b in lits:
        code, nbits = codes[b]
        w.put(code, nbits)
    if pad_bits:
        # Read last by the decoder: after its last code, bits it never reads.
        w.fields.append((0, pad_bits))
    return w.bytes()


def literals_huffman(lits, weights, streams=1, header_weights=None, treeless=False, pad_bits=0, regen=None):
    """A compressed literals section of `lits`, with a tree description of direct (4-bit) weights for symbols 0 to
    len(weights) - 2, the last one's implied, or none if `treeless`. `header_weights` writes other weights than the
    code uses (to make hostile descriptions); `regen` states another literal count."""
    codes = huffman_codes(weights)
    explicit = header_weights if header_weights is not None else weights[:-1]
    tree = b""
    if not treeless:
        packed = bytearray()
        for k in range(0, len(explicit), 2):
            hi = explicit[k]
            lo = explicit[k + 1] if k + 1 < len(explicit) else 0
            packed.append((hi << 4) | lo)
        tree = bytes([127 + len(explicit)]) + bytes(packed)
    if streams == 1:
        body = tree + huffman_stream_bytes(lits, codes, pad_bits)
    else:
        seg = (len(lits) + 3) // 4
        parts = [lits[0:seg], lits[seg:2 * seg], lits[2 * seg:3 * seg], lits[3 * seg:]]
        enc = [huffman_stream_bytes(p, codes, pad_bits if k == 3 else 0) for k, p in enumerate(parts)]
        body = tree + struct.pack("<HHH", len(enc[0]), len(enc[1]), len(enc[2])) + b"".join(enc)
    n = len(lits) if regen is None else regen
    c = len(body)
    kind = 3 if treeless else 2
    assert n < 1024 and c < 1024
    sf = 0 if streams == 1 else 1
    return (kind | (sf << 2) | (n << 4) | (c << 14)).to_bytes(3, "little") + body


def compressed_block(body, last=True):
    return block_head(last, 2, len(body)) + body


# ---------------------------------------------------------------------------------------------------------------
# libzstd.


class Buffer(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


class LibZstd:
    """libzstd's API (zstd.h) through ctypes."""

    def __init__(self, path):
        lib = ctypes.CDLL(path)
        sz = ctypes.c_size_t
        vp = ctypes.c_void_p
        lib.ZSTD_versionString.restype = ctypes.c_char_p
        lib.ZSTD_isError.argtypes = [sz]
        lib.ZSTD_isError.restype = ctypes.c_uint
        lib.ZSTD_getErrorName.argtypes = [sz]
        lib.ZSTD_getErrorName.restype = ctypes.c_char_p
        lib.ZSTD_compressBound.argtypes = [sz]
        lib.ZSTD_compressBound.restype = sz
        lib.ZSTD_createCCtx.restype = vp
        lib.ZSTD_freeCCtx.argtypes = [vp]
        lib.ZSTD_CCtx_setParameter.argtypes = [vp, ctypes.c_int, ctypes.c_int]
        lib.ZSTD_CCtx_setParameter.restype = sz
        lib.ZSTD_CCtx_setPledgedSrcSize.argtypes = [vp, ctypes.c_ulonglong]
        lib.ZSTD_compress2.argtypes = [vp, ctypes.c_char_p, sz, ctypes.c_char_p, sz]
        lib.ZSTD_compress2.restype = sz
        lib.ZSTD_compressStream2.argtypes = [vp, ctypes.POINTER(Buffer), ctypes.POINTER(Buffer), ctypes.c_int]
        lib.ZSTD_compressStream2.restype = sz
        lib.ZSTD_createDCtx.restype = vp
        lib.ZSTD_freeDCtx.argtypes = [vp]
        lib.ZSTD_DCtx_setParameter.argtypes = [vp, ctypes.c_int, ctypes.c_int]
        lib.ZSTD_DCtx_setParameter.restype = sz
        lib.ZSTD_DCtx_reset.argtypes = [vp, ctypes.c_int]
        lib.ZSTD_decompressStream.argtypes = [vp, ctypes.POINTER(Buffer), ctypes.POINTER(Buffer)]
        lib.ZSTD_decompressStream.restype = sz
        lib.ZSTD_decompress.argtypes = [ctypes.c_char_p, sz, ctypes.c_char_p, sz]
        lib.ZSTD_decompress.restype = sz
        self.lib = lib
        self.path = path
        self.version = lib.ZSTD_versionString().decode()
        self.dctx = lib.ZSTD_createDCtx()

    def compress(self, data, level=3, checksum=False, content_size=True, window_log=0, long=False, streamed=False,
                 block_size=0, piece=None):
        """A frame of `data`. `streamed` writes it with ZSTD_compressStream2 in pieces without stating the size
        (unless `content_size` and the size is pledged)."""
        lib = self.lib
        c = lib.ZSTD_createCCtx()
        try:
            lib.ZSTD_CCtx_setParameter(c, 100, level)  # ZSTD_c_compressionLevel
            lib.ZSTD_CCtx_setParameter(c, 201, 1 if checksum else 0)  # ZSTD_c_checksumFlag
            lib.ZSTD_CCtx_setParameter(c, 200, 1 if content_size else 0)  # ZSTD_c_contentSizeFlag
            if window_log:
                lib.ZSTD_CCtx_setParameter(c, 101, window_log)  # ZSTD_c_windowLog
            if long:
                lib.ZSTD_CCtx_setParameter(c, 160, 1)  # ZSTD_c_enableLongDistanceMatching
            if block_size:
                lib.ZSTD_CCtx_setParameter(c, 1004, block_size)  # ZSTD_c_maxBlockSize (experimental)
            cap = lib.ZSTD_compressBound(len(data)) + 64
            if not streamed:
                buf = ctypes.create_string_buffer(cap)
                n = lib.ZSTD_compress2(c, buf, cap, data, len(data))
                assert not lib.ZSTD_isError(n), lib.ZSTD_getErrorName(n)
                return buf.raw[:n]
            if content_size:
                lib.ZSTD_CCtx_setPledgedSrcSize(c, len(data))
            out = bytearray()
            src = ctypes.create_string_buffer(data, len(data) + 1)
            dst = ctypes.create_string_buffer(cap)
            at = 0
            piece = piece or 4096
            while True:
                end = min(len(data), at + piece)
                ib = Buffer(ctypes.cast(src, ctypes.c_void_p).value + at, end - at, 0)
                mode = 2 if end == len(data) else 0  # ZSTD_e_end, ZSTD_e_continue
                while True:
                    ob = Buffer(ctypes.cast(dst, ctypes.c_void_p), cap, 0)
                    r = lib.ZSTD_compressStream2(c, ctypes.byref(ob), ctypes.byref(ib), mode)
                    assert not lib.ZSTD_isError(r), lib.ZSTD_getErrorName(r)
                    out += dst.raw[:ob.pos]
                    if (mode == 2 and r == 0) or (mode == 0 and ib.pos == ib.size):
                        break
                at = end
                if mode == 2:
                    return bytes(out)
        finally:
            lib.ZSTD_freeCCtx(c)

    def decompress(self, data, limit=1 << 26, window_log_max=31):
        """libzstd's streaming decoder's reading of all of `data`: the bytes, or None if it refuses it or the output
        passes `limit`. An empty input is refused, as lib/std refuses it."""
        if len(data) == 0:
            return None
        lib = self.lib
        d = self.dctx
        lib.ZSTD_DCtx_reset(d, 1)  # ZSTD_reset_session_only
        lib.ZSTD_DCtx_setParameter(d, 100, window_log_max)  # ZSTD_d_windowLogMax
        src = ctypes.create_string_buffer(data, len(data))
        cap = 1 << 17
        dst = ctypes.create_string_buffer(cap)
        ib = Buffer(ctypes.cast(src, ctypes.c_void_p), len(data), 0)
        out = bytearray()
        while True:
            ob = Buffer(ctypes.cast(dst, ctypes.c_void_p), cap, 0)
            r = lib.ZSTD_decompressStream(d, ctypes.byref(ob), ctypes.byref(ib))
            if lib.ZSTD_isError(r):
                return None
            out += dst.raw[:ob.pos]
            if len(out) > limit:
                return None
            if ib.pos == ib.size:
                # All input taken: r == 0 means at a frame's end, even if the output was filled exactly. Otherwise
                # the stream is called again while it writes (it may hold a block's input and have given only part
                # of its output); a call that writes nothing means the input was cut short.
                if r == 0:
                    return bytes(out)
                if ob.pos == 0:
                    return None

    def decompress_once(self, data, capacity):
        """libzstd's one-call decoder: the bytes, or None."""
        buf = ctypes.create_string_buffer(max(1, capacity))
        n = self.lib.ZSTD_decompress(buf, capacity, data, len(data))
        if self.lib.ZSTD_isError(n):
            return None
        return buf.raw[:n]


def libzstd():
    """libzstd, or None if it is not installed."""
    candidates = [ctypes.util.find_library("zstd"), "/opt/homebrew/lib/libzstd.dylib", "/usr/local/lib/libzstd.dylib",
                  "/usr/lib/x86_64-linux-gnu/libzstd.so.1", "/usr/lib/aarch64-linux-gnu/libzstd.so.1",
                  "/usr/lib64/libzstd.so.1", "/usr/lib/libzstd.so.1"]
    for path in candidates:
        if path and (os.path.exists(path) or not os.path.isabs(path)):
            try:
                return LibZstd(path)
            except (OSError, AttributeError):
                pass
    return None


# ---------------------------------------------------------------------------------------------------------------
# The model against libzstd.


def hostile(rng, frame):
    """A mutant of `frame`: bytes inserted, flipped, replaced, deleted, the tail cut, or the header changed."""
    b = bytearray(frame)
    for _ in range(rng.randint(1, 3)):
        kind = rng.randrange(7)
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
        elif kind == 5:
            b[min(len(b) - 1, 4 + rng.randrange(8))] ^= 1 << rng.randrange(8)
        else:
            k = rng.randrange(len(b))
            b[k] = (b[k] + rng.choice((1, 255))) & 255
    return bytes(b)


def sample(rng, n):
    kind = rng.randrange(4)
    if kind == 0:
        return bytes(rng.choice(b"abcdefgh") if rng.random() < 0.8 else rng.randrange(256) for _ in range(n))
    if kind == 1:
        words = [bytes(rng.choice(b"etaoinshr") for _ in range(rng.randint(1, 8))) for _ in range(30)]
        out = bytearray()
        while len(out) < n:
            out += rng.choice(words) + b" "
        return bytes(out[:n])
    if kind == 2:
        out = bytearray()
        while len(out) < n:
            out += bytes([rng.randrange(4)]) * rng.randint(1, 100)
        return bytes(out[:n])
    return bytes(rng.randrange(256) for _ in range(n))


# Where the model is stricter than libzstd's decoder, by design: its fast Huffman loops (1.5.x) do not check that each
# literals stream is used up exactly, as the RFC requires and its reference loop checks, so it reads some corrupt
# literals sections as garbage that a checksum, if the frame has one, then catches; and its stream's buffer holds more
# than the window, so it takes an offset a little past the window, which RFC 8878 §3.1.1.1.2 bounds by the window and
# lib/std's ring (the window and one block) cannot reach; and its stream reads a compressed block of 0 bytes as an
# empty block, which its one-call decoder refuses, as the model does (a compressed block holds a literals section and a
# sequences section). No encoder writes any of these.
STRICTER = ("a Huffman stream not used up exactly", "an offset past the window", "an empty compressed block")


def compare(lib, frame, limit=1 << 26):
    """None if libzstd and the model read `frame` the same; else 'stricter' (a refusal of STRICTER that libzstd
    does not make), 'once' (a frame libzstd's stream refuses and its one-call decoder reads as the model does), or a
    description of the disagreement."""
    try:
        ours = decode(frame, limit)
        why = None
    except Refused as e:
        ours = None
        why = e.why
    theirs = lib.decompress(frame, limit)
    if ours == theirs:
        return None
    if ours is None and why in STRICTER:
        return "stricter"
    if theirs is None and ours is not None and lib.decompress_once(frame, max(1, len(ours))) == ours:
        # A frame libzstd's stream refuses and its one call reads. Phase 3 counted RLE blocks of exactly 128 KiB here,
        # but that was this file's reading of the stream (LibZstd.decompress stopped when the last call filled its
        # 128 KiB of output exactly, ADR 0196 phase 4): the stream reads them.
        return "once"
    return (f"libzstd {'refuses' if theirs is None else 'decodes'} a frame the model "
            f"{'refuses (' + why + ')' if ours is None else 'decodes'}: {frame.hex()[:120]}")


def self_check(count=6000):
    """The model's verdicts against libzstd's streaming decoder on libzstd's frames and their mutants."""
    lib = libzstd()
    if lib is None:
        print("zstd_model: libzstd not found; nothing to check against")
        return 0
    rng = random.Random(3)
    frames = 0
    disagree = 0
    stricter = 0
    stats = Stats()
    for k in range(count):
        n = rng.choice((0, 1, 5, 64, 300, 5000, 40000))
        data = sample(rng, n)
        level = rng.choice((1, 3, 9, 19, -5))
        frame = lib.compress(data, level, checksum=rng.random() < 0.5, content_size=rng.random() < 0.7,
                             window_log=rng.choice((0, 0, 10, 11, 17)), streamed=rng.random() < 0.3,
                             piece=rng.choice((100, 1000, 5000)))
        assert decode(frame, stats=stats) == data, "the model misreads a frame"
        for f in (frame, hostile(rng, frame)):
            frames += 1
            c = compare(lib, f)
            if c in ("stricter", "once"):
                stricter += 1
            elif c is not None:
                disagree += 1
                if disagree <= 8:
                    print("zstd_model: " + c)
    print(f"zstd_model: {frames} frames, libzstd {lib.version}: {disagree} disagreements, and {stricter} corrupt "
          f"frames the model refuses as STRICTER says or reads as libzstd's one call does")
    print("zstd_model: covered: " + ", ".join(f"{k} {v}" for k, v in sorted(stats.seen.items())))
    return disagree


if __name__ == "__main__":
    sys.exit(1 if self_check(int(sys.argv[1]) if len(sys.argv) > 1 else 6000) else 0)
