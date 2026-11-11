#!/usr/bin/env python3
"""An independent model of tests/reference/tests/corpus/run-pass/loop-versioning.cee.

The test's expected output is this script's output. It is written from the test's meaning (unbounded integers,
wrapped to 64 bits where the test says wrapping), not from anything ceesafec produced.

    python3 tools/oracles/loop_versioning_model.py
"""
M = 1 << 64


def fill(n):
    return [i * 3 + 1 for i in range(n)]


def plain(v, n):
    return sum(v[j] for j in range(n)) % M


def based(v, a, b, n):
    return sum(v[a * b + j] for j in range(n)) % M


def shifted(v, lo, n):
    s = 0
    for j in range(n):
        v[j + lo] = v[lo + j + 1] + 1
        s = (s + v[j + lo]) % M
    return s


def fixed(v, k, n):
    return sum(v[k] + j for j in range(n)) % M


def broken(v, stop, n):
    s = 0
    for j in range(n):
        if j == stop:
            break
        s = (s + v[j]) % M
    return s


def slices(a, b, n):
    s = 0
    for j in range(n):
        if a[j] % 2 == 1:
            b[j] = a[j] + b[j]
        s = (s + b[j]) % M
    return s


def matrix(n, length):
    c, b = fill(length), fill(length)
    for i in range(n):
        for k in range(n):
            x = b[i * n + k]
            for j in range(n):
                c[i * n + j] = (c[i * n + j] + x * b[k * n + j]) % M
    s = 0
    for value in c:
        s = (s * 31 + value) % M
    return s


def array(n):
    a = [0] * 8
    for j in range(n):
        a[j] = j + 5
    s = 0
    for value in a:
        s = s * 3 + value
    assert s < M
    return s


def main():
    n, none, big = 16, 0, 1 << 32
    v, w = fill(n), fill(n)
    print(plain(v, n))
    print(plain(v, none))
    print(based(v, 2, 3, 5))
    print(based(v, big, big, none))
    print(shifted(v, none, n - 1))
    print(fixed(v, 5, 4))
    print(fixed(v, 99, none))
    print(broken(v, 3, 100))
    print(slices(w, v, n))
    print(matrix(4, n))
    print(array(8))


if __name__ == "__main__":
    main()
