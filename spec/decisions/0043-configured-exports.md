# ADR 0043 — Exports that exist only in some builds, and C function pointers from addresses

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept your recommendations"): every question of §5 as
  recommended. Found porting CosmoOS's `netif.c` (roadmap 2.0 SA6),
  after ADRs 0041 and 0042.
- Scope: `@export` functions whose C symbol a build's configuration includes or leaves out; and the conversion of an
  address back to a C function pointer, which the compiler already accepts and the specification does not state.

## 1. Context

Kernel C compiles parts of a file only in some builds. `netif.c` has two kinds:

- **Exported functions that exist in some builds only.** `netif_tx_probe_set` and `netif_tx_probe_read` are defined
  `#if CONFIG_SELFTEST`; in other builds `netif.h` defines them as `static inline` stubs, so a definition of the same
  name there would not compile. `netif_test_worker_hooks_set` is defined, and exported to modules, only
  `#if CONFIG_DEBUG`; `netif.h` declares it only then.
- **Code inside a function that exists in some builds only**: the self-test's timing of a transmit, the debug hooks'
  calls. These need no feature: a C constant macro read in a static initialiser (`COSMO_CONFIG_DEBUG()`, as CosmoOS's
  `mbuf.cee` has it since 2026-11-02) gives a `static` that the C compiler folds, so `if (SELFTEST) { … }` compiles away
  where the C's `#if` did.

CeeSafe has no conditional items: every function is compiled in every build, and an exported one always defines its C
symbol. So a port of `netif.c` cannot define `netif_tx_probe_set` in a release build, where `netif.h` already does.

Separately, the hooks (`g_rx_hook`, the test hooks) are C function pointers published with atomic stores. CeeSafe keeps
them in an `AtomicUSize` (`f as usize`, §29.5) and needs the address back as a function pointer. `ceesafec` accepts
`n as extern "C" fn(…) -> R` for any `n: usize` and lowers it to a cast (checked 2026-11-02), as `n as *T` is for raw
pointers (§27.2); the specification states only `0 as extern "C" fn(…)`, the null pointer.

## 2. Options

| | A. `@cfg` on exports (proposed) | B. Conditional items in general | C. Floors |
|---|---|---|---|
| What | an `@export` function's C symbol is emitted inside `#if NAME`; the CeeSafe function exists in every build | any item may be conditional, as Rust's `cfg` | the configured exports stay C, calling CeeSafe |
| Checking | every item is checked in every build | an item is checked only in builds that have it, or the checker evaluates conditions | as A |
| Cost | the attribute and the lowering | the checker must know the build's configuration, or check every combination | C grows with each port that has a configured export |

**A is proposed.** It is what C interop needs, the configured symbols, while CeeSafe code stays one program checked
once. Code inside functions already has the constant-macro route, which also checks both branches.

## 3. Proposed decision

1. **`@cfg("NAME")`** on an `@export` function: its C definition (the exported function, its prototype, and its line
   in `ceesafe_exports.h`) is emitted inside `#if NAME` … `#endif`, so the symbol exists in the builds where the C
   macro `NAME` is non-zero, exactly where the C's `#if NAME` had it. `NAME` must be a valid C identifier (N0292). The
   CeeSafe function itself is compiled and checked in every build, and CeeSafe code may call it in any build.
2. `@cfg` anywhere else is N0290: a non-exported function, a static or a type has no C symbol to leave out.
3. **The run-time switch** stays the C constant macro (§39.4): the guide and §39.4 say how, with
   `static SELFTEST: bool = COSMO_CONFIG_SELFTEST();`, which the C compiler folds.
4. **C function pointers from addresses**: `n as extern "C" fn(…) -> R` for `n: usize` is a C function pointer to
   that address, as `n as *T` is a raw pointer (§27.2): making it is safe, calling through it is unsafe (§29.5), and
   a wrong address is the caller's `unsafe` obligation at the call. §29.5 states it.

## 4. Consequences

- `netif.c` ports with no floor beyond its module exports (which stay C, `#if CONFIG_DEBUG` included).
- A program's exports header differs between builds only by the `#if` lines, which C evaluates.
- New diagnostics: none; N0290 and N0292 cover a misplaced `@cfg` and a bad name.

## 5. Questions for the owner

1. A (`@cfg` on exports only) rather than B (conditional items in general) or C (floors)? Recommended: A.
2. State the existing `n as extern "C" fn` conversion in the specification as it behaves? Recommended: yes.
3. Release as ceesafec 0.6.0, then CosmoOS's pin in its own commit? Recommended: yes.

## 6. Implementation plan (once accepted)

Spec first (§28.1's attribute table, §28.4, §27.2 and §29.5), then the attribute's checking and the `#if` lowering of
the export and of its exports-header line, with corpus tests: an ffi test whose C harness is built twice, with the
macro set and unset; check-fail for `@cfg` elsewhere and a bad name. Then the contract suite, K3, a release, CosmoOS's
pin, and the netif port.

## 7. Implementation notes (2026-11-02)

- The HIR function carries the macro's name (`cfgName`); the backend wraps the export's prototype, its definition (in
  one unit or `--split`'s instances), and its exports-header line in `#if NAME` / `#endif`. Nothing else changes: the
  CeeSafe function, its checks and its callers are the same in every build.
- `@cfg` on a struct or other item is N0290 through the attribute table, as any attribute in the wrong place; on a
  function without `@export`, N0290 with a message saying so; a name that is not a C identifier, N0292.
- `n as extern "C" fn(…)` needed no change: §29.5 now states what ceesafec already did.
