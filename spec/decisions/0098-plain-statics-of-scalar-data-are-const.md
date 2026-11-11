# ADR 0098 — A plain `static` of scalar data is C's `const` object

- Status: **Accepted** by the owner, 2026-11-11 ("Go ahead with CeeSafe fix"), and implemented on branch `impl/0098`,
  on 0.36.0. Raised by CosmoOS's roadmap 1.3 (K1): the list of system calls a filter may never deny is C's `static
  const uint16_t native_always_allowed[]`, in read-only data, and written in CeeSafe it was emitted writable.
- Scope: the C emitter's statics (`src/backend/c_skeleton.cee`); `spec/type-system.md` §39.2 and §39.6. No change to
  what the checker accepts, to the runtime or to the library.

## 1. Context

ADR 0091 made a plain `static` of a C-object type (an imported C struct, a raw pointer, a C function pointer, an
array of these) C's `const` object. ADR 0080 did the same for an exported static with no atomic. Every other plain
static was emitted without `const`:

```ceesafe
static ALLOWED: [u16; 4] = [0, 68, 86, 87];
```

was `static ceeA4_u16 ceeS_ALLOWED = …;`, in writable data, though no CeeSafe code can write it: a plain static is
only read (§39.2), and writing one through a raw pointer made from its address is undefined behaviour already. For a
kernel the difference is real: C's `static const` table is mapped read-only, so a stray or hostile write faults; the
CeeSafe port of the same table lost that.

## 2. Decision

A plain `static` (not `static mut`) whose type is **scalar data** is emitted `const`: an integer, a float, a `bool`,
a `CChar`, or an array of these, to any depth. Exported or not; `static const` when not exported.

Nothing else changes. Its address is taken through `uintptr_t` where CeeSafe's pointer type is needed, as for the
`const` objects of ADRs 0080 and 0091 (§39.6): CeeSafe's pointer types carry no `const`.

Not included, on purpose: plain statics of CeeSafe structs and enums, and of the library's types. Some of those
change through a shared reference (an atomic, a `Mutex`, a `RawCell`), and telling which do not is a rule about
every member of every type; this ADR takes the types where there is nothing to decide.

## 3. Compatibility

- Source: none. No program is accepted or refused that was not before.
- Generated C: the definition gains `const`, and uses of the address gain the cast. A split build's declaration is
  `extern const`.
- A program that wrote a plain static through a raw pointer (undefined behaviour before, working by accident) now
  faults on systems that map read-only data read-only.

## 4. Tests

- Corpus `run-pass/static-scalar-data.cee` (new): an integer, a `bool`, an array and an array of arrays, read by
  value, by index, through a borrow, as a slice and through a raw pointer, beside a `static mut`; compiled and
  interpreted.
- `tools/const_statics_check.py` (new, a contract): that file's C defines each plain static `const` and the `static
  mut` not, and compiles with `-Werror=cast-qual`. With ceesafec 0.36.0 it fails on all four plain statics.

## 5. Found, not fixed

A `static` of a float type does not build, before this change and after it: `static RATIO: f64 = 1.5;` is emitted
with the initialiser `cee_f64_from_bits(UINT64_C(…))`, a function call, which C refuses in a static initialiser
("initializer element is not a compile-time constant", clang, ceesafec 0.36.0). The rule above names floats, but the
test has none for that reason.
