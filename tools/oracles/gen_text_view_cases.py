#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/adr0253-text-chars.cee: lib/core's TextView (ADR 0253) against
Python as the oracle.

- `chars()`: Python's UTF-8 decoder, with an error handler registered by `codecs.register_error`, is called once per
  maximal subpart of an ill-formed sequence (the Unicode Standard's §3.9) with its start and end; the characters
  between are decoded. Each `TextChar` is then (value, offset, length, valid), U+FFFD for a subpart.
- `indexOf`, `lastIndexOf` and `contains` on a `ByteStringView` are `bytes.find`, `rfind`, `in`; on a `StringView`
  (for the strings that are UTF-8) a match counts only where it starts and ends on a character boundary.
- `compareIgnoreAsciiCase` is Python's order of the bytes with `A`-`Z` made `a`-`z`; `equalsIgnoreAsciiCase` its
  equality; `split(",")` is `bytes.split(b",")`.

For each fixed case the program prints the characters, one `value@offset+length` (with `!` for a subpart) each;
then 2,000 seeded strings, mostly UTF-8 with stray and truncated bytes, give summary lines of the same, hashed.

usage: python3 tools/oracles/gen_text_view_cases.py   (formats with $CEESAFEC, default out/ceesafec)
"""

import codecs
import os
import subprocess

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
MASK = (1 << 64) - 1
RANDOM_CASES = 2000
NEEDLES = ["2c", "a9", "c3a9", "61", "e282ac", "ff", ""]

CASES = [
    "", "61", "c3a9", "e282ac", "f09f9880", "ff", "80", "c0af", "e080", "eda080", "e282", "f09f98", "f4908080",
    "61ff62", "61f18080e180c2628063", "e180e2f09192f1bf41", "22eda08022", "636166c3a973",
]


def chars(data):
    """(value, offset, length, valid) for each character of `data`, a maximal subpart standing as U+FFFD."""
    spans = []

    def record(error):
        spans.append((error.start, error.end))
        return ("�", error.end)

    codecs.register_error("text-view-oracle", record)
    data.decode("utf-8", "text-view-oracle")
    out = []
    at = 0
    for start, end in spans + [(len(data), len(data))]:
        for ch in data[at:start].decode("utf-8"):
            n = len(ch.encode("utf-8"))
            out.append((ord(ch), at, n, True))
            at += n
        if end > start:
            out.append((0xFFFD, start, end - start, False))
        at = end
    return out


def chars_line(data):
    return " ".join(f"{v}@{o}+{n}{'' if ok else '!'}" for v, o, n, ok in chars(data)) or "-"


def is_utf8(data):
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def boundaries(data):
    """The positions a StringView may be sliced at."""
    return {i for i in range(len(data) + 1) if i == len(data) or (data[i] & 0xC0) != 0x80}


def find_whole(data, needle, last):
    places = [i for i in range(len(data) - len(needle) + 1) if data[i:i + len(needle)] == needle]
    ok = boundaries(data)
    places = [i for i in places if i in ok and i + len(needle) in ok]
    if not places:
        return None
    return places[-1] if last else places[0]


def fold(data):
    return bytes(b + 32 if 0x41 <= b <= 0x5A else b for b in data)


def sign(x):
    return (x > 0) - (x < 0)


class Seeded:
    def __init__(self, seed):
        self.state = seed

    def next(self):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & MASK
        return self.state >> 33


def random_text(r):
    out = bytearray()
    for _ in range(r.next() % 12):
        x = r.next()
        kind = x % 9
        if kind < 2:
            out.append(0x61 + (x >> 3) % 26)
        elif kind == 2:
            out.append(0x41 + (x >> 3) % 26)
        elif kind == 3:
            out += b"\xc3\xa9"
        elif kind == 4:
            out += b"\xe2\x82\xac"
        elif kind == 5:
            out += b","
        elif kind == 6:
            out += b"\xf0\x9f\x98\x80"
        elif kind == 7:
            out.append((x >> 3) & 0xFF)
        else:
            out.append((x >> 3) & 0xFF)
            out.append((x >> 11) & 0xFF)
    return bytes(out)


def fnv(h, value):
    return ((h ^ (value & MASK)) * 0x100000001B3) & MASK


NONE = 999


def summary():
    r = Seeded(4253)
    char_hash = search_hash = view_hash = split_hash = 0xCBF29CE484222325
    valid = before = same = after = folded_equal = 0
    previous = b""
    for _ in range(RANDOM_CASES):
        data = random_text(r)
        for v, o, n, ok in chars(data):
            char_hash = fnv(fnv(fnv(fnv(char_hash, v), o), n), 1 if ok else 0)
        for h in NEEDLES:
            needle = bytes.fromhex(h)
            first = data.find(needle)
            last = data.rfind(needle)
            search_hash = fnv(fnv(fnv(search_hash, NONE if first < 0 else first), NONE if last < 0 else last),
                              1 if needle in data else 0)
            if is_utf8(data):
                w_first = find_whole(data, needle, False)
                w_last = find_whole(data, needle, True)
                view_hash = fnv(fnv(view_hash, NONE if w_first is None else w_first), NONE if w_last is None else w_last)
        if is_utf8(data):
            valid += 1
        pieces = data.split(b",")
        split_hash = fnv(split_hash, len(pieces))
        for p in pieces:
            split_hash = fnv(split_hash, len(p))
        s = sign((fold(previous) > fold(data)) - (fold(previous) < fold(data)))
        if s < 0:
            before += 1
        elif s > 0:
            after += 1
        else:
            same += 1
        if fold(previous) == fold(data):
            folded_equal += 1
        previous = data
    return [
        f"chars {char_hash}",
        f"bytes search {search_hash}",
        f"utf8 {valid} search {view_hash}",
        f"split {split_hash}",
        f"nocase {before} {same} {after} equal {folded_equal}",
    ]


def main():
    expected = [chars_line(bytes.fromhex(h)) for h in CASES]
    expected += summary()
    calls = "\n".join(f'    case("{h}");' for h in CASES)
    needles = ", ".join(f'"{h}"' for h in NEEDLES)
    source = f"""// GENERATED by tools/oracles/gen_text_view_cases.py: edit that, not this file.
// lib/core's TextView (ADR 0253) against Python: the characters of each case (`chars()`, one U+FFFD per maximal
// subpart, from the decoder's error spans), then {RANDOM_CASES} seeded strings, hashed: their characters, `indexOf`,
// `lastIndexOf` and `contains` of {len(NEEDLES)} needles on a `ByteStringView` (`bytes.find`) and, for UTF-8, on a
// `StringView` (whole characters only), `split(",")`, and `compareIgnoreAsciiCase` between neighbours.

fn hexValue(c: u8) -> u8 => if (c >= 'a') c - 'a' + 10 else c - '0';

fn decode(hex: StringView) -> Vector<u8> {{
    var out = Vector<u8>.create();
    let b = hex.bytes();
    var i: usize = 0;
    while (i + 1 < b.length) {{
        out.push(hexValue(b[i]) * 16 + hexValue(b[i + 1]))!;
        i += 2;
    }}
    return out;
}}

fn case(hex: StringView) {{
    let raw = decode(hex);
    var line = String.create();
    for (c in ByteStringView.fromBytes(raw.view()).chars()) {{
        if (line.length > 0) {{
            line.append(" ")!;
        }}
        line.appendNumber(c.value)!;
        line.append("@")!;
        line.appendNumber(c.offset)!;
        line.append("+")!;
        line.appendNumber(c.length)!;
        if (!c.valid) {{
            line.append("!")!;
        }}
    }}
    if (line.length == 0) {{
        line.append("-")!;
    }}
    Console.writeLine(line.view());
}}

fn next(state: borrow mut u64) -> u64 {{
    state = state.wrappingMul(6364136223846793005).wrappingAdd(1442695040888963407);
    return state >> 33;
}}

fn randomText(state: borrow mut u64) -> ByteString {{
    var out = ByteString.create();
    let pieces = next(mut state) % 12;
    for (k in 0..pieces) {{
        let x = next(mut state);
        let kind = x % 9;
        if (kind < 2) {{
            out.appendByte(truncate<u8>(0x61 + (x >> 3) % 26))!;
        }} else if (kind == 2) {{
            out.appendByte(truncate<u8>(0x41 + (x >> 3) % 26))!;
        }} else if (kind == 3) {{
            out.append("é")!;
        }} else if (kind == 4) {{
            out.append("€")!;
        }} else if (kind == 5) {{
            out.append(",")!;
        }} else if (kind == 6) {{
            out.append("😀")!;
        }} else if (kind == 7) {{
            out.appendByte(truncate<u8>(x >> 3))!;
        }} else {{
            out.appendByte(truncate<u8>(x >> 3))!;
            out.appendByte(truncate<u8>(x >> 11))!;
        }}
    }}
    return out;
}}

fn fnv(h: u64, value: u64) -> u64 => (h ^ value).wrappingMul(0x100000001B3);

fn place(p: usize?) -> u64 => (p ?? 999) as u64;

/// The searches of one needle on `t`, hashed into `h`.
fn searches<T: TextView, N: TextView>(h: u64, t: T, needle: N, withContains: bool) -> u64 {{
    let first = fnv(fnv(h, place(t.indexOf(needle))), place(t.lastIndexOf(needle)));
    return if (withContains) fnv(first, if (t.contains(needle)) 1 else 0) else first;
}}

fn random() {{
    var state: u64 = 4253;
    var charHash: u64 = 0xCBF29CE484222325;
    var searchHash: u64 = 0xCBF29CE484222325;
    var viewHash: u64 = 0xCBF29CE484222325;
    var splitHash: u64 = 0xCBF29CE484222325;
    var valid: u64 = 0;
    var before: u64 = 0;
    var same: u64 = 0;
    var after: u64 = 0;
    var foldedEqual: u64 = 0;
    let needleTexts = [{needles}];
    var needles = Vector<Vector<u8>>.create();
    for (h in needleTexts[..]) {{
        needles.push(decode(h))!;
    }}
    var previous = ByteString.create();
    for (k in 0..{RANDOM_CASES}u64) {{
        let text = randomText(mut state);
        let v = text.view();
        for (c in v.chars()) {{
            charHash = fnv(fnv(fnv(fnv(charHash, c.value as u64), c.offset as u64), c.length as u64), if (c.valid) 1 else 0);
        }}
        let utf8 = v.toUtf8();
        for (n in 0..needles.length) {{
            let needle = needles[n].view();
            searchHash = searches(searchHash, v, needle, true);
            match (utf8) {{
                Some(s) => {{ viewHash = searches(viewHash, s, ByteStringView.fromBytes(needle), false); }}
                null => {{}}
            }}
        }}
        if (utf8.isSome()) {{
            valid += 1;
        }}
        var count: u64 = 0;
        for (piece in v.split(",")) {{
            count += 1;
        }}
        splitHash = fnv(splitHash, count);
        for (piece in v.split(",")) {{
            splitHash = fnv(splitHash, piece.length() as u64);
        }}
        let order = previous.view().compareIgnoreAsciiCase(v);
        if (order < 0) {{
            before += 1;
        }} else if (order > 0) {{
            after += 1;
        }} else {{
            same += 1;
        }}
        if (previous.view().equalsIgnoreAsciiCase(v)) {{
            foldedEqual += 1;
        }}
        previous = text;
    }}
    Console.write("chars ");
    Console.writeLine(charHash);
    Console.write("bytes search ");
    Console.writeLine(searchHash);
    Console.write("utf8 ");
    Console.write(valid);
    Console.write(" search ");
    Console.writeLine(viewHash);
    Console.write("split ");
    Console.writeLine(splitHash);
    Console.write("nocase ");
    Console.write(before);
    Console.write(" ");
    Console.write(same);
    Console.write(" ");
    Console.write(after);
    Console.write(" equal ");
    Console.writeLine(foldedEqual);
}}

fn main() {{
{calls}
    random();
}}
"""
    source += "".join(f"// expect-stdout: {e}\n" for e in expected)
    path = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "adr0253-text-chars.cee")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", path], check=True)
    print(f"gen_text_view_cases: wrote {path} ({len(expected)} lines)")


if __name__ == "__main__":
    main()
