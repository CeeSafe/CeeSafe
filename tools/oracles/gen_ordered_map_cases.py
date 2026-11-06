#!/usr/bin/env python3
"""Writes tests/reference/tests/corpus/run-pass/ordered-map.cee: lib/std's OrderedMap (ADR 0075 §2.4) against a model in
this file, a sorted list of keys beside a dict.

The program and this file draw the same operations from the same xorshift64 (13, 7, 17; seed 0x9E3779B97F4A7C15), and
the expected lines are the model's:

1. fill: 10,000 ascending keys (0, 4, 8, …), so every split leaves half-full nodes and the tree is four levels deep
   (at most 32 × 32 × 31 + … = 32,767 entries fit in three).
2. mixed: 8,000 random puts, removals, gets, seeks, seekAfters and contains over keys below 40,000; each result is
   folded into an FNV-1a hash, printed with the length and the first and last keys every 1,000 operations.
3. scans: every entry forward (first, next) and backward (last, previous); 30 range scans (seek, then next until a key
   reaches the end of the range).
4. changes through valueAtMut (a walk that writes does not make its cursor stale) and getMut.
5. drain: every key below 40,000 removed in the order k = i × 7919 mod 40,000, so merges and borrows run at every
   level and the root shrinks to a leaf; then the map is empty.
6. refill after the drain (the freed nodes are reused), clear, and a put after the clear.
7. String keys (bytewise: "" first, a prefix before a longer key, uppercase before lowercase), Vector<u8> keys
   (bytewise, a prefix first), i8 and i64 keys (negative before positive) and bool keys, each against `sorted`.

usage: python3 tools/oracles/gen_ordered_map_cases.py   (formats with $CEESAFEC, default out/ceesafec)
"""

import bisect
import os
import subprocess

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
MASK = (1 << 64) - 1
FNV_START = 0xCBF29CE484222325


class Rng:
    def __init__(self):
        self.x = 0x9E3779B97F4A7C15

    def next(self):
        x = self.x
        x ^= (x << 13) & MASK
        x ^= x >> 7
        x ^= (x << 17) & MASK
        self.x = x
        return x


def fnv(h, value):
    for b in value.to_bytes(8, "little"):
        h = ((h ^ b) * 0x100000001B3) & MASK
    return h


class Model:
    """The oracle: keys kept sorted, values in a dict."""

    def __init__(self):
        self.keys = []
        self.values = {}

    def put(self, k, v):
        old = self.values.get(k)
        if old is None:
            bisect.insort(self.keys, k)
        self.values[k] = v
        return old

    def remove(self, k):
        old = self.values.pop(k, None)
        if old is not None:
            del self.keys[bisect.bisect_left(self.keys, k)]
        return old

    def seek(self, k):
        i = bisect.bisect_left(self.keys, k)
        return self.keys[i] if i < len(self.keys) else None

    def seek_after(self, k):
        i = bisect.bisect_right(self.keys, k)
        return self.keys[i] if i < len(self.keys) else None


def some(h, value):
    """The program's `fnv(h, 1 + x)` for a present x, `fnv(h, 0)` for none."""
    return fnv(h, 0 if value is None else 1 + value)


def u64_cases(lines):
    rng = Rng()
    m = Model()
    h = FNV_START
    # 1. fill
    for i in range(10000):
        h = some(h, m.put(4 * i, rng.next() >> 1))
    lines.append(f"fill {h} {len(m.keys)}")
    # 2. mixed
    for i in range(8000):
        op = rng.next() % 10
        k = (rng.next() >> 3) % 40000
        if op < 4:
            h = some(h, m.put(k, rng.next() >> 1))
        elif op < 6:
            h = some(h, m.remove(k))
        elif op == 6:
            h = some(h, m.values.get(k))
        elif op == 7 or op == 8:
            found = m.seek(k) if op == 7 else m.seek_after(k)
            h = some(h, found)
            if found is not None:
                h = some(h, m.values[found])
        else:
            h = fnv(h, 1 if k in m.values else 0)
        if (i + 1) % 1000 == 0:
            lines.append(f"mixed {i + 1} {h} {len(m.keys)} {m.keys[0]} {m.keys[-1]}")
    # 3. scans
    h = FNV_START
    for k in m.keys:
        h = fnv(fnv(h, k), m.values[k])
    lines.append(f"forward {h} {len(m.keys)}")
    h = FNV_START
    for k in reversed(m.keys):
        h = fnv(fnv(h, k), m.values[k])
    lines.append(f"backward {h} {len(m.keys)}")
    for _ in range(30):
        lo = rng.next() % 40000
        hi = lo + rng.next() % 3000
        inside = [k for k in m.keys if lo <= k < hi]
        lines.append(f"range {lo} {hi} {len(inside)} {sum(inside)}")
    # 4. changes in place
    for k in m.keys:
        if k % 7 == 0:
            m.values[k] ^= 0x5555
    for _ in range(100):
        k = rng.next() % 40000
        if k in m.values:
            m.values[k] += 1
    h = FNV_START
    for k in m.keys:
        h = fnv(fnv(h, k), m.values[k])
    lines.append(f"changed {h}")
    # 5. drain
    h = FNV_START
    for i in range(40000):
        h = some(h, m.remove((i * 7919) % 40000))
    lines.append(f"drained {h} {len(m.keys)} {'true' if not m.keys else 'false'}")
    lines.append("first null")
    # 6. refill, clear
    for i in range(500):
        m.put(1000 - 2 * i, i)
    lines.append(f"refill {len(m.keys)} {m.keys[0]} {m.keys[-1]} {m.values[m.seek(500)]}")
    lines.append("cleared 0 true null")
    lines.append("after clear 1 7")


def text_cases(lines):
    words = ["pear", "", "apple", "app", "Zebra", "apples", "banana", "a", "zebra", "éclair", "Apple", "b"]
    model = {}
    for i, w in enumerate(words):
        model[w] = i
    model["app"] = 100  # put again: replaced
    order = sorted(model, key=lambda s: s.encode("utf-8"))
    lines.append("text " + " ".join(f"[{w}]={model[w]}" for w in order))
    lines.append("text back " + " ".join(f"[{w}]" for w in reversed(order)))
    enc = sorted(w.encode("utf-8") for w in model)
    for probe in ["apple", "apq", "c", "zz"]:
        p = probe.encode("utf-8")
        at = next((w for w in enc if w >= p), None)
        after = next((w for w in enc if w > p), None)
        show = lambda w: "-" if w is None else "[" + w.decode("utf-8") + "]"
        lines.append(f"text seek {probe} {show(at)} {show(after)}")


BYTE_KEYS = [[], [0], [0, 0], [1], [0, 255], [255], [1, 2, 3], [1, 2], [0, 0, 0], [128]]


def byte_cases(lines):
    order = sorted(bytes(k) for k in BYTE_KEYS)
    lines.append("bytes " + " ".join("[" + ",".join(str(b) for b in k) + "]" for k in order))


SMALL = [5, -1, 127, -128, 0, 64, -64, 1, -2]
WIDE = [3, -9000000000, 9000000000, 0, -1, 1]


def int_cases(lines):
    lines.append("i8 " + " ".join(str(k) for k in sorted(SMALL)))
    lines.append("i64 " + " ".join(str(k) for k in sorted(WIDE)))
    lines.append("bool false=2 true=1")


PROGRAM = """// GENERATED by tools/oracles/gen_ordered_map_cases.py: edit that, not this file.
// lib/std's OrderedMap (ADR 0075 §2.4) against the generator's model (a sorted list beside a dict), the operations
// drawn from the same xorshift64 in both.

struct Rng {
    x: u64;

    fn next(borrow mut this) -> u64 {
        this.x = this.x ^ (this.x << 13);
        this.x = this.x ^ (this.x >> 7);
        this.x = this.x ^ (this.x << 17);
        return this.x;
    }
}

fn fnv(h: u64, value: u64) -> u64 {
    var x = h;
    for (k in 0..8u64) {
        x = (x ^ ((value >> (8 * k)) & 0xFF)).wrappingMul(0x100000001B3);
    }
    return x;
}

fn some(h: u64, value: u64?) -> u64 => match (value) {
    Some(v) => fnv(h, 1 + v),
    null => fnv(h, 0),
};

fn showLength(map: borrow OrderedMap<u64, u64>) {
    Console.write(" ");
    Console.write(map.length());
}

fn ends(map: borrow OrderedMap<u64, u64>) {
    Console.write(" ");
    Console.write(map.keyAt(map.first()!));
    Console.write(" ");
    Console.write(map.keyAt(map.last()!));
}

fn wholeHash(map: borrow OrderedMap<u64, u64>) -> u64 {
    var h: u64 = 0xCBF29CE484222325;
    var c = map.first();
    while (c != null) {
        let at = c!;
        h = fnv(fnv(h, map.keyAt(at)), map.valueAt(at));
        c = map.next(at);
    }
    return h;
}

fn u64Cases() {
    var rng = Rng { x: 0x9E3779B97F4A7C15 };
    var map = OrderedMap<u64, u64>.create();
    var h: u64 = 0xCBF29CE484222325;
    // 1. fill
    for (i in 0..10000u64) {
        h = some(h, map.put(4 * i, rng.next() >> 1)!);
    }
    Console.write("fill ");
    Console.write(h);
    showLength(borrow map);
    Console.writeLine("");
    // 2. mixed
    for (i in 0..8000u64) {
        let op = rng.next() % 10;
        let k = (rng.next() >> 3) % 40000;
        if (op < 4) {
            h = some(h, map.put(k, rng.next() >> 1)!);
        } else if (op < 6) {
            h = some(h, map.remove(borrow k));
        } else if (op == 6) {
            match (map.get(borrow k)) {
                Some(v) => {
                    h = fnv(h, 1 + v);
                }
                null => {
                    h = fnv(h, 0);
                }
            }
        } else if (op == 7 || op == 8) {
            let found = if (op == 7) map.seek(borrow k) else map.seekAfter(borrow k);
            match (found) {
                Some(at) => {
                    h = fnv(h, 1 + map.keyAt(at));
                    h = fnv(h, 1 + map.valueAt(at));
                }
                null => {
                    h = fnv(h, 0);
                }
            }
        } else {
            h = fnv(h, if (map.contains(borrow k)) 1 else 0);
        }
        if ((i + 1) % 1000 == 0) {
            Console.write("mixed ");
            Console.write(i + 1);
            Console.write(" ");
            Console.write(h);
            showLength(borrow map);
            ends(borrow map);
            Console.writeLine("");
        }
    }
    // 3. scans
    Console.write("forward ");
    Console.write(wholeHash(borrow map));
    showLength(borrow map);
    Console.writeLine("");
    h = 0xCBF29CE484222325;
    var back = map.last();
    var n: usize = 0;
    while (back != null) {
        let at = back!;
        h = fnv(fnv(h, map.keyAt(at)), map.valueAt(at));
        n += 1;
        back = map.previous(at);
    }
    Console.write("backward ");
    Console.write(h);
    Console.write(" ");
    Console.writeLine(n);
    for (r in 0..30) {
        let lo = rng.next() % 40000;
        let hi = lo + rng.next() % 3000;
        var count: u64 = 0;
        var sum: u64 = 0;
        var c = map.seek(borrow lo);
        while (c != null) {
            let at = c!;
            if (map.keyAt(at).compareKey(borrow hi) >= 0) {
                break;
            }
            count += 1;
            sum += map.keyAt(at);
            c = map.next(at);
        }
        Console.write("range ");
        Console.write(lo);
        Console.write(" ");
        Console.write(hi);
        Console.write(" ");
        Console.write(count);
        Console.write(" ");
        Console.writeLine(sum);
    }
    // 4. changes in place: a cursor walk that writes, then getMut
    var w = map.first();
    while (w != null) {
        let at = w!;
        if (map.keyAt(at) % 7 == 0) {
            let v = map.valueAtMut(at);
            v = v ^ 0x5555;
        }
        w = map.next(at);
    }
    for (r in 0..100) {
        let k = rng.next() % 40000;
        match (map.getMut(borrow k)) {
            Some(v) => {
                v += 1;
            }
            null => {}
        }
    }
    Console.write("changed ");
    Console.writeLine(wholeHash(borrow map));
    // 5. drain
    h = 0xCBF29CE484222325;
    for (i in 0..40000u64) {
        let k = (i * 7919) % 40000;
        h = some(h, map.remove(borrow k));
    }
    Console.write("drained ");
    Console.write(h);
    showLength(borrow map);
    Console.write(" ");
    Console.writeLine(map.isEmpty());
    if (map.first() == null && map.last() == null && map.seek(borrow h) == null) {
        Console.writeLine("first null");
    }
    // 6. refill, clear
    for (i in 0..500u64) {
        let old = map.put(1000 - 2 * i, i)!;
    }
    Console.write("refill");
    showLength(borrow map);
    ends(borrow map);
    Console.write(" ");
    let mid: u64 = 500;
    Console.writeLine(map.valueAt(map.seek(borrow mid)!));
    map.clear();
    Console.write("cleared");
    showLength(borrow map);
    Console.write(" ");
    Console.write(map.isEmpty());
    Console.writeLine(if (map.first() == null) " null" else " some");
    let again = map.put(3, 7)!;
    Console.write("after clear");
    showLength(borrow map);
    Console.write(" ");
    let three: u64 = 3;
    Console.writeLine(map.get(borrow three)!);
}

fn text(s: StringView) -> String {
    var t = String.create();
    t.append(s)!;
    return t;
}

fn showText(map: borrow OrderedMap<String, u32>, at: OrderedCursor?) {
    Console.write(" ");
    match (at) {
        Some(c) => {
            Console.write("[");
            let key = map.keyAt(c);
            Console.write(key.view());
            Console.write("]");
        }
        null => {
            Console.write("-");
        }
    }
}

fn textSeek(map: borrow OrderedMap<String, u32>, probe: StringView) {
    let key = text(probe);
    Console.write("text seek ");
    Console.write(probe);
    showText(map, map.seek(borrow key));
    showText(map, map.seekAfter(borrow key));
    Console.writeLine("");
}

fn textCases() {
    var map = OrderedMap<String, u32>.create();
    let words = ["pear", "", "apple", "app", "Zebra", "apples", "banana", "a", "zebra", "\\u{E9}clair", "Apple", "b"];
    for (i in 0..words.length) {
        let old = map.put(text(words[i]), truncate<u32>(i))!;
    }
    let replaced = map.put(text("app"), 100)!;
    Console.write("text");
    var c = map.first();
    while (c != null) {
        let at = c!;
        Console.write(" [");
        let key = map.keyAt(at);
        Console.write(key.view());
        Console.write("]=");
        Console.write(map.valueAt(at));
        c = map.next(at);
    }
    Console.writeLine("");
    Console.write("text back");
    c = map.last();
    while (c != null) {
        let at = c!;
        Console.write(" [");
        let key = map.keyAt(at);
        Console.write(key.view());
        Console.write("]");
        c = map.previous(at);
    }
    Console.writeLine("");
    textSeek(borrow map, "apple");
    textSeek(borrow map, "apq");
    textSeek(borrow map, "c");
    textSeek(borrow map, "zz");
}

fn byteCases() {
    var map = OrderedMap<Vector<u8>, bool>.create();
BYTE_PUTS
    Console.write("bytes");
    var c = map.first();
    while (c != null) {
        let at = c!;
        let key = map.keyAt(at);
        Console.write(" [");
        for (k in 0..key.length) {
            if (k > 0) {
                Console.write(",");
            }
            Console.write(key[k]);
        }
        Console.write("]");
        c = map.next(at);
    }
    Console.writeLine("");
}

fn intCases() {
    var small = OrderedMap<i8, u8>.create();
    let smallKeys = [SMALL_KEYS];
    for (i in 0..smallKeys.length) {
        let old = small.put(smallKeys[i], 0)!;
    }
    Console.write("i8");
    var c = small.first();
    while (c != null) {
        let at = c!;
        Console.write(" ");
        Console.write(small.keyAt(at));
        c = small.next(at);
    }
    Console.writeLine("");
    var wide = OrderedMap<i64, u8>.create();
    let wideKeys = [WIDE_KEYS];
    for (i in 0..wideKeys.length) {
        let old = wide.put(wideKeys[i], 0)!;
    }
    Console.write("i64");
    var d = wide.first();
    while (d != null) {
        let at = d!;
        Console.write(" ");
        Console.write(wide.keyAt(at));
        d = wide.next(at);
    }
    Console.writeLine("");
    var flags = OrderedMap<bool, u32>.create();
    let t = flags.put(true, 1)!;
    let f = flags.put(false, 2)!;
    let first = flags.first()!;
    let second = flags.next(first)!;
    Console.write("bool ");
    Console.write(flags.keyAt(first));
    Console.write("=");
    Console.write(flags.valueAt(first));
    Console.write(" ");
    Console.write(flags.keyAt(second));
    Console.write("=");
    Console.writeLine(flags.valueAt(second));
}

fn main() {
    u64Cases();
    textCases();
    byteCases();
    intCases();
}
"""


def main():
    lines = []
    u64_cases(lines)
    text_cases(lines)
    byte_cases(lines)
    int_cases(lines)
    puts = []
    for i, k in enumerate(BYTE_KEYS):
        puts.append(f"    var key{i} = Vector<u8>.create();")
        puts += [f"    key{i}.push({b}u8)!;" for b in k]
        puts.append(f"    let old{i} = map.put(key{i}, true)!;")
    source = (PROGRAM.replace("BYTE_PUTS", "\n".join(puts))
              .replace("SMALL_KEYS", ", ".join("-127i8 - 1i8" if k == -128 else f"{k}i8" for k in SMALL))
              .replace("WIDE_KEYS", ", ".join(f"{k}i64" for k in WIDE)))
    source += "".join(f"// expect-stdout: {line}\n" for line in lines)
    path = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "run-pass", "ordered-map.cee")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source)
    ceesafec = os.environ.get("CEESAFEC", os.path.join(ROOT, "out", "ceesafec"))
    subprocess.run([ceesafec, "fmt", path], check=True)
    print(f"gen_ordered_map_cases: wrote {path} ({len(lines)} lines)")


if __name__ == "__main__":
    main()
