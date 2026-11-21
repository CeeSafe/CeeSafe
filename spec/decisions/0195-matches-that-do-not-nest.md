# ADR 0195 — A `match` that does not nest what follows it, and three messages

- Status: **Accepted** by the owner, 2026-11-21 ("Accept all as recommended": every question as §5 or §6 recommends; release order: a patch first, then the next minor). Proposed the same day. Raised by CosmoStore's CEESAFE_GAPS G36 to G39 (G36 P1, G39 P2, G37 and G38
  P3), found with 0.54.0 to 0.79.0 and reproduced with 0.84.1 for this ADR.
- Scope: the C structurer (`src/backend/c_structurer.cee`, c-backend.md §3 steps 2 and 6); the test harness
  (`src/host/test_runner.cee`, `harnessSource`; ADR 0049 §3.4); the pattern checker (`src/check/patterns.cee`); the
  argument check (`src/check/references.cee`, `optionalBorrowArgument`); the parser's names (`src/syntax/parser.cee`);
  grammar.md §1.3, type-system.md §15.2 and §22.1. One language change, accepting a program that is refused today
  (§2.4); one reserved word made contextual (§2.3, a question). No change to NIR, the runtime or what a program does.

## 1. Context

### 1.1 G36: the C of a long function nests once per `match`

`ceesafec test` of a package with 300 trivial `@test fn tN() -> Result<void, TestFailure>` fails with
`--cflag -fbracket-depth=256` (clang's default; GitHub's macos-15 runner): `main.c:10090:1045: fatal error: bracket
nesting level exceeded maximum of 256`. The harness source is flat: one `if (name == "tN") { match (tN()) { … } }`
per test, each arm returning. The nesting is made by the structurer. Measured on the C of `ceesafec build --emit c`
(maximum depth of `{ ( [`, 300 repetitions in one function):

| Source shape, 300 times in one function | C depth |
|---|---|
| `if (name == "tN") { return N; }` | 5 |
| `if (name == "tN") { match (tN()) { Ok() => { return 1; } Err(_) => { return 2; } } }` (the harness) | 303 |
| `match (tN()) { Ok() => {} Err(f) => { return …; } }` | 303 |
| `tN()?;` | 303 |
| `if (x == 0) { … } else if (x == 1) { … } …` | 303 |

255 `?` in one function compile with `-fbracket-depth=256`; 256 do not. So this is not the harness's problem alone:
any function with about 250 `?`, or 250 statement `match`es, fails on a C compiler with clang's default, and comes
near C11's minimum translation limit of 127 nested blocks (5.2.4.1) long before that. Two rules combine:

1. **Placement** (§3 step 2, `placementOf`): a block with one forward predecessor is placed *inline*, at the branch
   to it. After `t()?;` the rest of the function is reached only from the `Ok` case, so it is placed in that case:

   ```c
   switch (t_2) {
   case ((uint32_t)0):
       ceeD_RvS11TestFailure(&t_1);
       t_5 = ceeF_t001();           /* the next `?`, and everything after it */
       switch (t_6) { case ((uint32_t)0): … default: return t_7; }
   default:
       return t_3;
   }
   ```

2. **Guard clauses** (§3 step 6, `hoist`) undo this for an `if` only: an arm that ends in a transfer is guarded and
   the other arm follows the `if`, which is why the first row stays at 5. For a `switch`, `hoist` only recurses into
   the cases, and `completesList` answers *true* for any switch ("conservative"), so an `if` whose arm is a `match`
   whose every arm returns is not seen to end, and its `else` (the next test) is not hoisted either.

An `else if` chain is nested in its source; it stays so in C because each `else` arm holds the condition's
temporary before its `if`, so step 6's "an `else` holding a single `if` is printed as `else if`" does not apply.

### 1.2 G37: a refused nested pattern leaves its names undeclared

`Some(Node.Items(v))` on a `borrow(a) Node?` is N0205, "this payload is a reference ('borrow Node'): bind it and
match on it separately" (type-system.md §15.2, "A payload that is itself a reference"). `checkPattern`
(`patterns.cee`, the variant payload loop; and the struct field loop, ADR 0123) replaces the refused pattern by a
wildcard without declaring the names in it, so the arm's body then gets N0200 "cannot find 'v' in this scope".

### 1.3 G38: a reserved word as a field

`struct T { class: u32; }` is N0107 "expected a field, 'fn' or 'const' in struct, found 'class'", with no mention of
the word being reserved; the same with `own`. As a parameter, `fn f(class: u32)` gets the right N0100 ("…, which is
a reserved word: choose another name (grammar.md §1.3)"), then two N0101s, because `parseIdentifier` reports without
consuming the word. `class Foo { … }` at the top is N0101 "expected 'fn', 'const', 'struct' or 'enum', found
'class'", which does not say what to write instead (language-spec.md Ex4: a `struct`, behind `Box` for identity).
The parser uses `class` nowhere but for recovery (`isDeclarationKeyword`); `own`, `where`, `yield` and `macro` not
at all.

### 1.4 G39: `borrow x` at a `borrow T?` parameter

For `fn lengthOf(s: borrow String?)` and `given: String?`, `lengthOf(given)` is accepted (§22.1, "Optional borrow
arguments", ADR 0074 §2.5), but `lengthOf(borrow given)` is N0205 `expected 'borrow String?', found 'borrow
(String?)'`. So is passing on a borrowed optional, `fn pass(r: borrow (String?)) => lengthOf(r)`. The names differ
by parentheses only, and nothing says that the first is an optional of a borrow, or that `borrow` may be dropped.
`len(borrow s)` at a `borrow String` parameter is accepted, as is `len(s)`: the optional is the one case where
writing `borrow` breaks a call. `asBorrow()` on a `borrow (T?)` is total, and `??` already applies it implicitly
(ADR 0139 §2).

## 2. Decision

### 2.1 G36: the structurer hoists switches, and bounds what it nests

2.1.1 **`completesList` is exact for a switch**: a switch completes when some case body completes or breaks the
switch. A case that ends in `return`, `goto`, `continue` or a `_Noreturn` call (the unreachable `default`'s
`ceesafe_panic`) does not.

2.1.2 **Switch guard clauses** (§3 step 6, new item): when exactly one case body of a switch completes and the
others end in a transfer, that body is moved after the switch and the case becomes `break;`, unless the body holds
a `break` of this switch (moved out, it would bind to an enclosing loop). Transfer elimination then removes what
falls through. `t()?;` prints as

```c
switch (t_2) {
case ((uint32_t)0):
    break;
default:
    return t_3;
}
ceeD_RvS11TestFailure(&t_1);   /* the rest of the function, at the same depth */
```

and with 2.1.1 the harness's `if` arms end, so each test's `else` is hoisted as in the first row of §1.1.

2.1.3 **A bound**: the structurer measures its tree (an `if`, a loop, a switch and a case each one level). A
function deeper than **96** levels is emitted flat (§3's flat form, already the fallback for an irreducible graph
and the corpus's oracle). This covers the shapes 2.1.2 cannot flatten, the long `else if` chain and deeply nested
source, leaves room under C11's 127 blocks for parentheses in a line, and makes the depth of the C a property of the
backend, not of the program.

2.1.4 **The harness** puts each test's dispatch in a function of its own, `fn ceesafeTestN() -> i32` (N its
index), and `ceesafeTestMain` is `if (name == "…") { return ceesafeTestN(); }` per test. It no longer depends on
2.1.1 and 2.1.2 to be flat, and each test's temporaries live in its own frame instead of all of them at the top of
one function of 300 tests' code. A test's name is in the listing and the protocol as before.

Of the three fixes the gap names, the emitter's is the one that is right: user code meets the same limit (§1.1). A
table of test functions is not written in CeeSafe (a field holding a function cannot be called), and one function
per test (2.1.4) is kept as well, for the frame and because it costs a dozen lines.

### 2.2 G37: the names of a refused pattern are declared, with the error type (S)

When a nested pattern under a reference payload (or field) is refused, every name it binds is declared in the arm
with the error type, as an unresolved type is elsewhere, so no N0200 or other error follows from its uses. The
message names the two-step form: "…: bind it (`Some(n)`) and match on `n` separately". The rule of §15.2 stays.

Matching *through* a reference payload (binding `v` as a `borrow(a) Vector<u32>`) is sound: it moves nothing, and is
what the two-step form does. It is not decided here (question 4): it reaches the usefulness algorithm, which treats a
reference payload as opaque, NIR's match lowering (a load through the payload's pointer before the inner tag test),
the interpreter, and ADR 0123's struct fields, for a P3 whose workaround is one more `match`.

### 2.3 G38: a reserved word where a name goes says so, once (S)

- `parseIdentifier` consumes a reserved word it reports, as the name `<missing>`, so a parameter gets the one N0100.
- A struct or enum member that starts with a reserved word followed by `:` (a field) or `(` (a variant) is N0100 with
  the same text, and is parsed as that member: one error, no N0107.
- `class` at the start of an item is N0101 "CeeSafe has no classes: declare a 'struct' (behind 'Box' for a value
  with identity; language-spec.md Ex4)".
- `class` becomes **contextual** (grammar.md §1.3, as `is` is, ADR 0085 §2.8): a keyword only at the start of an
  item, followed by an identifier, where it gets the message above; elsewhere an identifier, so `class: StorageClass`
  is a field. `own`, `where`, `yield` and `macro` stay reserved (question 3).

### 2.4 G39: `borrow x` is accepted at a `borrow T?` parameter (M), and the message tells the two types apart

2.4.1 §22.1, "Optional borrow arguments", gains: at a parameter `borrow T?` (`(borrow mut T)?`), an argument of type
`borrow (T?)` (`borrow mut (T?)`) is lent as by `asBorrow()` (`asBorrowMut()`). This covers `f(borrow given)`
(`f(given)` already being accepted) and passing on a borrowed optional, `f(r)`. The conversion is total, moves
nothing, and is the one `??` makes (ADR 0139 §2). Elsewhere (a `let` with a type, a return, a field) a
`borrow (T?)` stays a different type, as §22.1 says.

2.4.2 Where the two still meet as a mismatch, N0205 prints the optional borrow parenthesised and says which is
which: `expected '(borrow String)?', an optional borrow, found 'borrow (String?)', a borrow of an optional`, with the
fix "write 'given.asBorrow()'" (machine-applicable, so `ceesafec fix` applies it; and the reverse mismatch offers
nothing, as no total conversion exists). Type names elsewhere are unchanged: `borrow String?` is how the type is
written.

## 3. Consequences

- A function's C nests at most 96 levels whatever its source; long functions of `?` and `match` print flat,
  as guard clauses, which is also how they read in source. A function past the bound prints flat and reads worse in
  a debugger, which only an `else if` chain of about 90 arms or deeply nested source reaches.
- CosmoStore's CI drops `--cflag -fbracket-depth=1024`. Its 303 tests compile under any C11 compiler's limits.
- `ceesafec test`'s generated harness has one more function per test; its listing and protocol are unchanged.
- One error instead of two or three for G37 and G38. `class` is a name everywhere but at the start of an item.
- Programs refused today with N0205 at an argument are accepted (2.4.1). No accepted program changes meaning.

## 4. Implementation and checks

1. **Structurer** (`c_structurer.cee`): 2.1.1 in `completesList`; 2.1.2 in `hoist`, beside the `if` case, with
   `breaksList`'s walk for the switch's own `break`; 2.1.3 a `treeDepth` after `hoist`, `structureFunction`
   returning none past the bound (the caller already prints a flat body). c-backend.md §3 steps 1 (the bound) and 6.
2. **Harness** (`test_runner.cee`, `harnessSource`): 2.1.4. ADR 0049 §3.4's shape.
3. **Checker**: 2.2 in both refusal sites of `patterns.cee` (a helper that declares a pattern's names with the error
   type, also usable by or-patterns); 2.4.1 in `optionalBorrowArgument` (an operand of reference type whose pointee
   is the parameter's optional: an `asBorrow` node, as `members.cee` builds); 2.4.2 in `coerce`'s N0205 text.
   type-system.md §15.2 (the message) and §22.1 (2.4.1).
4. **Parser**: 2.3; grammar.md §1.3 (`class` moved out of the list, a paragraph beside `is`'s). tooling.md §9.4's
   port renames a C member `class` to `lock_class`; it could then keep the name (not changed by this ADR).
5. **Tests.**
   - `emit-c`: a function of 300 `?`, of 300 statement `match`es, and of a 300-arm `else if` chain, each compiled
     with `-fbracket-depth=256` and run; the first two checked flat (depth under 10), the third flat by the bound.
   - `tools/package_tests.py`: a package of 300 `@test` functions passes `ceesafec test --cflag -fbracket-depth=256`
     (fails on 0.84.1, `main.c:10090`).
   - `check-fail`: G37's repro has exactly one error (N0205); `struct T { own: u32; }` and `fn f(own: u32)` one N0100
     each; `class Foo {}` the new N0101. `check-pass`: `struct T { class: u32; }`, G39's repro, and `pass(r)` of §1.4.
     `check-fail`: `let r: borrow String? = borrow given;` with the new text and its fix (`fix` corpus).
   - The run corpus in both forms (its `clang -O2 flat` oracle) and the fixed point: the compiler's own C changes
     shape, not behaviour.

## 5. Questions for the owner

1. Fix G36 in the structurer (2.1.1 to 2.1.3) rather than only in the harness? Recommended: yes. User code with
   about 250 `?` or `match`es in one function fails the same way (§1.1); the harness alone would hide it.
2. Also give each test a function of its own (2.1.4), and release it first as a patch (it is the smaller change and
   unblocks CosmoStore's macOS CI)? Recommended: yes to both.
3. Make `class` contextual (2.3), and keep `own`, `where`, `yield` and `macro` reserved? Recommended: yes. `class`
   is S3's storage class and the natural name of a kind; reserving it bought only a message, which the contextual
   form keeps. The other four are kept for the features they were reserved for; the better message covers them.
4. Lift §15.2's rule, letting a nested pattern match through a reference payload (G37's language option)?
   Recommended: not in this ADR. It is sound but reaches coverage, NIR lowering and the interpreter for a P3; take it
   up in an ADR of its own if a second package asks.
5. Accept `borrow x` and a `borrow (T?)` at a `borrow T?` parameter (2.4.1), rather than only improving the message?
   Recommended: yes. Writing `borrow` is what one does at every other borrow parameter, the conversion is total and
   ADR 0139 already makes it for `??`; the message alone would keep a trap that cost CosmoStore three detours.
6. The bound of 2.1.3 at 96 levels? Recommended: yes: under C11's 127 with room for a line's parentheses, and far
   above anything but generated or pathological code.
