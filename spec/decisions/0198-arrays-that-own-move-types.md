# ADR 0198 — Arrays whose elements are not Copy: atomics first, then any type

- Status: **Accepted** by the owner, 2026-11-21 ("Accept all as recommended": every question as §5 or §6 recommends; release order: a patch first, then the next minor). Proposed the same day. Raised by CosmoStore's G41 (`CeeSafeApps/cosmostore/CEESAFE_GAPS.md`): a master
  key's words were to live in `[AtomicU64; 4]`, which N0259 refuses outside a static. It finishes what ADR 0075 §2.15
  left out ("Arrays of non-Copy elements outside statics", its §3). Phase 1 implemented for 0.86.0.
- Scope: type-system.md §14.1, §14.5, §16.1 to §16.5, §21.3, §39.4; diagnostics.md N0259 and N0251; the checker
  (`src/check/arrays.cee`, `src/types/types.cee`), NIR (one new rvalue), the C backend's drop glue
  (`src/backend/c_helpers.cee`) and the interpreter's (`src/interpreter/runtime.cee`); the guide's arrays page and
  book ch08. No new syntax, no runtime change, no change to a program that compiles today.

## 1. Context

- **What fails.** On 0.84.1 (`~/.ceesafe/bin/ceesafec check`, 2026-11-21):
  - `struct K { cells: [AtomicU64; 4]; }` and `let a: [AtomicU64; 2] = [AtomicU64.create(1), AtomicU64.create(2)];`
    are N0259 "array and slice elements must be Copy types in v0; 'AtomicU64' is not Copy". So is a struct of two
    atomics as element (`[P; 2]`), and `fn f<T>(a: [T; 2])` for an unbounded `T`.
  - `static SLOTS: [AtomicU64; 4] = [AtomicU64.constant(0); 4];`, `SLOTS[1].fetchAdd(1)` and
    `let s: Slice<AtomicU64> = SLOTS[..];` are accepted (ADR 0075 §2.15), and so is `sum(v.view())` over a
    `Vector<AtomicU64>`: slices of move types exist since M6b (§21.3).
- **The workaround.** CosmoStore's key holds `c0` to `c3` and writes every loop over them four times.
- **Why N0259 is there.** §16.1: "Arrays of move types need element-wise move tracking (future)". ADR 0075 §2.15
  kept locals, fields and parameters out because they "would need per-element drop flags".
- **What the code already does.** Three facts make the restriction narrower than it looks:
  - **An element is never a move path.** `isMovePath` (`src/nir/move_paths.cee:186`) admits fields and box derefs
    only; a move out of `a[i]` is already N0251 ("out of an element or enum payload of …",
    `src/nir/move_check.cee:549`), and `drops.cee:265` destroys an overwritten element in place, "A referent or
    element is always initialised". So an array is one leaf of its local's tree, with at most one drop flag.
  - **Copy-ness of structs already looks through arrays.** `flatten` (`src/check/structs.cee:694`) puts an array's
    element in the struct's contained types, so a struct with a `[AtomicU64; 4]` field is computed non-Copy.
  - **The type table does not.** `TypeTable.isCopy` (`src/types/types.cee:1670`) answers true for every array (the
    `_` arm), and `needsDrop` false: both are true only because N0259 keeps move types out.
- **The rest of the language is ready.** `for (x in a)` gives `borrow T` for a non-Copy element (§16.4); the slice
  methods taking closures take references (§36.4); `a[..].swap(i, j)` and `replace(mut a[i], v)` move elements
  without leaving a hole (§16.5, ADR 0075 §2.11); array patterns refuse by-value bindings of non-Copy elements
  (ADR 0085 §2.7, "checked but cannot arise"); `isSend` follows the element (`types.cee:1956`), and `isShare` stops at the library's thread-safe types (§38.3).

Per-element drop flags are needed only if a single element may be moved out. This ADR keeps that refused, so no
array ever has a hole, and none needs them.

## 2. Decision

### 2.1 The rule

An array owns its `N` elements, which are always all initialised. Whatever the element type:

- **Elements are places, never moved out of.** `a[i]` may be borrowed (`a[i].load()`, `borrow a[i]`,
  `borrow mut a[i]`), assigned when `a` is mutable, and read by value only when `T` is Copy. `let x = a[i];` and
  `move a[i]` for a non-Copy `T` stay N0251, whose message for an array now ends "only Copy values can be read
  from there (use 'replace(mut a[i], …)' or 'a[..].swap(i, j)')". Array patterns keep ADR 0085 §2.7.
- **The array is one value.** It is Copy iff its element is (§14.1, now true of `isCopy` too), needs drop iff its
  element does, and moves as a whole: a local, field, parameter, result, payload or capture. Moving it leaves the
  whole place moved-from (N0250 after), under one drop flag when it is moved on some paths only.
- **Destroying it destroys every element, from index 0 upward** (§14.5), as a vector's are (§23.1). Overwriting
  `a[i] = v` evaluates `v`, destroys the old element, then writes, as any overwrite (§14.5).
- **Unchanged:** references and scoped types as elements (§16.1, N0246), arrays of imported C structs (N0294),
  `[T; 0]`, equality (N0207 for any element type but integers, `bool` and fieldless enums), constants of array type
  (N0216), `extern "C"` signatures (Copy elements only).

### 2.2 Building one

- **A literal**, `[e1, …, en]`: each element moved in, left to right.
- **A repeat**, `[e; N]`: for a Copy `e`, one evaluation copied, as today. For a non-Copy `e`, `e` must be a static
  initialiser (§39.4: a `@constant` call such as `AtomicU64.constant(0)`, a struct literal of them), which has no
  effects and no drop, so its value is laid down `N` times bit for bit and once or `N` evaluations cannot be told
  apart. This is §2.15's static rule, now everywhere. Any other non-Copy `e` is N0259 "'[e; N]' copies its value;
  for 'AtomicU64', give a constant ('AtomicU64.constant(0)') or build it with 'arrayFrom'".
- **`arrayFrom(f)`**, a built-in function (§2, beside `replace` and `swap`; a package's own `arrayFrom` shadows it):
  `arrayFrom<T, N>(f: borrow mut fn(usize) -> T) -> [T; N]`, calling `f(0)` to `f(N − 1)` in order and moving each
  result into its element. `T` and `N` come from the explicit list or the expected type:
  `let words: [AtomicU64; 4] = arrayFrom((i) => AtomicU64.create(key.word(i)));`. A panic in `f` aborts (OD-5), so
  no half-built array is ever observed or destroyed. Effects: `f`'s.
- Not chosen: `[T; N].fill(f)` or `.from(f)` (G41's suggestion). `[AtomicU64; 4]` in an expression already parses as
  a repeat whose value is a type name, and `fill` is the slice method that copies one value (§16.5).

### 2.3 Slices and generic code

- `a[..]`, `a[lo..hi]` and `borrow mut a[lo..hi]` give `Slice<T>`/`SliceMut<T>` of any element type, as §21.3
  allows: `fn sum(s: Slice<AtomicU64>)` takes `SLOTS[..]`, `k.cells[..]` and `v.view()` alike. The methods that copy
  (`fill`, `copyFrom`, `copyWithin`, `extend`) keep `T: Copy` (N0259); `swap`, `sortBy`, `forEach`, `any`, `all`
  and `partitionPoint` take any `T`.
- `[T; N]` with a type parameter `T`: allowed when `T: Copy` (today), and for any `T` once phase 2 is in (§2.4); in
  phase 1 an unbounded `T` stays N0259, since `needsDrop(T)` is true for it. No `NoDrop` bound is added. Each
  instance decides its own copy-ness and drop glue, as `Vector<T>`'s does.
- A struct, enum or optional holding such an array is a move type, and needs drop iff the element does; `Send` and
  `Share` follow the element (`[AtomicU64; 4]` is both).

### 2.4 Phasing

- **Phase 1: elements without drop glue.** N0259's element test becomes `!needsDrop(T)` everywhere instead of in
  statics only (`src/check/arrays.cee:138`): atomics, structs and enums of them, `@noncopy` structs of plain data.
  References and other scoped types stay out (N0246). Moving such an array is a `memcpy`, as copying a Copy one is,
  so no backend emits anything new except `arrayFrom`; the moves are the move checker's, which already has them.
  This is G41.
- **Phase 2: any element type.** `needsDrop([T; N]) = needsDrop(T)`, drop glue for arrays in the C backend (a loop
  over `p->a[i]`, as `Vector`'s at `c_helpers.cee:519`, inside struct, enum and async-frame glue that reaches an
  array field) and in the interpreter (`destroyStep` pushes the elements in reverse, so they pop in index order).
  Unbounded `T` in generic code joins here. `[String; 3]`, `[Vector<u8>; 4]`, `[FileHandle; 2]` become values.
- Phase 2 needs no drop flag beyond today's per-local one, because §2.1 keeps elements whole. What it adds is glue,
  and the audit that every place that copies an array bitwise (`interpreter.cee:1733`, the C backend's assignment of
  array places, async-frame spills, closure captures) is a move once the array is not Copy.

## 3. Consequences

- CosmoStore writes `cells: [AtomicU64; 4]` and loops over `k.cells[..]`; a key wipe is
  `for (c in k.cells) { c.store(0); }`. Counters by kind become `[AtomicU64; KINDS]`, indexed by the kind.
- No accepted program changes meaning. The only visible change in phase 1 is that `TypeTable.isCopy` of an array
  follows the element, which no accepted program can observe, since every accepted array had a Copy element (or was
  a static, never copied, or C state under N0294).
- Arrays never have holes: taking one element out is `replace` or `swap`. This is stricter than Rust (which allows
  moving out of an array by constant index) and is what lets the design need no per-element flags. A later ADR could
  allow taking a whole array apart (`let [a, b, c] = arr;`, every element bound), as ADR 0123 §2.6 does for structs.
- `[e; N]` with a non-Copy `e` evaluates nothing at run time it would not at compile time; a program that wants `N`
  calls says `arrayFrom`, so the count is visible in the source.

## 4. Implementation and checks

**Phase 1** (estimate: 120 to 200 lines of compiler, 6 to 8 corpus files):
1. `src/types/types.cee`: `isCopy` of `TypeKind.Array` is `isCopy(element)`; `needsDrop` of it is
   `needsDrop(element)` (false in phase 1 for every array that passes N0259).
2. `src/check/arrays.cee` `checkElementCopy`: the test is `needsDrop`, not `inStatic && !needsDrop`; the message
   says "an array's elements must not need drop yet; 'String' has a drop block or contains one" (phase 2 removes
   it). The repeat's operand: Copy, or a static initialiser (reusing statics.cee's predicate), else N0259 (§2.2).
3. `src/nir/move_check.cee:549`: the array wording of N0251 (§2.1).
4. `arrayFrom`: checker (built-in, inference from the expected type as `replace`'s), NIR `NirRvalue.ArrayFrom(f)`,
   the C backend (a loop into the destination), the interpreter (the same loop), effect checking (`f`'s effects).
5. Spec: §16.1's v0 restriction replaced, §16.2 (`[e; N]`, `arrayFrom`), §21.3's last sentence, §39.4's "Elsewhere
   array elements stay Copy" removed, §14.1, diagnostics.md N0259 and N0251; guide/builtins.md, book ch08 (its N0259
   example stays valid: `clone` of a vector of `String`).
6. Corpus (`tests/reference/tests/corpus`):
   - run-pass `arrays-of-atomics.cee`: a local, a field, a parameter, a result, an enum payload and an optional of
     `[AtomicU64; 4]`; `arrayFrom`; a constant repeat; `for` by reference; `Slice<AtomicU64>` from an array, a static
     and a vector through one function; `replace(mut a[i], …)` and `a[..].swap`; threads sharing a
     `borrow [AtomicU64; 4]` (Share).
   - run-pass: a `@noncopy` struct of bytes as element, moved whole across a function and a closure capture.
   - check-fail: `let x = a[i]` and `move a[i]` (N0251); use after moving the array (N0250); `[AtomicU64.create(0); 4]`
     (N0259); `[String; 2]` and an unbounded `[T; N]` (N0259 until phase 2); `fill`/`copyFrom` on
     `Slice<AtomicU64>` (N0259); a by-value array pattern binding (N0205).
   - emit-c: `struct K { cells: [AtomicU64; 4]; }`, to pin the layout (32 bytes, align 8).
7. Checks: `python3 tools/contract_tests.py --ceesafec out/ceesafec` (includes the corpus under `--interp
   --check-leaks`, so the interpreter is held to the C build's outputs); the fixed point (the compiler's own sources
   use no such array, so stage 2 equals stage 1); CosmoStore's key with `[AtomicU64; 4]` under `ceesafec test` and
   its fault sweep.

**Phase 2** (estimate: 250 to 400 lines, 6 more corpus files):
1. Drop glue for arrays in `c_helpers.cee` (standalone and as a field, a payload, a box's contents, a frame's local)
   and in `interpreter/runtime.cee`; `needsDrop` now true for such arrays everywhere it is asked.
2. The bitwise-copy audit of §2.4, each site a move or a check that the type is Copy.
3. `checkElementCopy` keeps only the length, size, reference and C-boundary tests.
4. Corpus: run-pass `arrays-that-drop.cee` with a counting `drop` element (oracle: the order 0, 1, …, N−1 at scope
   end, at overwrite of `a[i]`, at reassignment of the whole array, at a moved-on-one-path array, in a struct field
   after the struct's `drop` block, in a `Vector<[T; 2]>`), `[String; 3]` and `[Vector<u8>; 4]` under
   `--check-leaks`, a generic `fn first<T>(a: borrow [T; 2]) -> borrow T`; check-fail for the same moves as phase 1;
   `tools/fault_sweep.py` over `arrayFrom` of vectors; the interpreter's destruction order equal to the C build's.

## 5. Questions for the owner

1. **Which element types, now?** Phase 1, types without drop glue, then phase 2, any type, as separate releases?
   Recommended: yes; phase 1 closes G41 with little risk, phase 2 waits for a user of `[String; N]` or similar.
2. **Partial moves out of an element refused for good** (`replace`/`swap` instead), so no per-element drop flags?
   Recommended: yes.
3. **`[e; N]` for a non-Copy `e` only when `e` is a static initialiser**, laid down `N` times? Recommended: yes; it
   is ADR 0075 §2.15's rule, and a call with effects is spelt `arrayFrom`.
4. **`arrayFrom(f)` as a built-in function in phase 1**, rather than `[T; N].from(f)` or a lib/core function?
   Recommended: yes, a built-in; `[T; N]` cannot be a receiver in an expression, and a library function cannot
   build an array of a non-Copy `T` without `unsafe`.
5. **A fallible `tryArrayFrom(f) -> Result<[T; N], E>`**, destroying the elements built when `f` fails? Recommended:
   not now; with phase 2, if asked for (in phase 1 nothing it builds needs destroying).
6. **Unbounded `[T; N]` in generic code waits for phase 2**, with no `NoDrop` bound in between? Recommended: yes.
7. **Destruction from index 0 upward** (a vector's order), not reverse (locals' order)? Recommended: index 0 upward,
   as `Vector`, `StableVector` and a struct's fields.

## As built (0.86.0, phase 1)

- **No new NIR rvalue.** `arrayFrom` lowers to a zero-filled result and a loop that calls the closure into each
  element, so the C backend is unchanged. Phase 2 will need a write of an element that does not drop the zero bytes
  before it.
- **Effects:** the calls through `arrayFrom`'s closure take the closure literal's effects, as `@effectsOf` does.
- **A temporary's elements:** a non-Copy element of an array temporary is borrowed in place, so `make()[1].load()`
  works.
- **One exception:** an array of C state stays Copy, as before.
- **One expectation changed:** `check-fail/adr0075-errors.cee`. `[AtomicU64.constant(0); 2]` is accepted now, so
  that line uses `AtomicU64.create(0)`.
