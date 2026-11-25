# ADR 0252 — Two-phase `copyFrom`, `fill` and slices lent mutably

- Status: **Proposed**, 2026-11-25, at the owner's instruction to close the SQLite port's finding 43
  (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md`, "An argument cannot read the receiver a method borrows mutably").
  Implemented on branch `fix/g5-compiler` (§7); not released.
- Scope: lifetimes.md §5 (B5); type-system.md §16.5 (bulk operations); intermediate-representation.md (the lowering
  of `copyFrom` and `fill`); the NIR builder (`src/nir/nir_builder.cee`) and the borrow checker's N0242 message
  (`src/nir/borrow.cee`); the corpus.

## 1. Context

The port's wal-index copies a block's bytes into a caller's buffer:

```ceesafe
fn readAt(v: borrow Vector<u8>, at: usize, into: SliceMut<u8>) {
    into.copyFrom(v.view()[at..at + into.length]);   // N0242: cannot use 'into' while 'into' is mutably borrowed
}
```

`into.length` is a `usize`, read and finished before the copy starts, yet ceesafec 0.95.10 refuses it. The port reads
it into a local first (`let end = at + into.length;`). Rust accepts the same code with two-phase borrows.

CeeSafe has two-phase borrows already (B5, lifetimes.md §5; ADR 0073 §2.2): for a `borrow mut this` method of a place,
and for every `mut p` argument, the place (indexes and their bounds checks included) is found where it stands and the
mutable loan is made at the call, so `v.push(v.length)` and `fail(mut s, s.count)` compile. Two kinds of receiver or
argument were left out, and the finding is the first:

1. **The bulk writes.** type-system.md §16.5 says of `d.copyFrom(s)` and `d.fill(v)`: "The receiver's loan starts
   before the argument is evaluated: these calls are not two-phase (B5 applies only to `borrow mut this` methods of a
   place). So the argument cannot read the destination: `s.fill(s[0])` is N0242." They are built-ins, lowered with
   their operands in order, so the receiver's loan is made first.
2. **A slice lent mutably**, written `a[lo..hi]`: as a receiver (`buf[..].fill(buf[1])`, also of a `borrow mut this`
   method of `SliceMut`) or as an argument `mut a[lo..hi]` (`f(mut buf[0..2], buf[3])`). It is not a place but a
   slicing of one, so neither B5 nor ADR 0073 §2.2 applied; its loan was made where it stands.

Reproduced with 0.95.10 on 2026-11-25: `into.copyFrom(v[at..at + into.length])`, `s.fill(s[0])`,
`buf[..].fill(buf[1])`, `buf[0..4].writeLE(0, buf[5])` and `f(mut buf[0..2], buf[3])` are N0242; `a.add(a.n)` with
`a: borrow mut Acc`, `s.writeLE(0, s[1])` with `s: SliceMut<u8>` and `v.writeLE(0, v[1])` with a vector already
compile (type-system.md §16.6 said the byte writers were "not two-phase, like `copyFrom`", which held only for a
slicing receiver; it is corrected).

## 2. Decision

### 2.1 What becomes two-phase

- **`d.copyFrom(s)` and `d.fill(v)`** are two-phase in their receiver, as a `borrow mut this` method is (B5): when the
  receiver is a place or a variable's reference (`into`, a `SliceMut` parameter), its loan is made after the argument.
- **A slice lent mutably**, `a[lo..hi]` (any of its forms, `a[..]`, `a[lo..]`, `a[..hi]`, and an array's or vector's
  methods that write, which are `a[..]`'s), is two-phase wherever a place would be: as the receiver of `copyFrom`,
  `fill` or a `borrow mut this` method, and as a `mut a[lo..hi]` argument of any call (functions, methods, closures,
  function values).

```ceesafe
into.copyFrom(v[at..at + into.length]);   // was N0242
s.fill(s[0]);                             // was N0242 (type-system.md §16.5's own example)
buf[..].fill(buf[1]);                     // was N0242
v.viewMut().fill(truncate<u8>(v.length)); // was N0242: `viewMut()` is the vector's slice `v[..]`
f(mut buf[0..2], buf[3]);                 // was N0242
```

### 2.2 When it is sound

The rule is ADR 0073 §2.2's, applied to two more forms. It holds because each of these is true:

1. **Evaluation order is unchanged** (language-spec.md: strictly left to right, receiver before arguments). Where the
   receiver or argument stands, its place is found, with its index values copied and their bounds checked; for a
   slice, `lo` and `hi` are evaluated, copied to temporaries and range-checked against the place's length there.
   Only the creation of the loan (`&mut a[lo..hi]`, the reborrow of `*into`) moves to the call. So
   `buf[i..j].fill(bump(mut i))` fills the range named before `bump` ran.
2. **No mutable loan exists until the call.** Each argument's reads finish before it, and so does each shared borrow
   that ends inside the argument (`v[at..at + into.length]` reads `into.length`, a `usize`).
3. **No argument's result may keep a borrow of the receiver into the call.** The loan is made at the call and checked
   there against every loan still live: a source that borrows the destination conflicts (N0241), as it did before,
   now reported at the receiver with the argument's borrow in the note:
   `d.copyFrom(d[0..2])`, `buf[0..2].copyFrom(buf[2..4])` (places do not reason about disjoint ranges, lifetimes.md
   §4; `buf.copyWithin(2, 0, 2)` is the copy within one slice).
4. **A slice's place is reserved.** Its bounds were checked against the length the place had where it stood. While
   a later argument does more than read (a call, a borrow, a move), a shared loan of the sliced place is held from the
   slice to the call (of the variable it is reached from, when the place is a slice's elements `(*s)`), as ADR 0073
   §2.2 holds one for `mut p`. A later argument that could change the length or the storage (`v[0..2].fill(pop(mut
   v))`, `buf[0..2].fill(poke(mut buf))`) is N0241, so the loan made at the call is the slice that was checked. A
   receiver that is not sliced (`into`, `v` in `v.push`) keeps B5's rule: it is not reserved, and `copyFrom`'s length
   check is made at the call, after the loan, on the slice the loan gives.
5. **Regions.** The loan is the same loan, of the same place and region, made later; it ends where it ended before
   (the call returns `void`, or the method's result is derived from it as before). Nothing a region bound or a result
   carries is changed, so §25's rules see the same loans.
6. **Closures.** A closure that captures the receiver holds its loans while the closure value lives. Called within an
   argument, it ends before the call; passed to the call, it is live there, and the receiver's loan conflicts (N0241).
7. **`async`.** An `await` in an argument suspends before the mutable loan exists. A sliced receiver's reservation is
   a shared loan held across it, as a `mut p` argument's is (ADR 0073 §2.2). Another task reaches the place only
   through a loan of it, which would conflict with the reservation or, without one, with the receiver's own variable,
   so no task can change the place between the argument and the call.

### 2.3 What stays refused

| Code | Diagnostic | Why |
|---|---|---|
| `d.copyFrom(d[0..2])` | N0241 at the receiver | the source borrows the destination into the call (§2.2.3) |
| `buf[0..2].copyFrom(buf[2..4])` | N0241 | two slices of one place overlap as places (lifetimes.md §4) |
| `v[0..2].fill(popped(mut v))` | N0241 | a later argument would change the reserved place (§2.2.4) |
| `whole(mut v).fill(truncate<u8>(v.length))` | N0242 + note | the receiver is a call's result: the call runs, and lends `v`, before the arguments |
| `h.buf.fill(h.buf[0])`, `h.buf` a `SliceMut` field | N0242 + note | a reference held in a field is reborrowed where it stands (ADR 0073 §2.2 covers a variable's reference only); Question 1 |

For the two N0242 rows, the message gains a note at the read: "the call lends it before its arguments are read: only
a receiver or 'mut' argument that is a place, a slice of one or a variable's reference waits for them (two-phase,
lifetimes.md §5), not a call's result or a reference held in a field; read the value into a local before the call".

## 3. Lowering

`lowerTwoPhaseOperands` (`src/nir/nir_builder.cee`), which already lowers B5 receivers and `mut p` arguments, takes
the two new forms:

- `copyFrom` and `fill` lower their operands through it, receiver first (`copyFrom`'s length check and the copy stay
  after the operands, on the place the loan gives).
- A `HirExpr.Slice(true, place, lo, hi)` operand: the place is lowered, `lowerSliceAt` evaluates the bounds and emits
  the range check, and the bounds read from places are copied to temporaries (`fixedSlice`); the `NirRvalue.Slice` is
  assigned to its temporary after the last argument, where a place's `Ref mut` is. A bound that is a compiler
  temporary is not copied: it is assigned once. The reservation, when a later argument does more than read, is a
  `Ref` of the place, or of its root variable when the place is unsized.
- "Does more than read" (`onlyReads`) now also counts as reads a shared slice of a place whose bounds and indexes
  only read (`src[from..from + n]`) and a struct literal whose fields only read: neither can change, move or lend
  mutably the reserved place, and a shared loan still live at the call is checked there anyway. This keeps
  reservations, ADR 0073 §2.2's included, to the calls that need them; it accepts nothing new.

The C differs from 0.95.10's only in where the slice's temporary is assigned (after the arguments) and in the bounds'
temporaries; the interpreter evaluates the same NIR order.

## 4. Options not taken

- **Two-phase for a reference held in a field** (`h.buf.fill(h.buf[0])`). The field would be reserved like a slice's
  place; it is a small step, but no finding asks for it yet, and ADR 0073 kept reborrows to variables. Question 1.
- **Two-phase for a call's result** (`v.viewMut2().fill(…)` for a user function). The call runs before the arguments
  and its result already holds the loan; delaying the call would change evaluation order. Rust refuses it too.
- **Reserving every two-phase receiver.** B5 deliberately does not (`v.push(v.pop()!)`); only a sliced receiver needs
  it, because its bounds were checked early.

## 5. Consequences

- Programs accepted before are accepted still: each loan is created at or after the point it was before, and a
  reservation is added only where a slice's loan used to begin, so it conflicts with nothing the earlier loan did not.
- Messages for the overlap cases move: `d.copyFrom(d[0..2])` was "cannot borrow 'd' while 'd' is mutably borrowed" at
  the argument, and is "cannot borrow 'd' as mutable while 'd' is borrowed" at the receiver.

## 6. Questions for the owner

1. Should a reference held in a field (`h.buf`, a `SliceMut` or `borrow mut T` field) be two-phase as a variable's is,
   reserved like a slice's place? Recommended: yes, when a port needs it.
2. The release notes should say that `s.fill(s[0])`, type-system.md §16.5's example of an error, now compiles.

## 7. Implementation (2026-11-25)

- `src/nir/nir_builder.cee`: `lowerTwoPhaseOperands` takes `HirExpr.Slice(true, …)` (`fixedSlice`, `fixedOperand`);
  `isReservedArgument` and `mayMeetReserved` count a mutable slice; `onlyReads` counts shared slices and struct
  literals of reads; `bfSliceCopy` and `bfSliceFill` lower through it.
- `tests/reference/tests/corpus/emit-c/slice-bulk.c`: the reviewed edit of ROADMAP §10's row (the slice made after
  the arguments; `to` copied where the receiver stands).
- `check-fail/slice-bulk-loans.cee`: `m.fill(m[0])` and `a[1..].fill(a[0])` lose their N0242 (they compile now).
- `src/nir/borrow.cee`: `reportConflict` adds the note of §2.3 to N0242 when the read lies within the call that lent.
- Corpus: run-pass `adr0252-two-phase-slices`; check-fail `adr0252-two-phase-slices`.
