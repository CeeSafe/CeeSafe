#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/zstd.cee: lib/std's Zstd and ZstdDecoder (ADR 0196 phase 3) against
oracles outside the code under test: libzstd, the reference implementation, writes the frames the program decodes
and reads every frame first, and tools/oracles/zstd_model.py, a decoder written from RFC 8878, agrees with it on each
(the generator stops if they differ, except where the model is stricter by design: STRICTER there). Each expected line
is the model's verdict; XXH64's expected values are the model's, which libzstd's checksums confirm.

The cases:
1. libzstd's frames of a few shapes and sizes at levels -5, 1, 3, 9 and 19 and with long-distance matching, with and
   without a content size and a checksum, written whole or streamed in pieces with small windows, so that between
   them they hold every block type (raw, RLE, compressed), every literals mode (raw, RLE, Huffman in 1 and 4 streams,
   treeless in 1 and 4) and every sequence table mode (predefined, RLE, FSE, repeat), repeated offsets, several
   blocks, and single-segment frames (the generator checks this coverage);
2. frames written by hand (zstd_model.py's writers): empty input, skippable frames alone, before and after a frame
   and cut short; several frames; a header cut short, its reserved bit, a dictionary ID (Unsupported) and an ID of 0,
   a window above 2 GiB, a content size above the limit, one no frame of its size can produce, and output short of or
   past it; raw and RLE blocks at and past the block maximum (the window, or 128 KiB), a reserved block type, a block
   cut short; checksums right, wrong and cut short; bytes after a frame;
3. compressed blocks written by hand: raw and RLE literals; Huffman literals (direct weights) in 1 and 4 streams,
   treeless after them and with no table before, four streams of fewer than 6 literals, a weight above 12, weights
   that do not make a complete code, a stream not used up (the model is stricter than libzstd's fast loops there);
   sequences in RLE mode: an offset exactly to the frame's start and one byte before it, past the window, each
   repeated offset with and without literals, a repeated offset of 0, a sequence past the literals, a block past its
   maximum, a modes byte's reserved bits, a repeated table with none before, an FSE table description whose counts
   do not add up and one whose log is too large, bytes after a section of no sequences;
4. the limit: exactly the output, one byte less, 0; and a ZstdDecoder whose window limit is below the frame's window;
5. zstd's golden decompression files of at most 1 KiB and 64 KiB of output (tools/oracles/zstd_golden, Zstandard's, under its BSD
   licence): read, or refused for the errors ones;
6. XXH64 of a few inputs and seeds.

Every case is decoded whole (Zstd.decompress, onto a vector that holds a byte already) and in small steps (a
ZstdDecoder), which must agree. The corpus program is short: the interpreter runs every corpus program. The long
lists (libzstd's frames of every shape and size, every prefix, mutants) are tools/codec_check.py's, compiled only.

usage: python3 tools/oracles/gen_zstd_cases.py   (needs libzstd; formats with $CEESAFEC, default out/ceesafec)
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_deflate_cases as g  # noqa: E402  the data shapes the deflate corpus uses
import zstd_model as m  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUTPUT = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "zstd.cee")

# The window limit of the corpus's decoders: every case's frame fits it, except the windowed ones.
STEP_WINDOW = 1 << 14


def runs(n, seed):
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += bytes([(x >> 16) & 3]) * (1 + (x >> 8) % 300)
    return bytes(out[:n])


SHAPES = {"message": g.message, "words": g.words, "zeros": g.zeros, "runs": runs}


def shape(name, n, seed=1):
    return SHAPES[name](n, seed)


def lib_frames(lib):
    """(label, frame) written by libzstd."""
    out = []
    for name, n, level in [("words", 0, 3), ("words", 1, 3), ("message", 100, 1), ("words", 3000, 1),
                           ("words", 3000, 3), ("words", 3000, 19), ("runs", 3000, 9), ("zeros", 5000, 3),
                           ("message", 2000, -5), ("words", 12000, 19), ("runs", 12000, 1)]:
        out.append((f"libzstd {name} {n} level {level}", lib.compress(shape(name, n), level)))
    for name, n, level, wl in [("words", 6000, 3, 10), ("words", 6000, 19, 11), ("runs", 9000, 9, 10),
                               ("message", 3000, 1, 12)]:
        f = lib.compress(shape(name, n), level, checksum=True, content_size=False, window_log=wl, streamed=True,
                         piece=700)
        out.append((f"libzstd {name} {n} level {level} streamed, window 2^{wl}, checksum", f))
    out.append(("libzstd words 8000 long", lib.compress(shape("words", 8000, 2), 19, long=True, window_log=13,
                                                        content_size=False)))
    out.append(("libzstd words 3000 checksum", lib.compress(shape("words", 3000, 3), 3, checksum=True)))
    return out


def hand_frames():
    """(label, frame, limit) written by hand."""
    fh, raw, rle, skip, ck, cb = m.frame_head, m.raw_block, m.rle_block, m.skippable, m.checksum_of, m.compressed_block
    abc = b"abcdefghij"
    cases = [
        ("empty input", b"", 64),
        ("a skippable frame alone", skip(b"meta"), 64),
        ("an empty skippable frame, nibble 15", skip(b"", 15), 64),
        ("a skippable frame cut short", skip(b"meta")[:-1], 64),
        ("a skippable frame's size cut short", skip(b"meta")[:6], 64),
        ("a skippable frame, then a frame", skip(b"xyz") + fh(10) + raw(abc), 64),
        ("a frame, then a skippable frame", fh(10) + raw(abc) + skip(b"xyz"), 64),
        ("two frames", fh(10) + raw(abc) + fh(single=True, fcs=3) + raw(b"xyz"), 64),
        ("a magic number cut short", fh(10)[:3], 64),
        ("not a magic number", b"\x28\xb5\x2f\xfe" + fh(10)[4:] + raw(abc), 64),
        ("a header cut short", fh(10, fcs=10, fcs_bytes=8)[:9], 64),
        ("the header's reserved bit", fh(10, reserved=True) + raw(abc), 64),
        ("the header's unused bit", fh(10)[:4] + bytes([fh(10)[4] | 16]) + fh(10)[5:] + raw(abc), 64),
        ("a dictionary ID", fh(10, dict_id=7) + raw(abc), 64),
        ("a dictionary ID of 4 bytes", fh(10, dict_id=1 << 20) + raw(abc), 64),
        ("a dictionary ID field of 0", fh(10, dict_id=0)[:4] + bytes([fh(10)[4] | 1]) + fh(10)[5:] + b"\x00" + raw(abc),
         64),
        ("a window of 2^31", fh(31) + raw(abc), 64),
        ("a window above 2^31", fh(32) + raw(abc), 64),
        ("a window of 2^10 and 7/8", fh(10, window_mantissa=7) + raw(abc), 64),
        ("a content size of 1 byte", fh(single=True, fcs=10) + raw(abc), 64),
        ("a content size of 2 bytes", fh(10, fcs=300) + raw(bytes(300)), 400),
        ("a content size of 2 bytes, 256", fh(10, fcs=256, fcs_bytes=2) + raw(bytes(256)), 400),
        ("a content size of 4 bytes", fh(10, fcs=10, fcs_bytes=4) + raw(abc), 64),
        ("a content size of 8 bytes", fh(10, fcs=10, fcs_bytes=8) + raw(abc), 64),
        ("a content size of 0, single segment", fh(single=True, fcs=0) + raw(b""), 64),
        ("a content size above the limit", fh(10, fcs=100) + rle(1, 100), 99),
        ("a content size no frame this size can have", fh(10, fcs=1 << 30, fcs_bytes=8) + rle(1, 1000), 1 << 31),
        ("output short of the content size", fh(10, fcs=11) + raw(abc), 64),
        ("output past the content size", fh(10, fcs=9) + raw(abc), 64),
        ("output past the content size in a later block", fh(10, fcs=12) + raw(abc, False) + raw(abc), 64),
        ("several blocks", fh(10) + raw(abc, False) + rle(7, 33, False) + raw(b""), 64),
        ("a raw block of the window", fh(10) + raw(bytes(1024)), 2048),
        ("a raw block past the window", fh(10) + raw(bytes(1025)), 2048),
        ("an RLE block past the window", fh(10) + rle(5, 1025), 2048),
        ("an RLE block of 128 KiB", fh(17) + rle(5, 131072), 1 << 17),
        ("an RLE block past 128 KiB", fh(20) + rle(5, 131073), 1 << 18),
        ("a single segment's block past its content size", fh(single=True, fcs=10) + rle(5, 11), 64),
        ("a reserved block type", fh(10) + m.block_head(True, 3, 0), 64),
        ("a block header cut short", fh(10) + raw(abc)[:2], 64),
        ("no last block", fh(10) + raw(abc, False), 64),
        ("a raw block cut short", fh(10) + raw(abc)[:-1], 64),
        ("an RLE block cut short", fh(10) + rle(5, 9)[:3], 64),
        ("a checksum", fh(10, checksum=True) + raw(abc) + ck(abc), 64),
        ("a wrong checksum", fh(10, checksum=True) + raw(abc) + ck(abc + b"!"), 64),
        ("a checksum cut short", fh(10, checksum=True) + raw(abc) + ck(abc)[:3], 64),
        ("a checksum of no bytes", fh(single=True, fcs=0, checksum=True) + raw(b"") + ck(b""), 64),
        ("a byte after the frame", fh(10) + raw(abc) + b"\x00", 64),
        ("the limit exactly", fh(10) + raw(abc), 10),
        ("the limit a byte short", fh(10) + raw(abc), 9),
        ("the limit a byte short, in a later block", fh(10) + raw(abc, False) + raw(abc), 19),
        ("a limit of 0, no output", fh(10) + raw(b""), 0),
    ]
    cases += compressed_cases()
    return cases


def compressed_cases():
    """(label, frame, limit): compressed blocks written by hand, in frames of a window of 2^10."""
    fh, cb, raw = m.frame_head, m.compressed_block, m.raw_block
    lr, srle = m.literals_raw, m.sequences_rle
    abc = b"abcdefghij"
    w = [0] * 97 + [3, 2, 1, 1]  # a 3, b 2, c and d 1 (implied): a complete code of 3 bits
    huff = b"abcdabcaabbaaddabcabcaaa"
    cases = [
        ("raw literals, no sequences", fh(10) + cb(lr(abc) + b"\x00"), 64),
        ("raw literals of 2 header bytes", fh(10) + cb(lr(bytes(range(100))) + b"\x00"), 128),
        ("RLE literals", fh(10) + cb(m.literals_rle(120, 40) + b"\x00"), 64),
        ("RLE literals cut short", fh(10) + cb(bytes([0x45, 0x01])), 64),
        ("raw literals past the block", fh(10) + cb(bytes([20 << 3]) + abc + b"\x00"), 64),
        ("a compressed block of 1 byte", fh(10) + cb(b"\x00"), 64),
        ("an empty compressed block (stricter)", fh(10) + cb(b""), 64),
        ("no sequences section", fh(10) + cb(lr(abc)), 64),
        ("bytes after no sequences", fh(10) + cb(lr(abc) + b"\x00\x00"), 64),
        ("Huffman literals, 1 stream", fh(10) + cb(m.literals_huffman(huff, w, 1) + b"\x00"), 64),
        ("Huffman literals, 4 streams", fh(10) + cb(m.literals_huffman(huff, w, 4) + b"\x00"), 64),
        ("Huffman literals, then treeless",
         fh(10) + cb(m.literals_huffman(huff, w, 4) + b"\x00", False) +
         cb(m.literals_huffman(huff[::-1], w, 1, treeless=True) + b"\x00"), 64),
        ("Huffman literals, then treeless in 4 streams",
         fh(10) + cb(m.literals_huffman(huff, w, 1) + b"\x00", False) +
         cb(m.literals_huffman(huff[::-1], w, 4, treeless=True) + b"\x00"), 64),
        ("treeless literals with no table before", fh(10) + cb(m.literals_huffman(huff, w, 1, treeless=True) + b"\x00"),
         64),
        ("treeless literals in a new frame",
         fh(10) + cb(m.literals_huffman(huff, w, 1) + b"\x00") + fh(10) +
         cb(m.literals_huffman(huff, w, 1, treeless=True) + b"\x00"), 64),
        ("4 Huffman streams of 5 literals", fh(10) + cb(m.literals_huffman(b"abcda", w, 4) + b"\x00"), 64),
        ("a Huffman stream not used up (stricter)", fh(10) + cb(m.literals_huffman(huff, w, 1, pad_bits=5) + b"\x00"),
         64),
        ("a Huffman weight above 12",
         fh(10) + cb(m.literals_huffman(huff, w, 1, header_weights=[0] * 97 + [13, 2, 1]) + b"\x00"), 64),
        ("Huffman weights not a complete code",
         fh(10) + cb(m.literals_huffman(huff, w, 1, header_weights=[0] * 97 + [3, 2, 2]) + b"\x00"), 64),
        ("Huffman weights with one longest code",
         fh(10) + cb(m.literals_huffman(huff, w, 1, header_weights=[0] * 97 + [2, 1]) + b"\x00"), 64),
        ("Huffman streams past the section", fh(10) + cb(m.literals_huffman(huff, w, 4)[:-3] + b"\x00"), 64),
        ("a match", fh(10) + cb(lr(abc) + srle([(10, 10 + 3, 20)])), 64),
        ("a match to the frame's start", fh(10) + raw(abc, False) + cb(lr(b"xy") + srle([(2, 12 + 3, 4)])), 64),
        ("a match a byte before the frame's start",
         fh(10) + raw(abc, False) + cb(lr(b"xy") + srle([(2, 13 + 3, 4)])), 64),
        ("a match before the frame's start, a frame before",
         fh(10) + raw(abc) + fh(10) + cb(lr(b"xy") + srle([(2, 4 + 3, 4)])), 64),
        ("a match past the window (stricter)",
         fh(10) + raw(bytes(range(256)) * 4, False) + cb(lr(b"xy") + srle([(2, 1025 + 3, 4)])), 4096),
        ("a match of the whole window",
         fh(10) + raw(bytes(range(256)) * 4, False) + cb(lr(b"xy") + srle([(2, 1024 + 3, 4)])), 4096),
        ("overlapping matches", fh(10) + cb(lr(b"abc") + srle([(1, 1 + 3, 43), (1, 2 + 3, 44), (1, 3 + 3, 46)])),
         256),
        ("the first repeated offset", fh(10) + cb(lr(b"zy") + srle([(1, 1, 35), (1, 1, 36)])), 128),
        ("the second and third repeated offsets",
         fh(10) + raw(abc, False) + cb(lr(b"zy") + srle([(1, 2, 35), (1, 3, 36)])), 128),
        ("a repeated offset with no literals, the second",
         fh(10) + raw(abc, False) + cb(lr(b"") + srle([(0, 1, 35)])), 128),
        ("repeated offsets with no literals, the third and the first less 1",
         fh(10) + raw(abc, False) + cb(lr(b"") + srle([(0, 2, 35), (0, 3, 36)])), 128),
        ("repeated offsets across blocks",
         fh(10) + cb(lr(abc) + srle([(10, 7 + 3, 35)]), False) + cb(lr(b"") + srle([(0, 1, 35), (0, 1, 36)])), 256),
        ("a repeated offset 1 less, to 0",
         fh(10) + cb(lr(abc) + srle([(10, 1 + 3, 35)]), False) + cb(lr(b"") + srle([(0, 3, 35)])), 256),
        ("a sequence past the literals", fh(10) + cb(lr(abc) + srle([(11, 5 + 3, 4)])), 64),
        ("a block past its maximum", fh(10) + cb(lr(abc) + srle([(10, 1 + 3, 1020)])), 2048),
        ("a match past the limit", fh(10) + cb(lr(abc) + srle([(10, 1 + 3, 100)])), 50),
        ("a match past the content size", fh(10, fcs=50) + cb(lr(abc) + srle([(10, 1 + 3, 100)])), 4096),
        ("the modes byte's reserved bits", fh(10) + cb(lr(abc) + b"\x01\x55\x00\x00\x00\x01"), 64),
        ("a repeated table with none before", fh(10) + cb(lr(abc) + b"\x01\xfc\x01"), 64),
        ("an RLE table symbol past the maximum", fh(10) + cb(lr(abc) + b"\x01\x54\x24\x00\x00\x01"), 64),
        ("FSE counts that do not add up", fh(10) + cb(lr(abc) + b"\x01\x80\x00\x00\x00\x01"), 64),
        ("an FSE log too large", fh(10) + cb(lr(abc) + b"\x01\x80\x05\x00\x00\x01"), 64),
        ("a sequences stream not used up", fh(10) + cb(lr(abc) + srle([(10, 5 + 3, 4)])[:-1] + b"\x07"), 64),
        ("a sequences stream with no end mark", fh(10) + cb(lr(abc) + srle([(10, 5 + 3, 4)])[:-1] + b"\x00"), 64),
    ]
    return cases


def window_for(label):
    """The window limit of the case's decoder in steps: 2^20 for the blocks of 128 KiB, else STEP_WINDOW."""
    return 1 << 20 if "128 KiB" in label else STEP_WINDOW


GOLDEN = os.path.join(HERE, "zstd_golden")


def golden_frames():
    """(label, frame, limit): zstd's golden decompression files of at most 1 KiB that decode to at most 64 KiB, or
    are refused (tools/oracles/zstd_golden): the interpreter runs the corpus."""
    out = []
    for sub, kind in (("", "golden"), ("errors", "golden error")):
        folder = os.path.join(GOLDEN, sub)
        for name in sorted(os.listdir(folder)):
            path = os.path.join(folder, name)
            if name.endswith(".zst") and os.path.getsize(path) <= 1024:
                with open(path, "rb") as f:
                    frame = f.read()
                if len(m.try_decode(frame) or b"") <= 65536:
                    out.append((f"{kind} {name}", frame, 1 << 20))
    return out


def checked(lib, label, frame, limit):
    """The expected line for `frame`, from the model, which libzstd must agree with (or be laxer, where the label
    says the model is stricter)."""
    c = m.compare(lib, frame, limit)
    if c == "stricter":
        assert "stricter" in label, label
    elif c == "once":
        assert "128 KiB" in label, label
    else:
        assert c is None, f"{label}: {c}"
    expected = m.verdict(frame, limit)
    stepped = m.verdict(frame, limit, window_for(label))
    if stepped != expected:
        # Only a window above the stepping decoder's limit makes them differ.
        assert "window" in label and stepped == "TooLarge", (label, expected, stepped)
        return f"{expected} but in steps {stepped}"
    return expected


PROGRAM_HEAD = '''// GENERATED by tools/oracles/gen_zstd_cases.py: edit that, not this file.
// lib/std's Zstd and ZstdDecoder (ADR 0196 phase 3). Every expected line comes from an oracle outside the code under
// test: libzstd wrote or read first every frame decoded here (an "ok" line has its output's length and CRC-32), or
// refused it, and tools/oracles/zstd_model.py, written from RFC 8878, agreed (it is stricter on one case, which
// libzstd's fast loops accept). Each frame is decoded whole and in small steps, which must agree. tools/codec_check.py
// runs the long lists, compiled only.

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

fn errorName(e: DecodeError) -> StringView => match (e) {
    DecodeError.Malformed => "Malformed",
    DecodeError.TooLarge => "TooLarge",
    DecodeError.Unsupported => "Unsupported",
    DecodeError.ChecksumMismatch => "ChecksumMismatch",
    DecodeError.OutOfMemory(_) => "OutOfMemory",
};

fn decimal(v: u64, into: borrow mut Vector<u8>) {
    if (v >= 10) { decimal(v / 10, into); }
    into.push(truncate<u8>(48 + v % 10))!;
}

/// A verdict: "ok <length> <crc>", or the error's name.
fn verdict(r: Result<void, DecodeError>, out: Slice<u8>, into: borrow mut Vector<u8>) {
    match (r) {
        Ok() => {
            into.extend("ok ".bytes())!;
            decimal(out.length as u64, into);
            into.push(32)!;
            decimal(Crc32.of(out) as u64, into);
        }
        Err(e) => { into.extend(errorName(e).bytes())!; }
    }
}

/// Decodes in steps of 1 to 7 bytes of input and 1 to 13 of output.
fn steps(input: Slice<u8>, limit: usize, windowLimit: usize, out: borrow mut Vector<u8>) -> Result<void, DecodeError> {
    var d = ZstdDecoder.create(limit as u64, windowLimit)?;
    var buffer = [0u8; 13];
    var at: usize = 0;
    var k: usize = 0;
    while (true) {
        k += 1;
        let si = 1 + (k * 5) % 7;
        let so = 1 + (k * 3) % 13;
        let end = if (input.length - at < si) input.length else at + si;
        let p = d.step(input[at..end], mut buffer[0..so])?;
        out.extend(buffer[0..p.written])!;
        at += p.read;
        if (p.done && at == input.length) { return Ok(); }
        if (!p.done && p.read == 0 && p.written == 0 && end == input.length) { return Err(DecodeError.Malformed); }
    }
    return Ok();
}

/// Decodes a frame whole, onto a vector that holds a byte already, which must survive (and be all that is left after
/// a refusal), and in steps; prints the verdict if they agree.
fn decode(label: StringView, pieces: Slice<StringView>, limit: usize, windowLimit: usize) {
    let input = unhex(pieces);
    var out = Vector<u8>.create();
    out.push(7)!;
    let r = Zstd.decompress(input.view(), limit, mut out);
    var whole = Vector<u8>.create();
    verdict(r, out[1..], mut whole);
    if (out[0] != 7) { whole.extend(" (the byte before changed)".bytes())!; }
    if (r.isErr() && out.length != 1) { whole.extend(" (output left behind)".bytes())!; }
    var stepped = Vector<u8>.create();
    let rs = steps(input.view(), limit, windowLimit, mut stepped);
    var byStep = Vector<u8>.create();
    verdict(rs, stepped.view(), mut byStep);
    Console.write(label);
    Console.write(" ");
    Console.write(StringView.fromUtf8(whole.view()) ?? "");
    if (whole.view() != byStep.view()) {
        Console.write(" but in steps ");
        Console.write(StringView.fromUtf8(byStep.view()) ?? "");
    }
    Console.writeLine("");
}

/// Decodes in steps with a window limit.
fn windowed(label: StringView, pieces: Slice<StringView>, limit: usize, windowLimit: usize) {
    let input = unhex(pieces);
    var stepped = Vector<u8>.create();
    let rs = steps(input.view(), limit, windowLimit, mut stepped);
    var byStep = Vector<u8>.create();
    verdict(rs, stepped.view(), mut byStep);
    Console.write(label);
    Console.write(" ");
    Console.writeLine(StringView.fromUtf8(byStep.view()) ?? "");
}

/// XXH64 in one call and fed in three pieces.
fn hash(label: StringView, pieces: Slice<StringView>, seed: u64) {
    let input = unhex(pieces);
    let n = input.length;
    var x = Xxh64.create(seed);
    x.update(input[0..n / 3]);
    x.update(input[n / 3..2 * n / 3]);
    x.update(input[2 * n / 3..]);
    Console.write("xxh64 ");
    Console.write(label);
    Console.write(" ");
    Console.write(Xxh64.of(input.view(), seed));
    Console.writeLine(if (x.finish() == Xxh64.of(input.view(), seed)) "" else " (in pieces differs)");
}

fn main() {
'''


def pieces(data):
    """`data` in hex, as an array of strings of at most 96 digits."""
    text = data.hex()
    parts = [text[k:k + 96] for k in range(0, len(text), 96)] or [""]
    return "[" + ", ".join(f'"{p}"' for p in parts) + "]"


HASHES = [("empty", b"", 0), ("123456789", b"123456789", 0), ("123456789 seed 1", b"123456789", 1),
          ("31 bytes", bytes(range(31)), 0), ("32 bytes", bytes(range(32)), 0),
          ("100 bytes seed 2^64 - 1", bytes(range(100)), (1 << 64) - 1), ("1000 words", g.words(1000, 3), 7)]


def program(cases, windowed_cases):
    body = []
    lines = []
    for k, (label, frame, limit, expected) in enumerate(cases):
        body.append(f'    let s{k} = {pieces(frame)};')
        body.append(f'    decode("{label}", s{k}[..], {limit}, {window_for(label)});')
        lines.append(f"{label} {expected}")
    for k, (label, frame, limit, window, expected) in enumerate(windowed_cases):
        body.append(f'    let w{k} = {pieces(frame)};')
        body.append(f'    windowed("{label}", w{k}[..], {limit}, {window});')
        lines.append(f"{label} {expected}")
    for k, (label, data, seed) in enumerate(HASHES):
        body.append(f'    let h{k} = {pieces(data)};')
        body.append(f'    hash("{label}", h{k}[..], {seed});')
        lines.append(f"xxh64 {label} {m.xxh64(data, seed)}")
    text = PROGRAM_HEAD + "\n".join(body) + "\n}\n"
    text += "".join(f"// expect-stdout: {line}\n" for line in lines)
    return text


REQUIRED = ["block raw", "block RLE", "block compressed", "literals raw", "literals RLE", "literals Huffman, 1 stream",
            "literals Huffman, 4 streams", "literals treeless, 1 stream", "literals treeless, 4 streams",
            "sequences predefined literal lengths", "sequences RLE offsets", "sequences FSE match lengths",
            "sequences repeat offsets", "repeat offsets", "skippable frame", "frame checksum",
            "frame size single segment", "frame"]


def main():
    lib = m.libzstd()
    if lib is None:
        print("gen_zstd_cases: libzstd is needed (Homebrew's zstd, or a Linux system's libzstd.so.1)")
        return 1
    cases = []
    stats = m.Stats()
    for label, frame in lib_frames(lib):
        limit = 1 << 20
        cases.append((label, frame, limit, checked(lib, label, frame, limit)))
        m.decode(frame, stats=stats)
    for label, frame, limit in golden_frames() + hand_frames():
        cases.append((label, frame, limit, checked(lib, label, frame, limit)))
        try:
            m.decode(frame, limit, stats=stats)
        except m.Refused:
            pass
    missing = [r for r in REQUIRED if r not in stats.seen]
    assert not missing, f"the corpus does not cover {missing}"
    big = m.frame_head(15) + m.raw_block(g.words(3000, 5))
    windowed_cases = [
        ("window 2^15, decoder limit 2^14", big, 1 << 20, 1 << 14, m.verdict(big, 1 << 20, 1 << 14)),
        ("window 2^15, decoder limit 2^15", big, 1 << 20, 1 << 15, m.verdict(big, 1 << 20, 1 << 15)),
        ("a single segment of 3000, decoder limit 2999", lib.compress(g.words(3000, 6), 3), 1 << 20, 2999,
         m.verdict(lib.compress(g.words(3000, 6), 3), 1 << 20, 2999)),
    ]
    assert windowed_cases[0][4] == "TooLarge" and windowed_cases[1][4].startswith("ok")
    text = program(cases, windowed_cases)
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", OUTPUT], check=True)
    print(f"gen_zstd_cases: wrote {os.path.relpath(OUTPUT, ROOT)}, {len(cases) + len(windowed_cases)} frames "
          f"(oracle libzstd {lib.version}); covered: {', '.join(sorted(stats.seen))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
