# ADR 0162 — A value is not copied through a temporary it only passes through

- Status: **Proposed**, 2026-11-17, at the owner's word ("Go ahead and write ADR 0162"), for ClayWind's finding O13:
  a function's frame holds every copy of a large value. Nothing is implemented; §5 says what the implementation must
  show before it is accepted as done.
- Scope: a new pass over NIR (`src/nir/`), run where closure-call specialisation runs, before the C backend and
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

## 5. What the implementation must show

- **Soundness, by the rules of `optimisations.md` §1:**
  - the run corpus (`run-pass`, `run-fail`, `ffi`) with the pass and with `--no-temporary-forwarding`: the same
    output, exit status, panic and allocation report, natively and in the interpreter;
  - the fault sweep and the cancellation sweep unchanged in what they report;
  - the fixed point, and the release chain, on the compiler built with the pass.
- **That it does what it is for**, measured and written into this section:
  - the number of locals of the function in §1's shape before and after (expected: five values to three);
  - ClayWind's GPU host built at the commit before its `Raster` shrank (`7315bac`): `main`'s frame for an iPhone,
    745 KiB before; and the frame of one async function of CeeSafe.Web or the HTTP client, before and after;
  - `ceesafec`'s own build time and binary size before and after, since the pass runs on every function.
- **A corpus test** whose C snapshot shows a forwarded call result, a forwarded chain, and each rule of §2.1 as a
  case that is not forwarded (a temporary read twice, read through a projection, a target read by the rvalue, an
  awaited call).
- **If a measurement disappoints** (the frame of §1's shape does not shrink as expected, or build time grows by
  more than a few percent), this ADR is amended before it is accepted as done, not after.
