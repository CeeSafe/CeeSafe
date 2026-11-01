# ADR 0042 — Statics that are C state: imported structs, ops tables and function addresses

- Status: **Accepted** by the owner, 2026-11-01 ("I accept ADR 0042 as proposed"): every question of §5 as
  recommended. Found porting CosmoOS's `loopback.c` (roadmap 2.0 SA6),
  after ADR 0041.
- Scope: static items (type-system.md §39) whose type is an imported C type, and the static initialisers they need:
  a struct literal of an imported type, a C function's address, another static's address, a null C pointer.

## 1. Context

Kernel C defines objects that C code then owns, registers and mutates: a driver's or protocol's **ops table**
(`static const struct netif_ops lo_ops = { .transmit = lo_transmit, .release = netif_release_static };`) and the
**object** that points to it (`static struct netif g_lo = { .name = "lo", .mtu = 65535, .flags = …, .ops = &lo_ops };`),
which an init function finishes and registers (`netif_register(&g_lo)`). CosmoOS has 64 ops tables in 26 files
(drivers, block, the network stack); every SA6 and SA9 port that defines a device or a protocol meets this.

ceesafec 0.4.1 refuses each part of it (checked 2026-11-01 on a reduced copy of `loopback.c`):

| What C writes | CeeSafe today |
|---|---|
| a static of an imported struct, initialised by a literal | N0294: CeeSafe cannot make an imported type, only point to one (§31.2); only a C constant macro can initialise one (§39.4) |
| that static at all | N0337: an imported struct is not `Share`, and a `static` must be (§39.2) |
| `.release = netif_release_static`, an extern function's address | N0298 in a static initialiser, though §29.5 makes it a value of an `extern "C" fn` field elsewhere |
| `.ops = &lo_ops`, another static's address | N0338: not a static initialiser (§39.4) |
| `static lo_filter_fn g_filter;`, a null C function pointer set later | `0 as extern "C" fn(…)` is not a static initialiser |

The constant-macro route (§39.4) would move each initialiser into a C header macro, and the ops table with it: C that
the conversion exists to remove.

## 2. Options

| | A. Static initialisers for C objects (proposed) | B. C floors | C. Allocate at init |
|---|---|---|---|
| What changes | §39.4 accepts a literal of an imported struct, function and static addresses, and null; such a static is `static mut` | the object and its ops table stay in a `*_floor.c`, CeeSafe implements the functions | CosmoOS gains allocation APIs (`netif_alloc`), and ports allocate their object |
| C left | none | a floor per device or protocol: 26 files' worth | none, but C APIs change for the ports' sake |
| Cost | the checker and the static-initialiser lowering | grows with every port | changes shared kernel interfaces; static objects (loopback, boot devices) become heap objects |

**A is proposed**, as for ADR 0041: CeeSafe should state the C exactly.

## 3. Proposed decision

1. **A `static mut` may have an imported C type** (§31), or an array of one. It is C state from the start: C code
   reaches it through the pointers CeeSafe gives it (`netif_register(borrow mut LO as *mut netif)`), so CeeSafe
   cannot assume it unchanged, and every use is unsafe, as §39.3 already makes every use of a `static mut`. A plain
   `static` of an imported type stays N0337: CeeSafe would read it as immutable while C writes it.
2. **A struct literal of an imported type is a static initialiser** in such a static, or in a field of one: it names
   any of the declared fields, each with a static initialiser, and every C member it does not name is zero, as C's
   designated initialisers leave it (C11 §6.7.9p21). It lowers to a C designated initialiser over the members' C names
   (`@cname`). Outside a static initialiser it stays N0294: no value of an imported type exists at run time.
3. **More static initialisers** (§39.4), where an `extern "C" fn` or raw pointer type is expected:
   - the name of an `extern "C"`, `@export` or `@callback` function: its address (§29.5's values, now in initialisers
     too);
   - `borrow S as *T` / `borrow S as *mut T` and the same of a field or element of `S`, for a static `S`: its address,
     an address constant (C11 §6.6p9); a `static mut`'s address is not an unsafe use, only what is done through it;
   - `0 as extern "C" fn(…) -> R` and `0 as *T`: null.
4. A `[u8; N]` field (a C `char` array, ADR 0034) takes a byte string literal's bytes in an initialiser, zero-padded
   to `N` (`name: "lo"`), so names read as they do in C. A literal longer than `N` is N0338.
5. **Lowering**: the static's C object is named by its C spelling's type and the static's own C name; ops tables are
   not `const` in C (§39.5), which C accepts wherever `const struct …_ops *` is expected.

## 4. Consequences

- Loopback becomes: `static mut LO_OPS: netif_ops = netif_ops { transmit: transmit, release: netif_release_static }`,
  `static mut LO: netif = netif { name: "lo", mtu: 65535, flags: …, caps: …, ops: borrow LO_OPS as *netif_ops }`, the
  transmit function as `@callback`, and `loopback_init` writing the addresses before `netif_register`.
- Drivers (SA9) and the block layer (SA7) define their ops tables and static devices the same way, with no floor.
- An ops table that C only reads is a `static mut` though it is never written: CeeSafe has no `const` C object in v1.
  A later decision may add one; nothing here prevents it.
- New diagnostics: none; N0294, N0337 and N0338 narrow.

## 5. Questions for the owner

1. A (static initialisers for C objects) rather than B (floors) or C (allocation APIs)? Recommended: A.
2. Imported-type statics only as `static mut` (every use unsafe), rather than a `Share` rule for C state? Recommended:
   yes: C writes them, so CeeSafe's shared-reference reading of a `static` would be wrong.
3. Byte string literals for `[u8; N]` name fields (§3.4)? Recommended: yes; otherwise `name: [108, 111, 0, …]`.
4. Release as ceesafec 0.5.0, then CosmoOS's pin in its own commit, as for 0.4.1? Recommended: yes.

## 6. Implementation plan (once accepted)

Spec first (§39.3–§39.5, §31.2, §29.5), then the checker and the static-initialiser lowering, with corpus tests: an ffi
test where C calls through a CeeSafe ops table and registers a CeeSafe static object; check-fail for a plain `static`
of an imported type, a literal outside a static, an oversized name; emit-c for the designated initialisers. Then the
contract suite, K3 against CosmoOS, a release, CosmoOS's pin, and the loopback port.

## 7. Implementation notes (2026-11-01)

- An imported type's literal is accepted while a static initialiser is checked, so a plain `static` of an imported
  type gets N0337 for its type (with a hint to declare it `static mut`), not N0294 for its literal.
- In an initialiser a static's name is its place, `*StaticAddress`: StaticFold accepts the place only under a borrow
  cast to a raw pointer (`IsStaticAddress`), and reports its value as "another static's value" (N0338).
- `emittedCallbacks` reads the statics' initialisers too, so a `@callback` named only in an ops table gets its C
  function (`ceeW_…`) and its prototype before the statics. Every static is declared by a tentative definition first
  when one initialiser holds an address, in one unit or `--split`'s `ceesafe_instances.c`.
- A string for a `[u8; N]` member is checked against `N` when the literal is checked (N0338), and lowers to a C string
  literal, which C zero-pads.
