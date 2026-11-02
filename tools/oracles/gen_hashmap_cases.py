#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/hash-map.cee: lib/std's Hasher and HashMap (ADR 0052) against oracles in
this file.

1. SipHash-1-3 of the messages 00, 00 01, … (lengths 0 to 15) under the key 00 01 … 0f. This file's SipHash is
   checked first, with two compression and four finalisation rounds, against SipHash-2-4's published test vectors
   (the paper's appendix, and the reference implementation's vectors.h): a failure stops the generator.
2. A random sequence of put, get, remove and contains on a HashMap<u64, u64> (seeded, 3000 operations, keys from a
   small range so keys repeat and are removed), against a model of ADR 0052 §2's order: a new key is appended; a put
   of a present key replaces its value in place; a removal moves the last entry into the removed one's place. The
   expected lines are the FNV-1a of every result, then the final iteration order.
3. String keys: header names put, got, replaced and removed, and the final order.
1b (printed after 1): SipHash-1-3 of mixed feeds, p bytes then a word then p + 9 bytes for p in 0..8, so the hasher's
   word paths meet a pending tail.

usage: python3 tools/oracles/gen_hashmap_cases.py   (formats with $CEESAFEC, default out/ceesafec)
"""

import os
import random
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
MASK = (1 << 64) - 1


def rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & MASK


def siphash(k0, k1, data, c, d):
    v0 = k0 ^ 0x736F6D6570736575
    v1 = k1 ^ 0x646F72616E646F6D
    v2 = k0 ^ 0x6C7967656E657261
    v3 = k1 ^ 0x7465646279746573

    def rounds(n):
        nonlocal v0, v1, v2, v3
        for _ in range(n):
            v0 = (v0 + v1) & MASK; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32)
            v2 = (v2 + v3) & MASK; v3 = rotl(v3, 16); v3 ^= v2
            v0 = (v0 + v3) & MASK; v3 = rotl(v3, 21); v3 ^= v0
            v2 = (v2 + v1) & MASK; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32)

    whole = len(data) - len(data) % 8
    for i in range(0, whole, 8):
        m = int.from_bytes(data[i:i + 8], "little")
        v3 ^= m
        rounds(c)
        v0 ^= m
    last = (len(data) & 0xFF) << 56 | int.from_bytes(data[whole:], "little")
    v3 ^= last
    rounds(c)
    v0 ^= last
    v2 ^= 0xFF
    rounds(d)
    return v0 ^ v1 ^ v2 ^ v3


# SipHash-2-4, key 00..0f, messages 00, 00 01, … : the first entries of the reference implementation's vectors.h.
SIPHASH24 = [0x726FDB47DD0E0E31, 0x74F839C593DC67FD, 0x0D6C8009D9A94F5A, 0x85676696D7FB7E2D]


def fnv(h, value):
    for b in value.to_bytes(8, "little"):
        h = ((h ^ b) * 0x100000001B3) & MASK
    return h


def main():
    key = bytes(range(16))
    k0 = int.from_bytes(key[:8], "little")
    k1 = int.from_bytes(key[8:], "little")
    for n, want in enumerate(SIPHASH24):
        got = siphash(k0, k1, bytes(range(n)), 2, 4)
        if got != want:
            sys.exit(f"gen_hashmap_cases: this oracle's SipHash-2-4 of {n} bytes is {got:#x}, not {want:#x}")
    expected = [str(siphash(k0, k1, bytes(range(n)), 1, 3)) for n in range(16)]
    # 1b. Mixed feeds: p bytes, a word, then p + 9 bytes, so the word and the runs of whole words meet a pending tail.
    word = 0x1122334455667788
    for p_len in range(9):
        data = bytes(range(p_len)) + word.to_bytes(8, "little") + bytes(16 + k for k in range(p_len + 9))
        expected.append(str(siphash(k0, k1, data, 1, 3)))

    # 2. The integer map.
    rng = random.Random(49)
    ops = []
    order = []      # keys in iteration order
    values = {}
    h = 0xCBF29CE484222325
    for _ in range(3000):
        kind = rng.choice("ppppggrc")
        k = rng.randrange(0, 600)
        v = rng.randrange(0, 1 << 32)
        ops.append((kind, k, v))
        if kind == "p":
            if k in values:
                h = fnv(h, 1 + values[k])
            else:
                h = fnv(h, 0)
                order.append(k)
            values[k] = v
        elif kind == "g":
            h = fnv(h, 1 + values[k] if k in values else 0)
        elif kind == "r":
            if k in values:
                h = fnv(h, 1 + values[k])
                i = order.index(k)
                order[i] = order[-1]
                order.pop()
                del values[k]
            else:
                h = fnv(h, 0)
        else:
            h = fnv(h, 1 if k in values else 0)
    expected.append(f"{h} {len(order)}")
    order_line = " ".join(f"{k}={values[k]}" for k in order[:40])
    expected.append(order_line)

    # 3. String keys.
    names = ["host", "accept", "user-agent", "content-length", "cookie", "x-trace"]
    smap_order = []
    svalues = {}
    script = [("p", "host", 1), ("p", "accept", 2), ("p", "user-agent", 3), ("p", "content-length", 4),
              ("p", "cookie", 5), ("r", "accept", 0), ("p", "x-trace", 6), ("p", "host", 7), ("r", "zzz", 0),
              ("r", "user-agent", 0)]
    lines = []
    for kind, name, v in script:
        if kind == "p":
            old = svalues.get(name)
            if name not in svalues:
                smap_order.append(name)
            svalues[name] = v
            lines.append(f"put {name}: {old if old is not None else '-'}")
        else:
            old = svalues.pop(name, None)
            if old is not None:
                i = smap_order.index(name)
                smap_order[i] = smap_order[-1]
                smap_order.pop()
            lines.append(f"remove {name}: {old if old is not None else '-'}")
    expected += lines
    expected.append(" ".join(f"{n}={svalues[n]}" for n in smap_order))

    ops_array = ", ".join(f"{('pgrc'.index(kind))}u32" for kind, _, _ in ops)
    keys_array = ", ".join(f"{k}u64" for _, k, _ in ops)
    values_array = ", ".join(f"{v}u64" for _, _, v in ops)
    script_calls = []
    for kind, name, v in script:
        if kind == "p":
            script_calls.append(f'    showPut(mut names, "{name}", {v});')
        else:
            script_calls.append(f'    showRemove(mut names, "{name}");')

    source = f"""// GENERATED by tools/oracles/gen_hashmap_cases.py: edit that, not this file.
// lib/std's Hasher (SipHash-1-3) and HashMap (ADR 0052), each expected line from the generator's oracles: its own
// SipHash (checked against SipHash-2-4's published vectors), and a model of the map's order (a removal moves the last
// entry into the gap).

fn line(text: StringView) {{
    Console.writeLine(text);
}}

fn fnv(h: u64, value: u64) -> u64 {{
    var x = h;
    for (k in 0..8u64) {{
        x = (x ^ ((value >> (8 * k)) & 0xFF)).wrappingMul(0x100000001B3);
    }}
    return x;
}}

fn text(s: StringView) -> String {{
    var t = String.create();
    t.append(s)!;
    return t;
}}

fn showPut(map: borrow mut HashMap<String, u64>, name: StringView, value: u64) {{
    Console.write("put ");
    Console.write(name);
    Console.write(": ");
    match (map.put(text(name), value)!) {{
        Some(old) => {{ Console.writeLine(old); }}
        null => {{ Console.writeLine("-"); }}
    }}
}}

fn showRemove(map: borrow mut HashMap<String, u64>, name: StringView) {{
    Console.write("remove ");
    Console.write(name);
    Console.write(": ");
    let key = text(name);
    match (map.remove(borrow key)) {{
        Some(old) => {{ Console.writeLine(old); }}
        null => {{ Console.writeLine("-"); }}
    }}
}}

fn main() {{
    // 1. SipHash-1-3 under the key 00..0f.
    var message = [0u8; 16];
    for (k in 0..16usize) {{
        message[k] = truncate<u8>(k);
    }}
    for (n in 0..16usize) {{
        var h = Hasher.create(0x0706050403020100, 0x0F0E0D0C0B0A0908);
        h.bytes(message[0..n]);
        Console.writeLine(h.finish());
    }}
    // 1b. Mixed feeds: `p` bytes, a word, then `p + 9` bytes (ADR 0052 §8: a word and whole runs of words are
    // compressed directly only while nothing is pending).
    var long = [0u8; 24];
    for (k in 0..24usize) {{
        long[k] = truncate<u8>(16 + k);
    }}
    for (p in 0..9usize) {{
        var h = Hasher.create(0x0706050403020100, 0x0F0E0D0C0B0A0908);
        h.bytes(message[0..p]);
        h.u64(0x1122334455667788);
        h.bytes(long[0..(p + 9)]);
        Console.writeLine(h.finish());
    }}

    // 2. The integer map against the model.
    let kinds = [{ops_array}];
    let keys = [{keys_array}];
    let values = [{values_array}];
    var map = HashMap<u64, u64>.withSeed(1, 2);
    var h: u64 = 0xCBF29CE484222325;
    for (i in 0..kinds.length) {{
        let k = keys[i];
        if (kinds[i] == 0) {{
            h = fnv(h, match (map.put(k, values[i])!) {{
                Some(old) => 1 + old,
                null => 0,
            }});
        }} else if (kinds[i] == 1) {{
            h = fnv(h, match (map.get(borrow k)) {{
                Some(v) => 1 + v,
                null => 0,
            }});
        }} else if (kinds[i] == 2) {{
            h = fnv(h, match (map.remove(borrow k)) {{
                Some(old) => 1 + old,
                null => 0,
            }});
        }} else {{
            h = fnv(h, if (map.contains(borrow k)) 1 else 0);
        }}
    }}
    Console.write(h);
    Console.write(" ");
    Console.writeLine(map.length());
    for (i in 0..(if (map.length() < 40) map.length() else 40)) {{
        if (i > 0) {{
            Console.write(" ");
        }}
        Console.write(map.keyAt(i));
        Console.write("=");
        Console.write(map.valueAt(i));
    }}
    Console.writeLine("");

    // 3. String keys, under the process's random key: the output does not depend on it.
    var names = HashMap<String, u64>.create();
{chr(10).join(script_calls)}
    for (i in 0..names.length()) {{
        if (i > 0) {{
            Console.write(" ");
        }}
        let name = names.keyAt(i);
        Console.write(name.view());
        Console.write("=");
        Console.write(names.valueAt(i));
    }}
    Console.writeLine("");
}}
{chr(10).join("// expect-stdout: " + e for e in expected)}
"""
    path = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "hash-map.cee")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", path], check=True)
    print(f"gen_hashmap_cases: wrote {path} ({len(expected)} lines)")


if __name__ == "__main__":
    main()
