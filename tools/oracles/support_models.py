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


def ordered_edges_model():
    """tests/support/run-pass/ordered-map-edges.cee: lib/std's OrderedMap after loads in key order (ADR 0194 §2.4)."""
    import bisect
    x = 4242
    puts = removals = hits = 0
    checksum = 0
    for r in range(40):
        x = lcg(x)
        n = 50 + (x >> 33) % 3000
        keys, values = [], {}

        def put(k, v):
            nonlocal puts
            puts += 1
            if k not in values:
                bisect.insort(keys, k)
            values[k] = v

        def remove(k):
            nonlocal removals, hits
            removals += 1
            if k in values:
                hits += 1
                del values[k]
                keys.pop(bisect.bisect_left(keys, k))

        for i in range(n):
            put(4 * i + 4 if r % 2 == 0 else 4 * (n - i), i)
        for step in range(1500):
            x = lcg(x)
            op = (x >> 20) % 8
            a = x >> 33
            if op < 3:
                put(a % (4 * n + 8), x >> 40)
            elif op < 5:
                remove(a % (4 * n + 8))
            elif op == 5:
                put((keys[-1] if keys else 0) + 1 + a % 3, step)
            elif op == 6:
                put(keys[0] - 1 if keys and keys[0] > 0 else a % 100, step)
            elif keys:
                remove(keys[0] if a & 1 == 0 else keys[-1])
            else:
                remove(a)
        for k in keys:
            checksum = (checksum * 31 + k * 1000003 + values[k]) & MASK
        order = list(keys)
        j = len(order)
        while j > 1:
            x = lcg(x)
            p = (x >> 33) % j
            order[p], order[j - 1] = order[j - 1], order[p]
            j -= 1
        for k in order:
            remove(k)
    return [f"ordered map edges: rounds 40, puts {puts}, removals {removals}, hits {hits}, checksum {checksum}"]


def bytes_order():
    words = [b"", b"a", b"ab", b"abc", b"b", b"B", b"_", b"z", b"\xc3\xa9", b"\xe2\x82\xac", b"aa"]
    order = sorted(range(len(words)), key=lambda i: words[i])
    return ["order: " + " ".join(str(i) for i in order)]


def byte_string_keys_model():
    """tests/support/run-pass/byte-string-keys.cee: ByteString keys of HashMap and OrderedMap (ADR 0231)."""
    letters = [0x61, 0x62, 0xFF, 0xC3, 0xA9, 0x7A]

    def make_key(x):
        out = bytearray()
        y = x
        for _ in range((x >> 33) % 6):
            y = lcg(y)
            out.append(letters[(y >> 40) % 6])
        return bytes(out)

    values = {}
    x = 4242
    puts = hits = removals = 0
    for step in range(6000):
        x = lcg(x)
        key = make_key(x)
        op = (x >> 20) % 4
        if op < 2:
            puts += 1
            values[key] = step
        elif op == 2:
            if key in values:
                hits += 1
        else:
            removals += 1
            values.pop(key, None)
    checksum = 0
    lossy = 0
    for key in sorted(values):
        for b in key:
            checksum = (checksum * 31 + b) & MASK
        checksum = (checksum * 1000003 + values[key]) & MASK
        lossy += len(key.decode("utf-8", "replace").encode("utf-8"))
    return [f"byte string keys: puts {puts}, hits {hits}, removals {removals}, keys {len(values)}, "
            f"checksum {checksum}, lossy {lossy}"]


if __name__ == "__main__":
    for line in (map_model() + interner_model() + bits_model() + bytes_order() + ordered_edges_model() +
                 byte_string_keys_model()):
        print(line)
