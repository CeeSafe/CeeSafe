#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/os-string-wtf8.cee: lib/core's OsString and OsStringView (ADR 0257)
against Python's codecs as the oracle.

WTF-8 is Python's `surrogatepass` UTF-8: `units` (UTF-16, any units) decoded with `utf-16-le` and `surrogatepass`
pairs what pairs and keeps what does not, and encoding that with `utf-8` and `surrogatepass` gives `OsString.fromWide`.
Bytes are well-formed WTF-8 (`isWtf8`, `toWide` not null) when they decode with `utf-8` and `surrogatepass` and the
result has no lead surrogate directly before a trail surrogate (a pair is written as its code point's four bytes);
`toWide` is then that text encoded with `utf-16-le` and `surrogatepass`. `isUtf8` is the strict decoder.
`toUtf8Lossy` gives each encoded surrogate (`ED A0 80` to `ED BF BF`) one U+FFFD and the bytes between them Python's
`decode('utf-8', 'replace')` (ADR 0257 §2.2).

For each fixed case the program prints one line; then 2,000 seeded unit arrays and 2,000 seeded byte strings (from an
LCG both sides compute) give one summary line each: counts and FNV-1a hashes of the bytes, units and lossy texts.

usage: python3 tools/oracles/gen_os_string_cases.py   (formats with $CEESAFEC, default out/ceesafec)
"""

import os
import re
import subprocess

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
MASK = (1 << 64) - 1
RANDOM_CASES = 2000

WIDE_CASES = [
    [], [0x61], [0xE9], [0x20AC], [0xD83D, 0xDE00], [0xD800], [0xDC00], [0xDBFF, 0xDFFF], [0xDC00, 0xD800],
    [0xD800, 0x61], [0x61, 0xDFFF], [0xD800, 0xD800, 0xDC00], [0xD83D, 0xDE00, 0xDE00], [0xFFFF], [0xFEFF, 0x0],
    [0x7F, 0x80, 0x7FF, 0x800], [0xD7FF, 0xE000], [0x61, 0xDC80, 0x62],
]

BYTE_CASES = [
    "", "61", "c3a9", "eda080", "edbfbf", "edb080", "eda080edb080", "edb080eda080", "eda08061edb080", "f09f9880",
    "eda0", "ed", "ed9fbf", "ee8080", "ff", "c0af", "e080", "f4908080", "61eda080", "eda080eda080", "edafbfedb080",
    "616263eda080ff62", "e282", "f09f98", "eda080e282", "c328eda080",
]

# The pools the seeded cases draw from, in the program too (`randomUnits`, `randomBytes`).
UNIT_POOL = [0x61, 0x7A, 0xE9, 0x20AC, 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0xD83D, 0xDE00, 0xFFFD, 0x0800, 0x07FF]
BYTE_POOL = ["61", "c3a9", "eda080", "edb080", "f09f9880", "ed", "ff", "80", "e282", "edbfbf"]

SURROGATE = re.compile(rb"\xed[\xa0-\xbf][\x80-\xbf]")


class Lcg:
    def __init__(self, seed):
        self.state = seed

    def next(self):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & MASK
        return self.state >> 33


def from_wide(units):
    raw = b"".join(u.to_bytes(2, "little") for u in units)
    return raw.decode("utf-16-le", "surrogatepass").encode("utf-8", "surrogatepass")


def to_wide(data):
    try:
        text = data.decode("utf-8", "surrogatepass")
    except UnicodeDecodeError:
        return None
    for a, b in zip(text, text[1:]):
        if 0xD800 <= ord(a) <= 0xDBFF and 0xDC00 <= ord(b) <= 0xDFFF:
            return None
    raw = text.encode("utf-16-le", "surrogatepass")
    return [int.from_bytes(raw[i:i + 2], "little") for i in range(0, len(raw), 2)]


def is_utf8(data):
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def lossy(data):
    out = []
    at = 0
    for m in SURROGATE.finditer(data):
        out.append(data[at:m.start()].decode("utf-8", "replace"))
        out.append("�")
        at = m.end()
    out.append(data[at:].decode("utf-8", "replace"))
    return "".join(out).encode("utf-8")


def fnv(h, data):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & MASK
    return h


def hex_bytes(data):
    return data.hex() if data else "-"


def hex_units(units):
    return " ".join(f"{u:04x}" for u in units) if units else "-"


def random_units(rng):
    units = []
    for _ in range(rng.next() % 9):
        x = rng.next()
        units.append(UNIT_POOL[(x >> 4) % len(UNIT_POOL)] if x % 10 < 7 else (x >> 4) % 0x10000)
    return units


def random_bytes(rng):
    return b"".join(bytes.fromhex(BYTE_POOL[rng.next() % len(BYTE_POOL)]) for _ in range(rng.next() % 7))


def expectations():
    expect = []
    for units in WIDE_CASES:
        data = from_wide(units)
        expect.append(f"wide {hex_units(units)} -> {hex_bytes(data)} {str(to_wide(data) is not None).lower()} "
                      f"{str(is_utf8(data)).lower()}")
    for text in BYTE_CASES:
        data = bytes.fromhex(text)
        units = to_wide(data)
        shown = "null" if units is None else hex_units(units)
        expect.append(f"bytes {hex_bytes(data)} -> {str(units is not None).lower()} {shown} "
                      f"{str(is_utf8(data)).lower()} {hex_bytes(lossy(data))}")
    rng = Lcg(257)
    wide_hash = 0xCBF29CE484222325
    round_trips = 0
    for _ in range(RANDOM_CASES):
        units = random_units(rng)
        data = from_wide(units)
        wide_hash = fnv(wide_hash, data + b"\x00")
        if to_wide(data) == units:
            round_trips += 1
    expect.append(f"random wide {RANDOM_CASES} {round_trips} {wide_hash}")
    byte_hash = 0xCBF29CE484222325
    lossy_hash = 0xCBF29CE484222325
    well = 0
    utf8 = 0
    for _ in range(RANDOM_CASES):
        data = random_bytes(rng)
        units = to_wide(data)
        if units is not None:
            well += 1
            byte_hash = fnv(byte_hash, b"".join(u.to_bytes(2, "little") for u in units) + b"\xff\xff")
        if is_utf8(data):
            utf8 += 1
        lossy_hash = fnv(lossy_hash, lossy(data) + b"\x00")
    expect.append(f"random bytes {RANDOM_CASES} {well} {utf8} {byte_hash} {lossy_hash}")
    return expect


def literal(values, suffix, width):
    return "[" + ", ".join(f"0x{v:0{width}X}{suffix}" for v in values) + "][..]"


def main():
    wide = "\n".join(f"    showWide({literal(u, 'u16', 4)});" if u else "    showWide(Vector<u16>.create().view());"
                     for u in WIDE_CASES)
    byte = "\n".join(f"    showBytes({literal(bytes.fromhex(t), 'u8', 2)});" if t else
                     "    showBytes(Vector<u8>.create().view());" for t in BYTE_CASES)
    units = ", ".join(f"0x{u:04X}u16" for u in UNIT_POOL)
    pieces = "\n".join(f"        {k} => {{ bytes.extend({literal(bytes.fromhex(t), 'u8', 2)})!; }}"
                       for k, t in enumerate(BYTE_POOL[:-1]))
    last = literal(bytes.fromhex(BYTE_POOL[-1]), "u8", 2)
    source = (PROGRAM.replace("WIDE_CASES", wide).replace("BYTE_CASES", byte).replace("UNIT_POOL", units)
              .replace("UNIT_COUNT", str(len(UNIT_POOL))).replace("BYTE_PIECES", pieces)
              .replace("BYTE_LAST", last).replace("BYTE_COUNT", str(len(BYTE_POOL)))
              .replace("RANDOM_CASES", str(RANDOM_CASES)))
    source += "\n".join(f"// expect-stdout: {line}" for line in expectations()) + "\n"
    path = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "os-string-wtf8.cee")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", path], check=True)


PROGRAM = r'''// GENERATED by tools/oracles/gen_os_string_cases.py: lib/core's OsString and OsStringView (ADR 0257) against Python's
// `surrogatepass` codecs. Each case prints its units or bytes, the WTF-8 or UTF-16 made of them, `isWtf8`, `isUtf8`
// and the lossy text's bytes, in hex; then seeded cases (an LCG the generator runs too) summarised with FNV-1a.

fn hexBytes(bytes: Slice<u8>) -> String {
    var out = String.create();
    if (bytes.length == 0) {
        out.append("-")!;
    }
    for (b in bytes) {
        out.appendHex(b as u64, 2, false)!;
    }
    return out;
}

fn hexUnits(units: Slice<u16>) -> String {
    var out = String.create();
    if (units.length == 0) {
        out.append("-")!;
    }
    for (i in 0..units.length) {
        if (i > 0) {
            out.append(" ")!;
        }
        out.appendHex(units[i] as u64, 4, false)!;
    }
    return out;
}

fn showWide(units: Slice<u16>) {
    let name = OsString.fromWide(units)!;
    Console.write("wide ");
    Console.write(hexUnits(units).view());
    Console.write(" -> ");
    Console.write(hexBytes(name.bytes()).view());
    Console.write(" ");
    Console.write(name.isWtf8());
    Console.write(" ");
    Console.writeLine(name.isUtf8());
}

fn showBytes(bytes: Slice<u8>) {
    let name = OsStringView.fromBytes(bytes);
    let wide = name.toWide()!;
    Console.write("bytes ");
    Console.write(hexBytes(bytes).view());
    Console.write(" -> ");
    Console.write(name.isWtf8());
    Console.write(" ");
    match (wide) {
        Some(units) => { Console.write(hexUnits(units.view()).view()); }
        null => { Console.write("null"); }
    }
    Console.write(" ");
    Console.write(name.isUtf8());
    Console.write(" ");
    let text = name.toUtf8Lossy()!;
    Console.writeLine(hexBytes(text.view().bytes()).view());
}

fn next(state: borrow mut u64) -> u64 {
    state = state.wrappingMul(6364136223846793005).wrappingAdd(1442695040888963407);
    return state >> 33;
}

fn fnv(h: u64, bytes: Slice<u8>) -> u64 {
    var hash = h;
    for (b in bytes) {
        hash = (hash ^ (b as u64)).wrappingMul(0x100000001B3);
    }
    return hash;
}

fn randomUnits(state: borrow mut u64) -> Vector<u16> {
    let pool = [UNIT_POOL];
    var units = Vector<u16>.create();
    let count = next(mut state) % 9;
    for (k in 0..count) {
        let x = next(mut state);
        units.push(if (x % 10 < 7) pool[((x >> 4) % UNIT_COUNT) as usize] else truncate<u16>((x >> 4) % 0x10000))!;
    }
    return units;
}

fn addPiece(k: u64, bytes: borrow mut Vector<u8>) {
    match (k) {
BYTE_PIECES
        _ => { bytes.extend(BYTE_LAST)!; }
    }
}

fn randomBytes(state: borrow mut u64) -> Vector<u8> {
    var bytes = Vector<u8>.create();
    let count = next(mut state) % 7;
    for (k in 0..count) {
        addPiece(next(mut state) % BYTE_COUNT, mut bytes);
    }
    return bytes;
}

fn main() {
WIDE_CASES
BYTE_CASES
    var state: u64 = 257;
    var wideHash: u64 = 0xCBF29CE484222325;
    var roundTrips = 0;
    let end = [0u8];
    for (i in 0..RANDOM_CASES) {
        let units = randomUnits(mut state);
        let name = OsString.fromWide(units.view())!;
        wideHash = fnv(fnv(wideHash, name.bytes()), end[..]);
        match (name.view().toWide()!) {
            Some(back) => {
                if (back == units) {
                    roundTrips += 1;
                }
            }
            null => {}
        }
    }
    Console.write("random wide RANDOM_CASES ");
    Console.write(roundTrips);
    Console.write(" ");
    Console.writeLine(wideHash);
    var byteHash: u64 = 0xCBF29CE484222325;
    var lossyHash: u64 = 0xCBF29CE484222325;
    var well = 0;
    var utf8 = 0;
    let separator = [0xFFu8, 0xFF];
    for (i in 0..RANDOM_CASES) {
        let bytes = randomBytes(mut state);
        let name = OsStringView.fromBytes(bytes.view());
        match (name.toWide()!) {
            Some(units) => {
                well += 1;
                for (u in units) {
                    let pair = [truncate<u8>(u & 0xFF), truncate<u8>(u >> 8)];
                    byteHash = fnv(byteHash, pair[..]);
                }
                byteHash = fnv(byteHash, separator[..]);
            }
            null => {}
        }
        if (name.isUtf8()) {
            utf8 += 1;
        }
        let text = name.toUtf8Lossy()!;
        lossyHash = fnv(fnv(lossyHash, text.view().bytes()), end[..]);
    }
    Console.write("random bytes RANDOM_CASES ");
    Console.write(well);
    Console.write(" ");
    Console.write(utf8);
    Console.write(" ");
    Console.write(byteHash);
    Console.write(" ");
    Console.writeLine(lossyHash);
}
'''

if __name__ == "__main__":
    main()
