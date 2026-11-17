# ADR 0162 — A value is not copied through a temporary it only passes through

- Status: **Withdrawn** by the owner, 2026-11-17 ("Go ahead drop it"), after it was accepted ("I accept ADR 0162 as
  written"), implemented and measured: the pass was sound and gained nothing in a release build (§5). **Nothing of
  it is in the compiler.** This document stays as the record of what was tried, why its premise was wrong, and what
  the numbers were; the implementation is in the history of branch `adr/0162-temporary-forwarding` (commit
  `d49106ca`).
- Scope (as proposed): a new pass over NIR (`src/nir/`), run where closure-call specialisation runs, before the C backend and
  before the interpreter; `src/main.cee` (its switch); `optimisations.md` (a new §6),
  `intermediate-representation.md` §3, `c-backend.md` §2.1; the corpus's C snapshots, which change. No change to
  the language, to a library or to the runtime.

## 1. Context

- **What the owner's iPhone showed.** ClayWind's GPU host ended as it started (ClayWind's findings O11, O13). Its
  `main` kept a `Raster`, 123 KiB then, five times in its frame, 745 KiB on a main thread whose stack is 1 MiB on
  iOS. Two things were changed around it (0.62.1: a windowed build's `main` holds nothing; ClayWind: a `Raster` is
  3 KiB now, and its packager links a larger stack). The cause in the compiler is as it was.
- **Where the copies come from.** One line of the program,

  ```
  var masks = match (Raster.create(memory, capacity)) { Ok(r) => r, Err(_) => { return 1; } };
  ```

  is five values of the C that ceesafec emits, each a variable of the function for the whole function:

  ```
  t_25 = ceeM_Raster_create(t_22, t_24);   // the call's result, a Result<Raster, AllocError>
  t_26 = t_25;                             // the match's scrutinee
  v_r_6 = t_26.u.v_Ok.f__0;                // the binding `r`
  t_21 = v_r_6;                            // the match's value
  v_masks_7 = t_21;                        // `masks`
  ```

  - `t_25` and `t_21` are **temporaries that a value only passes through**: each is assigned once and read once, by
    the statement that follows.
  - Every local and temporary of a function is declared at the top of the C function (c-backend.md §2.1), since
    the body is a control-flow graph and not nested blocks. A C compiler gives each its own slot: clang and gcc
    share slots only between variables of disjoint C scopes, and these have one scope. `-O2`, which ceesafec builds
    with, removed none of the five.
- **It is the same in an async function**, where the locals are fields of the task's frame (type-system.md §40):
  the frame is allocated with every such temporary in it.
- **NIR has no pass that removes a copy.** Its passes check (moves, borrows, effects), insert (drops) and
  specialise (closure calls, ADR 0019); `optimisations.md` has no section on copies.
- **A move makes this sound to reason about.** A temporary that is assigned once and moved once has no other
  reader by construction, and the move checker has already proved that nothing reads it afterwards.

## 2. Decision

### 2.1 The rule

**Temporary forwarding.** In a block, two adjacent statements

```
T = R;          // T: a compiler temporary
D = use(T);     // the next statement: T whole, as the only operand
```

become one, `D = R`, and `T` is no longer a local of the function, when all of these hold:

1. **`T` is a compiler temporary**: a NIR local without a name (`NirLocal.name` is none), not a parameter, not a
   drop flag, not a `using` local, and not one of a `debugAssert`'s own temporaries (ADR 0087 §2.5), whose
   statements are compiled conditionally.
2. **`T` is mentioned exactly twice** in the function: as the whole target of the first statement (no projection)
   and as the whole operand of the second (no projection). `StorageDead(T)` statements do not count, and are
   removed with it. A `Ref` of `T`, a `Drop` of it, a mention in a terminator or in another block rules it out.
3. **`R` is a `Use`** (a place, moved or copied, or a constant) **or a `Call`** that is an ordinary call: no task
   role (`rvalueTasks` is 0: not `await`, not `Async.run`, not a spawn). §3 says why not the other rvalues yet.
4. **`D`'s local is not read by `R`**: no operand of `R` is a place of the local `D` starts from. (Before, `R` was
   evaluated wholly into `T` before `D` was written; the C for `D = R` must not write `D` in pieces while `R` still
   reads it. With this rule it cannot matter how the backend writes `D`.)
5. **`T` and `D` have the same type** (a `Use` neither converts nor wraps).

The same holds with a block's `Return(T)` in place of the second statement, when `T = R` is the block's last
statement and `R` is a `Use`: the block returns `R`'s operand.

When `R` is `Use(S)`, the forwarded operand keeps `S`'s own moved flag. The pass runs to a fixed point over a
function, so a chain `T1 = R; T2 = T1; D = T2` becomes `D = R`.

### 2.5 Amendment: a straight way, and several assignments (2026-11-17)

The first build with the rule of §2.1 forwarded nothing in the line of §1. Its NIR shows why:

```
bb1:  _0(r) = (_3 as Ok)._0;
      _2 = _0(r);              // the match's value, assigned in the arm
      StorageDead(_0(r));
      goto bb4;
bb4:  StorageDead(_4);
      StorageDead(_3);
      _1(masks) = _2;          // read in the block after the match
```

The two statements are never adjacent in one block: the value of a `match` or an `if` is assigned in each arm and
read after them. So rules 2 and "adjacent" are, as implemented (`optimisations.md` §6.1 has them in full):

- **Every mention of `T` but one is a whole assignment of it**: one assignment or several (an `if`'s two branches),
  each forwarded, or none is. `return T` takes a `T` with one assignment.
- **The way from each assignment to the read is straight and empty**: the rest of its block, then through each
  `goto` the next block from its start, with nothing on it but `StorageDead` statements, none of a local that `D`'s
  place mentions; and each block the ways pass into is entered only by them. `D` is then written in each arm, where
  `T` was, and nothing can read `D` or change what `D`'s place names in between.

Rules 1, 3, 4 and 5 are as written. What §2.2 says is not touched still holds, with "adjacent" read as "on such a
way".

### 2.2 What it does not touch

- **Named locals.** `r` and `masks` above stay: a debugger shows them, and `--dump-hir` names them. So the line of
  §1 goes from five values to three (`t_26`, `r`, `masks`), not to one.
- **A temporary read through a projection** (`t_26.u.v_Ok.f__0`), read twice, borrowed, or dropped.
- **Evaluation order, panics and drops.** `R` is evaluated where it was; `D` is written where it was, by the next
  statement, with nothing between; a check statement between the two (`BoundsCheck` and the others) means they
  are not adjacent, and nothing is forwarded.

### 2.3 Where it runs, and its switch

- **After analysis, on the NIR that is compiled and the NIR that is interpreted**, where closure-call
  specialisation runs (`c_backend.cee`, and `main.cee` for `run --interp`), after it. Checks and diagnostics see the
  NIR as it was: no diagnostic changes.
- **`--no-temporary-forwarding`** (`build`, `run`) leaves every temporary, as `--no-closure-specialisation` and
  `--no-placement` leave theirs (`optimisations.md` §1).
- **`--dump-nir` prints the NIR before the pass**, as it prints it before specialisation: the corpus's NIR
  snapshots do not change. The C snapshots (`emit-c`) do, where a temporary went.

### 2.4 What a program can observe

Nothing: its output, exit status, panics, drop order and allocation counts are the same (`optimisations.md` §1).
Its frames are smaller, and a copy of the value is not made.

## 3. Alternatives considered

- **Coalescing by liveness**: giving several NIR locals one C variable when they are never live together, as a
  register allocator's copy coalescing does. It would also fold `r` into `masks`, and the result into the
  scrutinee. It needs a liveness analysis over the control-flow graph and care with locals whose address is
  taken; the rule above needs neither, and is where such a pass would start. Left for a later ADR, with
  measurements from this one.
- **Named locals too** (a binding assigned once and moved once by the next statement). One slot fewer in the
  line of §1, but a name the program wrote would be gone from a debugger. Not without a decision about debug
  builds, which is its own question.
- **Other rvalues** (`Aggregate`, `EnumAggregate`, `Array`, `Builtin`, arithmetic). Sound by the same argument, but
  the C backend may write an aggregate's fields one at a time, and some builtins take their result's address.
  Each needs its own reading of the backend. `Use` and `Call` are the two in the chain that was measured.
- **Awaited calls.** Their result reaches the awaiter through the task's frame (type-system.md §40.6); forwarding
  it changes what the coroutine lowering stores where. Left out (rule 3).
- **Scoping the C variables** in nested blocks so that the C compiler shares their slots. The body is a
  control-flow graph with `goto`s; a variable's uses are not a C scope.
- **Leaving it to the C compiler.** `-O2` did not remove a copy of 123 KiB, in clang or gcc.
- **A larger stack**, which is what ClayWind's packager now asks for on iOS. It hides the size; it does not
  reduce it, and an async task's frame is not on that stack.

## 4. Consequences

- **Smaller frames and fewer copies** wherever a call's result is matched or a block's value is assigned: the
  commonest shapes of CeeSafe code (`match (f()) { Ok(v) => v, … }`, `let x = if (c) a else b;`).
- **The emitted C changes** for most functions: the corpus's `emit-c` snapshots are regenerated once, and the
  compiler's own C changes (the fixed point is reached again, on the new C).
- **A new optimisation to keep correct**: a switch, a section in `optimisations.md`, and the corpus run both ways.
- **The compiler's own sources** stay within what ceesafec 0.8.0 accepts (the contract suite's `release chain`
  step checks it).
- **Not covered**: the three values that remain in the line of §1; the zero-filling of every named local at a
  function's start (`T v = {0};`), which writes each large local once whether or not it is used on the path taken.

## 5. What the implementation showed (2026-11-17)

**It is sound, and in a release build it gains nothing.** The second half is not what §1 expected, and §1's reading
was wrong; both are recorded here so that the owner decides with them whether the pass stays.

- **Soundness** (`optimisations.md` §1):
  - the corpus with `out/ceesafec` built from the branch: every family passes (run-pass 404, run-fail 55, ffi 20;
    13 `emit-c` snapshots regenerated, each a temporary gone and its assignments redirected); run-pass and run-fail
    in the interpreter, 404 and 55; run-pass, run-fail and ffi natively with `--no-temporary-forwarding`, and under
    `-Werror`, all passing;
  - the fixed point: the compiler built with the pass emits the C its own rebuild emits;
  - `emit-c/temporary-forwarding.cee`, one function for each case: temporaries with the pass off and on are 5 and 3
    (a call matched), 3 and 2 (an `if` as a value), 3 and 1 (a chain), 6 and 6 (read by an operation), 1 and 1 (the
    call reads its target); its exit status is 19 natively, interpreted, and with the pass off.
  - Not run on this branch yet: the whole contract suite, its release chain step, the fault and cancellation sweeps.
- **What it is for, measured:**
  - **ClayWind's GPU host at `7315bac`, for an iPhone, `-O2`: `main`'s frame is 745,408 bytes without the pass and
    745,456 with it.** No gain.
  - The same host as a debug build (`-g`, so `-O0`), `ceeF_main`'s frame: 1,124,464 bytes without, 874,016 with: a
    fifth less.
  - A 4 KiB struct matched out of a call (§1's shape): `-O2`, the same frame with and without; `-g`, five pages
    without and four with.
  - Async task frames (ClayWind's chat server, 21 of them): 213 fields with and without. The temporaries that went
    were never live across a suspension, so they were not fields.
  - The program's C: 994 temporaries without, 943 with (5 % fewer); 6,633 lines and 6,531.
  - `ceesafec` building itself: 61.7 s without and 57.7 s with, once each on a machine that was not idle (no
    difference shown); the binary is 48 bytes smaller.
- **Why §1 was wrong.** It counted the C's variables, five for the one line, and said `-O2` "removed none of the
  five". That was read from the C source, not from the binary. clang at `-O2` already forwards a copy through a
  variable that is written once and read once (a call's result slot, a `memcpy` of a `memcpy`), which is all this
  pass removes. What fills the frame in a release build is what the pass leaves by its own rule: the call's
  `Result`, the binding `r`, `masks`, and the copy inside the `GpuBackend` made from it, with the values of the
  functions inlined into `main`.
- **What it left of the decision, and the decision:**
  - the pass would have helped debug builds (`-O0`), where no C compiler forwards anything, and made the C a little
    shorter;
  - it did not address ClayWind's finding O13 in release builds. That needs what §3 set aside: coalescing by
    liveness (so that `r` and `masks` are one value), or passing a large value by reference where it is moved;
  - it was one more optimisation to keep correct, for that gain. **The owner dropped it.** `optimisations.md` has no
    §6, and `--no-temporary-forwarding` is not an option.
  - **For whoever takes O13 up again**: measure the binary's frames first (`otool`, `-fstack-usage`), with the
    optimisation level a release uses, before counting what the C declares.
