# ADR 0041 — C interop for porting a stateful subsystem: shared atomics, imported unions, variadic calls

- Status: **Accepted** by the owner, 2026-11-01: every question of §5 as recommended (A; members restricted to
  any-bit-pattern types; variadic calls unsafe, with a safe formatter and log line in the libraries; ceesafec 0.4.0).
  Asked for by the owner ("Start with the ADR") after a survey of CosmoOS's network stack, which roadmap 2.0 SA6
  converts to CeeSafe.
- Scope: three gaps in C interoperability (type-system.md §28–§33) that stop CeeSafe code from taking over C state
  which C code still shares: integers C reads and writes atomically, unions, and C's variadic functions.

## 1. Context

CosmoOS's network stack (`kernel-services/network/`, 18 files, about 11,900 lines of C besides its self-tests) is
stateful code whose structs the rest of the kernel and its drivers share. The checksum, its pure first module, is
ported. A survey of the rest (2026-11-01, CosmoOS `ed33a047`) found what CeeSafe's interop cannot yet express
safely; everything else it uses is covered (C function pointers §29.5, imported and exported structs §31 and ADRs
0029–0030, statics §39, the kernel library's locks and objects, and floors for C-only state).

**Shared atomics.** About 290 operations: `__atomic_load_n` 121, `__atomic_store_n` 99, `__atomic_fetch_add` 48,
`__atomic_compare_exchange_n` 11, `__atomic_fetch_sub` 5, `__atomic_exchange_n` 4, `__atomic_fetch_or` 2,
`__atomic_fetch_and` 1, on plain `uint32_t`/`uint64_t` members (interface statistics, reference counts, flags, the
TCP connection's state) that C code on other CPUs also reads and writes atomically. CeeSafe's atomics (§38.3, ADR
0026) are its own types, `AtomicU32`/`AtomicU64`/`AtomicUSize`/`AtomicBool`; an imported field must be declared
with exactly its C type (§31.1), so a CeeSafe port could only reach such a member non-atomically, which races with C.
`AtomicU64` lowers to a struct whose one member is a `uint64_t` at offset 0, with its size and alignment (checked by
`_Static_assert` in every unit), so it is already laid out as the integer.

**Unions.** `struct netaddr` (`kernel/net/inet.h`), the address every socket, protocol control block, firewall rule
and NAT entry holds, keeps its IPv4 and IPv6 addresses in an anonymous union:
`struct netaddr { uint16_t family; uint16_t port; union { uint32_t v4; struct in6_addr v6; }; }`. CeeSafe imports no
union. Declaring `v4` and `v6` as two fields would let safe code borrow both at once while they share storage.

**Variadic calls.** 41 calls in 9 files: the kernel log (`klog`, through `kinfo`/`kwarn`/`kerror`), `ksnprintf`,
`panic` and `kprintf`, all `__printf`-checked variadic functions. CeeSafe cannot declare or call a C variadic
function, so ports so far moved each message into a small C floor (`wait_floor_fatal(const char *what)`), which adds
C for every message, and SA6 would add dozens.

Not in scope: `goto` (19 in `tcp.c`, 3 in `tap.c`) is rewritten as structured control flow; an anonymous struct
member (`netif.h`) is named on CosmoOS's side, as `struct mbuf_pkt` was; why clang vectorises the C checksum's pair
loop with `ld2` on arm64 and not the C ceesafec emits for one is a separate investigation (the checksum was accepted
at 1.06–1.09x on arm64).

## 2. Options

| | A. CeeSafe expresses the C (proposed) | B. CosmoOS changes its C | C. Floors |
|---|---|---|---|
| Atomics | an imported or exported field may have an atomic type whose C member is the integer | wrap each shared member in a CeeSafe-owned struct | a C accessor per member |
| Unions | imported union members, one place for the borrow checker | split `netaddr`'s union into two fields (+4 bytes each, and every `memcmp`/hash over it changes) | — |
| Variadics | declare and call variadic C functions (unsafe), and a safe, allocation-free formatter in the libraries on top | a non-variadic log entry, `klog_write(level, text, len)`, in CosmoOS | one C wrapper per message |
| Cost | three interop features, specified and tested in CeeSafe | changes to shared kernel headers and C written for the port | C grows with every port: against the CeeSafe rule's point 1 |

**A is proposed.** The C is the contract other code relies on; CeeSafe should be able to state it exactly, as §31
already does for structs. B changes shared kernel code for the convenience of a port, and C adds the C the roadmap
exists to remove.

## 3. Proposed decision

### 3.1 Atomic fields shared with C

1. In an `extern "C"` struct (§31.1), a field may be declared `AtomicU32`, `AtomicU64` or `AtomicUSize` when its C
   member's type is `uint32_t`, `uint64_t` or `size_t`. (Implementation note: `AtomicBool` is excluded. It is laid
   out as a `u32` and C's `bool` is one byte, so it cannot be a `bool` member; the survey found no shared `bool`.) The `_Generic` member check compares with
   that integer type. The field is a place of the atomic type: safe code reaches it only through the type's methods,
   which lower to `__atomic_*` on the member's address, the same operations C's code uses on it.
2. In a `@repr(C)` struct that a C header shares (§28.3, ADR 0029's exports header), a field of an atomic type is
   written as the integer type in the header, so C code goes on using `__atomic_*` on it. The atomic types are
   specified as laid out exactly as their integer, which the existing layout assertions already check.
3. The atomic types gain `fetchSub`, `fetchOr` and `fetchAnd` (with `…With(order)` forms, as `add` and `swap`
   have), which CosmoOS's code uses; `fetchAdd`, `swap` and `compareExchange` exist.
4. Soundness: CeeSafe accesses such a member only atomically. That C does too is the C side's obligation, as for any
   state C shares (§31); a non-atomic C access would be a data race in C already.

### 3.2 Unions in imported structs

1. Inside an `extern "C"` struct, `union { a: T; b: U; }` declares the members of a C11 anonymous union member, reached
   as `r.a` and `r.b` (C's `p->a`). `union name { … }` declares an imported union type, used as a struct type is
   (by pointer, by reference, embedded as a field), whose members are reached the same way.
2. The members share storage, so the borrow checker treats every member of one union as **one place**: borrowing or
   assigning any member conflicts with every other member while the loan lives (B1–B2).
3. A member's type must be one for which every bit pattern is a value: integers, raw pointers, `CChar`, arrays of
   these, and imported types whose declared fields obey the same rule. `bool`, enums, references, slices, closures and
   CeeSafe structs are refused with a new diagnostic, so reading one member after writing another reinterprets bytes,
   as C11 allows, and can never produce an invalid value.
4. Lowering: an anonymous member's member is `p->a`; a union type's is `u.a`. Each declared member gets the same
   `_Generic` check as a struct field.

### 3.3 Variadic C functions, and formatting without allocation

1. `fn klog(level: u32, fmt: *CChar, ...);` in an `extern "C"` block declares a variadic C function. A call passes
   its fixed parameters as usual and any number of further arguments of the types C's default argument promotions
   leave unchanged: `i32`, `u32`, `i64`, `u64`, `usize`, `isize`, `f64`, raw pointers and `*CChar`. A narrower
   integer is refused (with the fix: widen it with `as`), as is anything else.
2. A call to a variadic function is **unsafe**: whether the arguments agree with a format string is not something
   CeeSafe checks (C's `__printf` checks only a literal format, and the generated C passes a pointer).
3. Safe formatting on top, with no allocation, for every profile: `lib/core` gains a writer over a caller's
   `SliceMut<u8>` that appends text, decimal and hexadecimal integers and a byte, truncating as `snprintf` does and
   keeping the result zero-terminated, and reports the length it wanted, as `snprintf`'s result does. Network code
   formats addresses with it (`netaddr_str`, `procnet`).
4. `lib/kernel` gains a log line built with that writer in a fixed buffer of `KLOG_LINE_MAX` (256) bytes and sent with
   one call, `klog(level, "%s", line)`: the only variadic call, inside the library, with a fixed format, so a port's
   messages are safe code. A `panic` built the same way ends in `panic("%s", line)`.

## 4. Consequences

- A port can take over a C struct whose members C also changes atomically, and `struct netaddr`, without a floor.
  Every message a port logs is safe CeeSafe; the one variadic call is in `lib/kernel`.
- Imported unions are restricted to members of any-bit-pattern types; a union holding a `bool` or a pointer to a
  CeeSafe struct stays opaque or behind a floor.
- New diagnostics: a union member of a type with invalid bit patterns, a variadic argument of a type C would promote
  or cannot pass, and an atomic field whose C member is not the matching integer.
- CosmoOS can use these after a ceesafec release that has them and a pin bump; SA6's ports wait for it.

## 5. Questions for the owner (answered 2026-11-01: each as recommended)

1. A (CeeSafe expresses the C) rather than B (change CosmoOS's headers) or C (floors)? Recommended: A.
2. Union members restricted to any-bit-pattern types (§3.2.3)? Recommended: yes; it is what makes reading a union safe.
3. Variadic calls unsafe, with a safe formatter and log line in the libraries (§3.3)? Recommended: yes. The
   alternative, a CosmoOS `klog_write(level, text, len)`, would need no language change but adds C to CosmoOS.
4. The release: these as ceesafec 0.4.0 (main is 0.4.0-dev), with CosmoOS's pin moved in its own commit? Recommended:
   yes.

## 6. Implementation plan (once accepted)

Spec first: §31.1 and §31.2 (atomic fields; union members and union types), §28 (variadic declarations and calls),
§38.3 (the atomic types' layout and new operations), lib/core's and lib/kernel's API pages, the diagnostics table.
Then, each with corpus tests (check-fail for every new diagnostic, run-pass and emit-c for the lowering, ffi for a C
harness that shares an atomic field, a union and a variadic function with CeeSafe), under ASan, UBSan and TSan:

1. Atomic fields in imported and exported structs; `fetchSub`, `fetchOr`, `fetchAnd`.
2. Imported unions, the one-place borrow rule and the member-type restriction.
3. Variadic declarations and calls.
4. The formatter in `lib/core`, the log line and `panic` in `lib/kernel`, with a `--kernel cosmoos` test (K3) and a
   CosmoOS self-test that logs from CeeSafe.

Then the contract suite, a release, and CosmoOS's pin.

## 7. Implementation notes (2026-11-01)

- **Atomic fields.** `AtomicBool` is excluded (§3.1.1). In `ceesafe_exports.h` an `AtomicUSize` member is written
  `uint64_t`, as a `usize` member of a `@repr(C)` struct is (§28.5); in an imported struct it is checked against
  `size_t`.
- **Unions.** `union` is a word only in an extern block, before a name or, inside an imported struct, before `{`;
  elsewhere it stays an identifier. Every member of one union shares a union group in the type table, which the borrow
  checker's overlap test compares (N0240–N0242). Member types: N0346. An empty union, or a union inside an anonymous
  union: N0107.
- **Variadic calls.** `...` is lexed as `..` and `.` with nothing between, so no token changes. A variadic function
  has no wrapper: each call converts its arguments in place, so it has no function pointer (N0291) and cannot return
  a `@repr` enum (N0291). A unit that calls one disables `-Wformat-security` and `-Wformat-nonliteral`: the format is a
  pointer, and agreeing with it is the caller's `unsafe` obligation. Further arguments: N0347.
