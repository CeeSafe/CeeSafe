#!/usr/bin/env python3
"""A model of AES-256-GCM for the tests of lib/std's Aes256Gcm (ADR 0135), written from the definitions and sharing
nothing with the code under test: the S-box from the field inverse and the affine map, the cipher byte by byte, and
GHASH as integer arithmetic on 128-bit numbers.

self_check() runs the four AES-256 test cases of the GCM specification (McGrew and Viega, test cases 13 to 16) and,
when the `cryptography` package can be imported, 300 random cases against it (OpenSSL).
"""

import os


def _gmul(a, b):
    r = 0
    for _ in range(8):
        if b & 1:
            r ^= a
        high = a & 0x80
        a = (a << 1) & 0xFF
        if high:
            a ^= 0x1B
        b >>= 1
    return r


def _sbox_of(x):
    inverse = 0
    if x:
        inverse = 1
        for _ in range(254):
            inverse = _gmul(inverse, x)
    s = 0
    for i in range(8):
        bit = (inverse >> i) ^ (inverse >> ((i + 4) % 8)) ^ (inverse >> ((i + 5) % 8))
        bit ^= (inverse >> ((i + 6) % 8)) ^ (inverse >> ((i + 7) % 8)) ^ (0x63 >> i)
        s |= (bit & 1) << i
    return s


SBOX = [_sbox_of(x) for x in range(256)]


def expand_key(key):
    assert len(key) == 32
    words = [list(key[4 * i:4 * i + 4]) for i in range(8)]
    rcon = 1
    for i in range(8, 60):
        t = list(words[i - 1])
        if i % 8 == 0:
            t = [SBOX[t[1]] ^ rcon, SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
            rcon = _gmul(rcon, 2)
        elif i % 8 == 4:
            t = [SBOX[b] for b in t]
        words.append([words[i - 8][j] ^ t[j] for j in range(4)])
    return [sum(words[4 * r:4 * r + 4], []) for r in range(15)]


def encrypt_block(round_keys, block):
    s = [block[i] ^ round_keys[0][i] for i in range(16)]
    for r in range(1, 15):
        s = [SBOX[b] for b in s]
        s = [s[((i // 4 + i % 4) % 4) * 4 + i % 4] for i in range(16)]
        if r < 14:
            mixed = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                mixed += [
                    _gmul(a[0], 2) ^ _gmul(a[1], 3) ^ a[2] ^ a[3],
                    a[0] ^ _gmul(a[1], 2) ^ _gmul(a[2], 3) ^ a[3],
                    a[0] ^ a[1] ^ _gmul(a[2], 2) ^ _gmul(a[3], 3),
                    _gmul(a[0], 3) ^ a[1] ^ a[2] ^ _gmul(a[3], 2),
                ]
            s = mixed
        s = [s[i] ^ round_keys[r][i] for i in range(16)]
    return bytes(s)


def _ghash_mul(x, y):
    """The product of two elements of GF(2^128) as GCM numbers them: bit 0 is the most significant."""
    z = 0
    v = y
    for i in range(128):
        if (x >> (127 - i)) & 1:
            z ^= v
        v = (v >> 1) ^ (0xE1 << 120 if v & 1 else 0)
    return z


def _ghash(h, aad, text):
    def blocks(data):
        for i in range(0, len(data), 16):
            yield int.from_bytes(data[i:i + 16].ljust(16, b"\0"), "big")
    y = 0
    for block in list(blocks(aad)) + list(blocks(text)):
        y = _ghash_mul(y ^ block, h)
    return _ghash_mul(y ^ ((len(aad) * 8) << 64 | (len(text) * 8)), h)


def seal(key, nonce, aad, plaintext):
    """The ciphertext and the 16-byte tag."""
    assert len(nonce) == 12
    rk = expand_key(key)
    h = int.from_bytes(encrypt_block(rk, bytes(16)), "big")
    out = bytearray()
    for i in range(0, len(plaintext), 16):
        stream = encrypt_block(rk, nonce + (2 + i // 16).to_bytes(4, "big"))
        out += bytes(a ^ b for a, b in zip(plaintext[i:i + 16], stream))
    mask = int.from_bytes(encrypt_block(rk, nonce + (1).to_bytes(4, "big")), "big")
    return bytes(out), (_ghash(h, aad, bytes(out)) ^ mask).to_bytes(16, "big")


def self_check():
    assert SBOX[0x00] == 0x63 and SBOX[0x53] == 0xED and len(set(SBOX)) == 256
    zero_key = bytes(32)
    key = bytes.fromhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308")
    nonce = bytes.fromhex("cafebabefacedbaddecaf888")
    text = bytes.fromhex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255")
    cipher = bytes.fromhex(
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad")
    aad = bytes.fromhex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
    cases = [
        (zero_key, bytes(12), b"", b"", b"", "530f8afbc74536b9a963b4f1c4cb738b"),
        (zero_key, bytes(12), b"", bytes(16), bytes.fromhex("cea7403d4d606b6e074ec5d3baf39d18"),
         "d0d1c8a799996bf0265b98b5d48ab919"),
        (key, nonce, b"", text, cipher, "b094dac5d93471bdec1a502270e3cc6c"),
        (key, nonce, aad, text[:60], cipher[:60], "76fc6ece0f4e1768cddf8853bb2d551b"),
    ]
    for k, n, a, p, c, t in cases:
        got_c, got_t = seal(k, n, a, p)
        assert got_c == c and got_t.hex() == t, "a test case of the GCM specification"
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        return "4 specification cases (the cryptography package is not installed)"
    import random
    rng = random.Random(135)
    for _ in range(300):
        k, n = os.urandom(32), os.urandom(12)
        a, p = os.urandom(rng.randrange(0, 70)), os.urandom(rng.randrange(0, 300))
        c, t = seal(k, n, a, p)
        assert AESGCM(k).encrypt(n, p, a) == c + t, "a random case against the cryptography package"
    return "4 specification cases and 300 random cases against the cryptography package"


if __name__ == "__main__":
    print("aes_gcm_model:", self_check())
