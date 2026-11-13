# ADR 0114 — What a read through a place needs, and what it carries

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-b`; released in ceesafec
  0.42.0. Raised by the writers of the book's chapters 13 to 22 (gaps B1, B3 and B5 of the second round).
- Scope: the move checker (`src/nir/move_check.cee`), the borrow checker's loan edges (`src/nir/borrow.cee`), the
  pointer cast (`src/check/unsafe.cee`); `spec/ownership.md` (O3), `spec/lifetimes.md` §3, `spec/type-system.md`
  §13.5, §14.4 and §27.2; the guide. No new diagnostic code, no new syntax, no change to the libraries, the runtime
  or the generated C: every program accepted before means what it meant, and three kinds of program that were
  refused are accepted.

## 1. Context

Three rules of the flow checkers were stricter than the invariants they keep, and each was stricter in one spelling
only, so a reader met a refusal that a rewording of the same program did not get. Measured on ceesafec 0.40.0 at
`d97381f`, 2026-11-13:

| Program | Refused | The same program, accepted |
|---|---|---|
| `let f = p.first; Console.writeLine(p.second);` (both fields `String`) | N0250 "'p' is used here after it was moved" | `borrow p.second`, `let s = p.second`, `p.second.view()` |
| in `impl<region r> NonEmpty<r>`, `for (part in borrow mut this.parts) { return part; }` with result `StringView<r>?` | N0245 | `loop { match (this.parts.next()) { … } }` |
| in `impl<region r> Table<r>`, `for (pair in this.pairs) { return Some(pair.value); }` with result `StringView<r>?` | N0245 | `for (i in 0..this.pairs.length) { … this.pairs[i].value … }` |
| `fn f(r: borrow mut T) { let p = r as *mut T; r.x += 1; }` | N0250 at `r.x += 1` | `let p = (borrow mut *r) as *mut T;` |

`type-system.md` §27.2 already said of the cast that "the borrow ends at once", which the compiler did not do for a
`borrow mut` variable: it moved the variable into the cast.

## 2. Decision

### 2.1 A read through a place needs the value that holds what it reads through (B1)

A read, a borrow or a write of a place whose path goes through a reference, into an element or into the text of a
string needs the **longest move path the place starts with** to be definitely initialised, not the whole local. For
`p.second`'s text that is `p.second`; for `bag.items[0]` it is `bag.items`; for `*lender.count` it is `lender.count`;
for `(*r).x` it is `r`, as before.

N0250 and N0212 name that path ("'p.first' is used here after it was moved"), which for a place through a local
reference is the local, as before.

**Why it is sound.** O3 asks that a place be definitely initialised where it is read. The memory a place such as
`p.second.*` names is reached from the value of `p.second` alone: every step after the move-path prefix (the deref
of a reference or of a vector's or string's buffer, an index, a variant's payload) starts from that value and from
nothing else in `p`. A move-path node's state covers all its leaves, so "the prefix is definitely initialised" is
exactly "the value those steps start from is there". O2 already lets a field be moved out of a struct without a
`drop` block while the others stay owned (O5: "a partially moved struct drops only its remaining initialised
fields"); the checker already accepted `borrow p.second` and `p.second.view()` after `let f = p.first`, which read the
same memory. Nothing changes for destruction: the drop flags are per move path and do not depend on this check. A
struct with a `drop` block is still never partly moved (N0252), so its fields are read only while it is whole.

Still errors, with tests: the text of the field that was moved, an element of a moved vector (read and written), the
whole value after a partial move (`look(p)`), a field after the whole value was moved, a field moved on one path.

### 2.2 N0250 says where the value was moved

N0250 carries a note, "the value was moved here", at the nearest statement, going back from the use along the blocks
where the path may still be moved, that moves the path, a part of it or a value that holds it. A move into a `move`
closure is such a statement. There is no note when the move is in the statement itself (`f(x, x)`).

### 2.3 A value read through a local reference carries what the referent holds (B3)

ADR 0074 §2.4 gave a reference parameter two placeholders, its own loan and its referent's, so that a value read
through `this` carries only what `this` refers to holds, and can be returned with a region of the referent
(`StringView<r>` from a `borrow Table<r>`). The rule stopped at the parameter: a value read through a **local**
reference made from it took every loan of that local, the outer placeholder included. A `for` makes exactly such a
local: `for (x in this.items)` borrows the elements as a slice and reads `(*slice)[i]`, and
`for (x in borrow mut this.parts)` keeps the iterator reference in a hidden local and passes `&mut *iterator` to
`next`. So the items could not leave with the referent's region, while the index form and the `loop`/`match` form,
which read through `this` directly, could.

The rule now follows the reference to where it was made. For a local reference `x` whose one definition is
`x = &p` (lifetimes.md §3):

- a value read through `x` (`v = (*x)…`, with no other reference on the way) takes the loans a deep result of `x`
  takes: those of `p`'s local, less the outer placeholder when `p` is reached through a reference parameter; and if
  `p` is itself `(*y)…` for a local reference `y`, those of `y`'s referent, by the same rule;
- a deep call result (`t = f(a)` where `a = &(*x)…`) likewise takes what `x`'s referent holds.

A local reference with more than one definition, or one not made by a borrow (a parameter view, a call's result),
keeps the old rule: all its loans. So does the result of a built-in operation that is given a place through `x`:
unlike a copy, a built-in's result may point into what it was given (a region's allocation points into the region),
so it keeps every loan of `x`, the loan on `p` included.

**Why it is sound.** A value copied out of memory holds the references that were stored in that memory, and no
others: reading does not make a reference to the memory read. The loans stored in what `x = &p` refers to are, in
the points-to model of lifetimes.md §3, the loans of `p`'s local (every store into `p`, direct or through a
reference, adds an edge to it: §25.7 rule 2). The loan of the borrow `&p` itself is a loan *on* `p`, held by `x`;
the value read does not point into `p`, so it does not need `p` to stay borrowed. This is the argument ADR 0074 §2.4
made for a parameter, applied one step on; the reference `x` itself still holds its loan for as long as it is live,
so a loop still freezes the vector or the iterator it runs over. What the item views (the text) is still held:
the loans of `p`'s local travel with it.

Still errors, with tests: the text an item views changed or destroyed while the item lives (N0241, N0243), the
vector changed inside its own loop (N0241), an item of a local iterator returned past the text it splits (N0244),
and a view of an *owned* string of the field returned as if it had the text's region (N0245: that view is a loan of
the field itself). `check-fail/local-reference-read-keeps-loans.cee` holds the cases that would be holes if the rule
dropped too much: a view of a local string or of a local vector's element made through a local reference and
returned or kept past the owner (N0244, N0243), the owner changed while such a view lives (N0241), a view stored
through the reference (a field, a `push`, an optional's payload) and read back through it past the text's life
(N0243), an item of a local iterator kept past the text (N0243), and a region's allocation made through a reference
to the region and kept past the region (N0243).

### 2.4 The cast of a `borrow mut` to a raw pointer reborrows it (B5)

`r as *mut T` and `r as *T`, for `r` a `borrow mut T` variable or field, reborrow `r` for the cast, as every other
use of a `borrow mut` does (§13.3), instead of moving it. The reborrow ends with the cast, because a raw pointer
holds no loan (§27.2, which already said so). `r` is usable afterwards.

**Why it is sound.** A raw pointer is outside the safe rules by design: making one is safe, using one is `unsafe`
(§27.3), and the writer of the `unsafe` block answers for aliasing. The cast gave such a pointer before; what
changes is only that the safe reference is not consumed, which `(borrow mut *r) as *mut T` already allowed. No safe
reference is duplicated: the reborrow is the single live exclusive reference during the cast and is dead after it.
The cast is still a use: a moved reference cannot be cast (N0250), and a variable that is lent cannot be mutably
reborrowed for one (N0241).

## 3. What was not done

- A partial move out of a struct with a `drop` block stays N0252, and a move through a reference N0251.
- An implicit borrow in a `let` (`let r: borrow dyn Shape = box;`, gap B7) is not added: a value is lent without
  the word `borrow` only as an argument (§13.2). N0205 now says to write `borrow`.

## 4. Tests

`tests/reference/tests/corpus/`: `run-pass/partial-move-read-through.cee`, `run-pass/partial-move-destroys-once.cee`
(moves on one branch, in a loop, and a field assigned again), `check-fail/partial-move-read-through.cee` (2.1, 2.2);
`run-pass/for-item-deep-return.cee`, `check-fail/for-item-deep-return.cee`,
`check-fail/local-reference-read-keeps-loans.cee` (2.3);
`run-pass/raw-cast-reborrows.cee`, `check-fail/raw-cast-reborrows.cee`, `check-fail/raw-cast-types.cee` (2.4). The
run-pass tests pass natively under `--check-leaks` and in the interpreter.
