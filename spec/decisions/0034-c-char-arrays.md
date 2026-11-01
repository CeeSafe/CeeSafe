# ADR 0034 — C `char` arrays in imported structs

- Status: **Accepted** by the owner, 2026-11-01 ("`[u8; N]` matches `char[N]`").
- Scope: the field types of imported C structs (type-system.md §31.1) and §28.3's `CChar`. Reported by the owner from
  CosmoOS work (`cosmofs_internal.h:199`, `char pool_name[CFS_LABEL_MAX + 1]`).

## 1. Context

An imported C struct's field must be declared with exactly its member's C type, and the generated C checks it with
`_Generic` (§31.1, §31.3). `CChar` is C's `char`, allowed only as a pointee (`*CChar`), because `char`'s signedness
differs between targets (§28.3, N0281). So a member `char name[N]` has no declaration. §31.1 says so: "A plain `char`
array cannot be declared".

Checked 2026-11-01 with ceesafec at `844e755`:
- `name: [CChar; 16]` is N0281;
- `name: [u8; 16]` compiles in CeeSafe, then the C fails: `static assertion failed: ceesafec import: struct
  pool.name is not uint8_t[16]`.

So a C struct with a `char` array member cannot be used from CeeSafe, even when CeeSafe never touches that member.

C itself allows reading and writing any object through an lvalue of character type (C11 6.5p7). `unsigned char`,
and `uint8_t` where it exists, has no padding bits and represents every byte. A `char` array read as bytes is
therefore the same on every target, whatever `char`'s signedness.

## 2. Options

| | A. `[u8; N]` also matches `char[N]` | B. `[CChar; N]` as a field type only |
|---|---|---|
| Declaration | `name: [u8; 16]` | `name: [CChar; 16]` |
| Reading a byte | `p.name[0]`, a `u8` (0–255) | not a value: through a new `.bytes()` view |
| Passing to C as `char *` | `(borrow p.name as *u8) as *CChar` | `borrow p.name as *CChar` |
| New language surface | none: one more accepted C type in the import check | a new array element rule and a view API |
| Matches the C declaration exactly | as bytes (the documented meaning) | yes |

## 3. Proposed decision: A

1. **For an imported struct's member, `[u8; N]` matches `uint8_t[N]`, `char[N]`, `signed char[N]` and
   `unsigned char[N]`.** The import check accepts any of them. Its size assertion is unchanged and still holds, since
   the character types are one byte. (An imported member is never an array of arrays: §31.1.)
2. **A scalar `char` member** is declared `u8`, on the same grounds: C converts the member to `uint8_t` modulo 256
   when CeeSafe reads it, and back when it writes. Its place may be borrowed: the generated C converts its address to
   `uint8_t *`, a character-type access (C11 6.5p7). ceesafec cannot tell a `char` member from a `uint8_t` one (only
   the C compiler sees the header), so it converts the address of every `u8` member of an imported struct.
3. `CChar` stays a pointee only (§28.3). Function prototypes keep matching the C library exactly.
4. Elsewhere, outside imported structs (exports, `@repr(C)` structs CeeSafe defines, prototypes), `char` arrays
   still do not cross: a CeeSafe-defined struct says `uint8_t` in its C.

A because the bytes are the meaning C guarantees, and it adds nothing to learn. B would give `CChar` values that
cannot be read and need their own API, for the same bytes.

## 4. Consequences

- type-system.md §31.1: the sentence "A plain `char` array cannot be declared" is replaced by rules 1 and 2.
- The C backend (`src/backend/c_skeleton.cee`, the import assertions): `_Generic` gains the character-type cases for
  a `[u8; N]` or `u8` field. The emitter already reads array members through a cast to `ceeA<N>_u8 *`, which is a
  character-type access.
- Tests:
  - run-pass: a header struct with `char name[16]` and a `char tag` member, read, written and passed to a C function
    as `char *`, on every host (`--cc cl` too);
  - the existing `[u8; N]`-is-`uint8_t[N]` import test unchanged.

## 6. Implementation (2026-11-01)

- The import assertions (`src/backend/c_skeleton.cee`) accept the character types for a `[u8; N]` or `u8` field in
  a nested `_Generic`: `uint8_t` is usually `unsigned char`, and one `_Generic` may not name compatible types twice.
- A borrow of a `u8` member of an imported struct is `(uint8_t *)(uintptr_t)&…` (`isImportedByteMember`,
  `src/backend/c_body.cee`). Amended from the accepted text, which made such a borrow N0294: ceesafec cannot know the
  member's C type, so that rule would have forbidden borrowing `uint8_t` members too, which compiled before.
- Test: `ffi/c-chars.cee` (`char name[16]`, `char tag`, `signed char level[4]`, `unsigned char key[2]`; read,
  written in place, borrowed, and the name passed to C's `strlen` as `*CChar`), with `-Werror` and under ASan+UBSan.

## 5. Question for the owner

1. A, `[u8; N]` for `char[N]` members? **Yes.**
