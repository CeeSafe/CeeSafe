#!/usr/bin/env python3
"""Independent oracle for tests/support (ROADMAP P1: "each one is tested against an independent model").

Recomputes, in Python, the summary lines that tests/support/run-pass/*.cee print, from the same seeded operation
sequences. The tests also compare against a naive model inside the program on every step; this script pins the
summaries to values computed outside CeeSafe. Run it and compare with the tests' `// expect-stdout:` lines:

    python3 tools/oracles/support_models.py
"""

MASK = (1 << 64) - 1


def lcg(state):
    return (state * 6364136223846793005 + 1442695040888963407) & MASK


def map_model():
    keys, values, index = [], [], {}
    x = 12345
    puts = hits = misses = 0
    for step in range(20000):
        x = lcg(x)
        key = (x >> 33) % 600
        op = (x >> 20) % 4
        if op < 3:
            puts += 1
            if key in index:
                values[index[key]] = step
            else:
                index[key] = len(keys)
                keys.append(key)
                values.append(step)
        else:
            if key in index:
                hits += 1
            else:
                misses += 1
    checksum = 0
    for k, v in zip(keys, values):
        checksum = (checksum * 31 + k * 1000003 + v) & MASK
    return [f"map: ops 20000, keys {len(keys)}, puts {puts}, hits {hits}, misses {misses}, checksum {checksum}"]


def interner_model():
    names, ids = [], {}
    x = 777
    checksum = 0
    total_bytes = 0
    for _ in range(5000):
        x = lcg(x)
        length = 1 + (x >> 40) % 6
        name = bytearray()
        for _ in range(length):
            x = lcg(x)
            name.append(ord("a") + (x >> 50) % 4)
        name = bytes(name)
        if name not in ids:
            ids[name] = len(names)
            names.append(name)
            total_bytes += len(name)
        checksum = (checksum * 131 + ids[name]) & MASK
    return [f"interner: 5000 interns, {len(names)} names, {total_bytes} bytes, checksum {checksum}"]


def bits_model():
    size = 1000
    a, b = [False] * size, [False] * size
    x = 99
    for _ in range(30000):
        x = lcg(x)
        bit = (x >> 33) % size
        op = (x >> 16) % 3
        target = a if (x >> 8) % 2 == 0 else b
        if op == 0:
            target[bit] = True
        elif op == 1:
            target[bit] = False
    union = [p or q for p, q in zip(a, b)]
    inter = [p and q for p, q in zip(a, b)]
    diff = [p and not q for p, q in zip(a, b)]
    return [f"bits: a {sum(a)}, b {sum(b)}, union {sum(union)}, intersection {sum(inter)}, difference {sum(diff)}"]


def bytes_order():
    words = [b"", b"a", b"ab", b"abc", b"b", b"B", b"_", b"z", b"\xc3\xa9", b"\xe2\x82\xac", b"aa"]
    order = sorted(range(len(words)), key=lambda i: words[i])
    return ["order: " + " ".join(str(i) for i in order)]


if __name__ == "__main__":
    for line in map_model() + interner_model() + bits_model() + bytes_order():
        print(line)
