#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/snappy.cee: lib/std's Snappy (ADR 0196 phase 2) against oracles
outside the code under test: libsnappy, the reference implementation, writes the blocks the program decodes and
decodes every block first, and tools/oracles/snappy_model.py, a decoder written from snappy's
format_description.txt, agrees with it on each (the generator stops if they differ). cramjam's (Rust's `snap`)
verdicts are compared too where it is installed, and its differences printed.

The cases:
1. libsnappy's blocks of a few shapes and sizes (one and two fragments of 64 KiB), decoded and their preambles read;
2. the model's blocks of every kind of element (tools/oracles/snappy_model.py `variety`): literals with every length
   tag (60 to 63) and some longer than they need, copies with 1, 2 and 4 byte offsets, of every length from 1 to 64,
   overlapping (offsets 1 to 9 below the length);
3. blocks written by hand: preambles of 1 to 5 bytes, a non-minimal varint, a sixth byte, a fifth above 15, cut
   short; literals cut short in the tag's length or the bytes, a literal of 2^32 bytes; copies of offset 0, one byte
   before the start, exactly the start, offsets of 2^31, each kind cut short; output short of the preamble and past
   it (bytes after the block); a length past the limit, and a length no block of the size could produce;
4. the limit: exactly the output, one byte less, 0;
5. Avro's snappy blocks: the block and the big-endian CRC-32 (zlib.crc32) of its bytes, and a wrong one;
6. round trips of lib/std's own encoder (the input is the oracle), with its preamble read back and its size within
   32 + n + n / 6.

The corpus program is short: the interpreter runs every corpus program. The long lists (libsnappy's blocks of every
shape around the fragment size, libsnappy decoding lib/std's output, every prefix, mutants) are
tools/codec_check.py's, compiled only.

usage: python3 tools/oracles/gen_snappy_cases.py   (needs libsnappy; formats with $CEESAFEC, default out/ceesafec)
"""

import os
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_deflate_cases as g  # noqa: E402  the data shapes the deflate corpus uses
import snappy_model as m  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUTPUT = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "snappy.cee")


def runs(n, seed):
    """Runs of a few byte values, 1 to 300 long: overlapping copies."""
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += bytes([(x >> 16) & 3]) * (1 + (x >> 8) % 300)
    return bytes(out[:n])


SHAPES = {"message": g.message, "words": g.words, "zeros": g.zeros, "runs": runs}


def shape(name, n, seed):
    return SHAPES[name](n, seed)


def checked(lib, cj, label, block, limit):
    """The expected line for `block`, from the model, which libsnappy must agree with."""
    ours = m.decode(block)
    theirs = lib.decompress(block)
    assert ours == theirs, f"{label}: the model and libsnappy differ"
    if cj is not None and cj.decompress(block) != theirs:
        print(f"gen_snappy_cases: {label}: cramjam {'refuses' if theirs is not None else 'decodes'} it, libsnappy not")
    return m.verdict(block, limit)


def hand_cases():
    """(label, block, limit, must be refused) written by hand: each a refusal of ADR 0196's or an edge."""
    v, lit, c1, c2, c4 = m.varint, m.literal, m.copy1, m.copy2, m.copy4
    abc = b"abcdefghij"
    cases = [
        ("empty input", b"", 16, True),
        ("preamble 0, nothing else", b"\x00", 16, False),
        ("preamble cut short", b"\x80", 16, True),
        ("preamble cut short after four", b"\xff\xff\xff\xff", 16, True),
        ("preamble non-minimal", b"\x80\x00", 16, False),
        ("preamble non-minimal, a literal", b"\x81\x00" + lit(b"x"), 16, False),
        ("preamble of five bytes, 2^32 - 1", b"\xff\xff\xff\xff\x0f", 1 << 20, True),
        ("preamble of five bytes, fifth 16", b"\xff\xff\xff\xff\x10", 1 << 20, True),
        ("preamble of six bytes", b"\x80\x80\x80\x80\x80\x00", 1 << 20, True),
        ("preamble of five bytes, small", b"\x8a\x80\x80\x80\x00" + lit(abc), 16, False),
        ("a literal", v(10) + lit(abc), 16, False),
        ("a literal, length in 1 byte", v(10) + lit(abc, 1), 16, False),
        ("a literal, length in 2 bytes", v(10) + lit(abc, 2), 16, False),
        ("a literal, length in 3 bytes", v(10) + lit(abc, 3), 16, False),
        ("a literal, length in 4 bytes", v(10) + lit(abc, 4), 16, False),
        ("a literal of 60", v(60) + lit(bytes(range(60))), 64, False),
        ("a literal of 61", v(61) + lit(bytes(range(61))), 64, False),
        ("a literal of 256", v(256) + lit(bytes(256)), 300, False),
        ("a literal of 257", v(257) + lit(bytes(range(256)) + b"!"), 300, False),
        ("a literal cut in its bytes", v(10) + lit(abc)[:-1], 16, True),
        ("a literal cut in its length", v(10) + b"\xf4\x09", 16, True),
        ("a literal of 2^32", v(10) + b"\xfc\xff\xff\xff\xff" + abc, 16, True),
        ("a literal past the preamble", v(9) + lit(abc), 16, True),
        ("a literal short of the preamble", v(11) + lit(abc), 16, True),
        ("copy1 offset 0", v(14) + lit(abc) + c1(0, 4), 16, True),
        ("copy2 offset 0", v(14) + lit(abc) + c2(0, 4), 16, True),
        ("copy4 offset 0", v(14) + lit(abc) + c4(0, 4), 16, True),
        ("copy1 a byte before the start", v(14) + lit(abc) + c1(11, 4), 16, True),
        ("copy2 a byte before the start", v(14) + lit(abc) + c2(11, 4), 16, True),
        ("copy4 a byte before the start", v(14) + lit(abc) + c4(11, 4), 16, True),
        ("copy4 offset 2^31", v(14) + lit(abc) + c4(1 << 31, 4), 16, True),
        ("copy4 offset 2^32 - 1", v(14) + lit(abc) + c4((1 << 32) - 1, 4), 16, True),
        ("a copy first", v(4) + c2(1, 4), 16, True),
        ("copy1 from the start", v(21) + lit(abc) + c1(10, 11), 32, False),
        ("copy2 from the start", v(74) + lit(abc) + c2(10, 64), 80, False),
        ("copy4 from the start", v(11) + lit(abc) + c4(10, 1), 16, False),
        ("copy2 of 1, 2 and 3", v(16) + lit(abc) + c2(3, 1) + c2(7, 2) + c2(10, 3), 16, False),
        ("copy1 cut short", v(14) + lit(abc) + c1(3, 4)[:1], 16, True),
        ("copy2 cut short", v(14) + lit(abc) + c2(3, 4)[:2], 16, True),
        ("copy4 cut short", v(14) + lit(abc) + c4(3, 4)[:4], 16, True),
        ("a copy past the preamble", v(13) + lit(abc) + c2(3, 4), 16, True),
        ("bytes after the block", v(10) + lit(abc) + b"\x00", 16, True),
        ("a literal after the block", v(10) + lit(abc) + lit(b"z"), 16, True),
        ("a length past the limit", v(10) + lit(abc), 9, True),
        ("a length past the limit, then nothing", v(1000000), 999999, True),
        ("a length no block this size makes", v(1000000), 1 << 30, True),
        ("a length no block this size makes, a copy", v(1000000) + lit(b"a") + c2(1, 64), 1 << 30, True),
        ("the most a 3-byte copy makes", v(65) + lit(b"a") + c2(1, 64), 128, False),
        ("a long run", v(1 + 64 * 40) + lit(b"q") + c2(1, 64) * 40, 4096, False),
    ]
    for d in range(1, 10):
        cases.append((f"an overlapping copy, offset {d}", v(d + 64) + lit(abc[:d]) + c2(d, 64), 128, False))
        cases.append((f"an overlapping copy1, offset {d}", v(d + 11) + lit(abc[:d]) + c1(d, 11), 32, False))
    for k, n in ((1, 127), (2, 16383), (3, 70000), (4, 100), (5, 100)):
        # A preamble of k bytes (the 4 and 5 byte ones not minimal, to keep the corpus small) and a run of n bytes.
        pre = v(n) if k <= 3 else bytes([(n & 127) | 128]) + b"\x80" * (k - 2) + b"\x00"
        body = lit(b"z") + c2(1, 64) * ((n - 1) // 64) + (c2(1, (n - 1) % 64) if (n - 1) % 64 else b"")
        cases.append((f"a preamble of {k} bytes", pre + body, n, False))
    return cases


def avro_cases(lib):
    """(label, block and CRC, limit, expected): the model decodes the block, zlib.crc32 is the checksum."""
    data = g.words(700, 4)
    block = lib.compress(data)
    crc = zlib.crc32(data).to_bytes(4, "big")
    wrong = (zlib.crc32(data) ^ 1).to_bytes(4, "big")
    assert m.decode(block) == data and m.decode(block[:-1]) is None
    return [("avro words 700", block + crc, len(data), f"ok {len(data)} {zlib.crc32(data)}"),
            ("avro a wrong crc", block + wrong, len(data), "ChecksumMismatch"),
            ("avro cut short", block[:-1] + crc, len(data), "Malformed")]


def corpus_cases(lib, cj):
    """(label, block, limit, expected)."""
    cases = []
    for name, n in [("words", 0), ("words", 1), ("words", 100), ("words", 3000), ("message", 300), ("zeros", 5000),
                    ("runs", 2000), ("zeros", 70000), ("words", 20000)]:
        data = shape(name, n, 1)
        cases.append((f"libsnappy {name} {n}", lib.compress(data), n))
    for name, n, seed in [("words", 3000, 1), ("message", 1000, 2), ("runs", 3000, 3), ("words", 600, 4)]:
        data = shape(name, n, seed)
        block = m.variety(data, seed)
        assert lib.decompress(block) == data
        cases.append((f"every element {name} {n}", block, n))
    data = g.words(2000, 9)
    block = lib.compress(data)
    cases += [("limit exactly the output", block, 2000), ("limit a byte short", block, 1999),
              ("limit 0, no output", lib.compress(b""), 0), ("limit 0, a byte", lib.compress(b"a"), 0)]
    out = []
    for label, block, limit in cases:
        out.append((label, block, limit, checked(lib, cj, label, block, limit)))
    for label, block, limit, refused in hand_cases():
        expected = checked(lib, cj, label, block, limit)
        assert (expected != "Malformed" and expected != "TooLarge") != refused, (label, expected)
        out.append((label, block, limit, expected))
    return out


PREAMBLES = [("none", b""), ("0", b"\x00"), ("127", b"\x7f"), ("128", b"\x80\x01"), ("non-minimal 0", b"\x80\x00"),
             ("2^21", b"\x80\x80\x80\x01"), ("2^32 - 1", b"\xff\xff\xff\xff\x0f"), ("fifth 16", b"\xff\xff\xff\xff\x10"),
             ("six bytes", b"\x80\x80\x80\x80\x80\x00"), ("cut", b"\xff\xff"), ("with more", b"\x05\x10abcde")]

ROUND_TRIPS = [("words", 0, 1), ("words", 1, 1), ("words", 14, 2), ("words", 15, 3), ("words", 16, 4),
               ("message", 100, 5), ("words", 3000, 6), ("zeros", 3000, 7), ("message", 3000, 8), ("runs", 3000, 9),
               ("zeros", 140000, 10), ("words", 70000, 11)]

PROGRAM_HEAD = '''// GENERATED by tools/oracles/gen_snappy_cases.py: edit that, not this file.
// lib/std's Snappy (ADR 0196 phase 2). Every expected line comes from an oracle outside the code under test: libsnappy
// wrote or decoded first every block decoded here (an "ok" line has its output's length and CRC-32), or refused it,
// and tools/oracles/snappy_model.py, written from snappy's format_description.txt, agreed; a round trip's oracle is its
// input. tools/codec_check.py runs the long lists, compiled only.

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

fn runs(n: usize, seed: u32) -> Vector<u8> {
    var v = Vector<u8>.create();
    var x = seed;
    while (v.length < n) {
        x = (x.wrappingMul(1103515245).wrappingAdd(12345)) & 0x7FFFFFFF;
        let b = truncate<u8>((x >> 16) & 3);
        for (k in 0..1 + (x >> 8) % 300) {
            v.push(b)!;
        }
    }
    v.truncate(n);
    return v;
}

fn shape(name: StringView, n: usize, seed: u32) -> Vector<u8> {
    if (name == "message") { return message(n, seed as usize); }
    if (name == "words") { return words(n, seed); }
    if (name == "runs") { return runs(n, seed); }
    var v = Vector<u8>.create();
    v.resize(n, 0)!;
    return v;
}

fn errorName(e: DecodeError) -> StringView => match (e) {
    DecodeError.Malformed => "Malformed",
    DecodeError.TooLarge => "TooLarge",
    DecodeError.Unsupported => "Unsupported",
    DecodeError.ChecksumMismatch => "ChecksumMismatch",
    DecodeError.OutOfMemory(_) => "OutOfMemory",
};

/// Decodes a block onto a vector that holds a byte already, which must survive, and must be all that is left after
/// a refusal.
fn decode(label: StringView, pieces: Slice<StringView>, limit: usize) {
    let input = unhex(pieces);
    var out = Vector<u8>.create();
    out.push(7)!;
    let r = Snappy.decompress(input.view(), limit, mut out);
    Console.write(label);
    Console.write(" ");
    match (r) {
        Ok() => {
            Console.write("ok ");
            Console.write(out.length - 1);
            Console.write(" ");
            Console.write(Crc32.of(out[1..]));
            Console.writeLine(if (out[0] == 7) "" else " (the byte before changed)");
        }
        Err(e) => {
            Console.write(errorName(e));
            Console.writeLine(if (out.length == 1 && out[0] == 7) "" else " (output left behind)");
        }
    }
}

/// An Avro snappy block: a raw block, then the big-endian CRC-32 of the bytes it decodes to.
fn avro(label: StringView, pieces: Slice<StringView>, limit: usize) {
    let input = unhex(pieces);
    Console.write(label);
    Console.write(" ");
    if (input.length < 4) {
        Console.writeLine("Malformed");
        return;
    }
    let body = input[0..input.length - 4];
    var out = Vector<u8>.create();
    match (Snappy.decompress(body, limit, mut out)) {
        Ok() => {
            if (input.readBE<u32>(input.length - 4) != Crc32.of(out.view())) {
                Console.writeLine("ChecksumMismatch");
                return;
            }
            Console.write("ok ");
            Console.write(out.length);
            Console.write(" ");
            Console.writeLine(Crc32.of(out.view()));
        }
        Err(e) => { Console.writeLine(errorName(e)); }
    }
}

fn length(label: StringView, pieces: Slice<StringView>) {
    let input = unhex(pieces);
    Console.write("length ");
    Console.write(label);
    Console.write(" ");
    match (Snappy.length(input.view())) {
        Some(n) => { Console.writeLine(n); }
        null => { Console.writeLine("none"); }
    }
}

/// Compressed by lib/std, with a byte already in the vector, and decoded back.
fn roundTrip(name: StringView, n: usize, seed: u32) {
    let data = shape(name, n, seed);
    var packed = Vector<u8>.create();
    packed.push(9)!;
    Snappy.compress(data.view(), 6, mut packed)!;
    let block = packed[1..];
    var back = Vector<u8>.create();
    let decoded = match (Snappy.decompress(block, n, mut back)) {
        Ok() => back.view() == data.view(),
        Err(_) => false,
    };
    let stated = Snappy.length(block) ?? n + 1;
    Console.write("round trip ");
    Console.write(name);
    Console.write(" ");
    Console.write(n);
    Console.write(": ");
    Console.write(if (decoded) "same" else "different");
    Console.write(if (stated == n) ", length stated" else ", length wrong");
    Console.writeLine(if (packed[0] == 9 && block.length <= 32 + n + n / 6) ", within the bound" else ", too long");
}

fn main() {
'''


def pieces(data):
    """`data` in hex, as an array of strings of at most 96 digits."""
    text = data.hex()
    parts = [text[k:k + 96] for k in range(0, len(text), 96)] or [""]
    return "[" + ", ".join(f'"{p}"' for p in parts) + "]"


def program(cases, avros):
    body = []
    lines = []
    for k, (label, block, limit, expected) in enumerate(cases):
        body.append(f'    let s{k} = {pieces(block)};')
        body.append(f'    decode("{label}", s{k}[..], {limit});')
        lines.append(f"{label} {expected}")
    for k, (label, block, limit, expected) in enumerate(avros):
        body.append(f'    let a{k} = {pieces(block)};')
        body.append(f'    avro("{label}", a{k}[..], {limit});')
        lines.append(f"{label} {expected}")
    for k, (label, block) in enumerate(PREAMBLES):
        p = m.preamble(block)
        body.append(f'    let p{k} = {pieces(block)};')
        body.append(f'    length("{label}", p{k}[..]);')
        lines.append(f"length {label} {p[0] if p else 'none'}")
    for name, n, seed in ROUND_TRIPS:
        body.append(f'    roundTrip("{name}", {n}, {seed});')
        lines.append(f"round trip {name} {n}: same, length stated, within the bound")
    text = PROGRAM_HEAD + "\n".join(body) + "\n}\n"
    text += "".join(f"// expect-stdout: {line}\n" for line in lines)
    return text


def main():
    lib = m.libsnappy()
    if lib is None:
        print("gen_snappy_cases: libsnappy is needed (Homebrew's snappy, or a Linux package's libsnappy)")
        return 1
    cj = m.cramjam()
    text = program(corpus_cases(lib, cj), avro_cases(lib))
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", OUTPUT], check=True)
    print(f"gen_snappy_cases: wrote {os.path.relpath(OUTPUT, ROOT)} (oracle {lib.path}"
          f"{', and ' + cj.path if cj else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
