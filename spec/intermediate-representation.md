# Intermediate Representations (as implemented)

Status: describes the code as of M1 increment 1c. The design rationale is in `architecture.md` §2.
That document is the plan; this one is the implementation record.

## 1. AST

`src/syntax/ast.cee`. Tables of nodes with spans. Printed by `ceesafec parse --dump-ast`.

## 2. HIR

`src/hir/hir.cee`. It is typed and fully resolved:

- Constant references are replaced by their values.
- Parentheses and expression-bodied functions are removed.
- `else if` becomes nested `if`.
- Assignment targets are locals.

Printed by `ceesafec check --dump-hir`. Static semantics: `type-system.md`.

## 3. NIR (CeeSafe IR)

`src/nir/`. A control-flow graph of basic blocks per function, modelled on Rust MIR.
It is not SSA. M1 subset:

```text
function  = locals: [local]          -- _0.._(n-1) are the parameters, in order
            blocks: [block]          -- bb0 is the entry block
local     = id, type, name?          -- name is null for compiler temporaries
block     = id, statements: [stmt], terminator
place     = local projection*                  -- projection = '.' field | Deref (M2a, M2b) | Downcast(V) | Index(operand) (M2d)
stmt      = Assign(place, rvalue)
          | Eval(rvalue)             -- for calls whose result is unused (void)
          | StorageDead(local)       -- scope end; temporaries at the end of their statement (M2b)
          | Drop(place, flag?)       -- inserted by drop elaboration (M2c); guarded by a bool flag local if present
          | Unpack(place, V, [local?])          -- M3c: move every payload of variant V out of a consumed enum local
          | BoxFree(place, flag?)               -- M3c: free a partially moved box without destroying its contents
          | BoundsCheck(index, length)          -- panics unless index < length (M2d)
          | RangeCheck(low, high, length)       -- panics unless low <= high <= length (M2d);
                                                 -- with a `Text` place (M6c), also unless both bounds
                                                 -- are UTF-8 character boundaries of that StringView
          | LengthCheck(left, right)            -- panics ("slice lengths differ") unless left == right (M9b)
rvalue    = Use(operand)
          | Binary(op, operand, operand, span)     -- op semantics: type-system.md §6 (may panic)
          | Unary(op, operand, span)
          | Cast(operand, type)                    -- lossless `as`; also round<T> to a float (type-system.md §35.4);
                                                 -- also a coercion of `borrow T`/`Box<T, A>` to a dyn type (M13 G2, §37.2):
                                                 -- the pair (T, dyn I) is in the program's DynTables; loans and moves
                                                 -- flow through it as through any cast
          | Truncate(operand, type)
          | FloatToInt(operand, Panic | Checked | Saturating, type, span)   -- M11 G7: truncate<T> of a float (§35.4); Checked and
                                                 -- Saturating also of an integer (ADR 0074 §2.9)
          | Aggregate(struct, [operand])           -- field values in declaration order (M2a)
          | Ref(Shared | Mut, place)               -- creates a loan (M2b; lifetimes.md)
          | Discriminant(place) | EnumAggregate(V, [operand])   -- M2d enums
          | Array([operand]) | ArrayRepeat(operand)             -- M2d array values
          | Slice(Shared | Mut, place, low, high)  -- a slice of an array or deref'd slice; a loan of the whole place
          | Length(place)                          -- a slice's length
          | Call(function, [operand])              -- a call; after closure-call specialisation (optimisations.md
                                                 -- §5) it may name a variant of the function (Variant > 0), and a
                                                 -- CallClosure through a known closure becomes a Call of its body.
                                                 -- A dyn method (IsDynMethod, no body; §37.3) dispatches through
                                                 -- the table of argument 0, a dyn reference; checked as a call of
                                                 -- its signature, so loans and effects need no new rule
          | Builtin(kind, [operand])               -- Console.write/writeLine; CallFunction(f, args…): a call through a
                                                 -- function value (M11 G8a), result loans from every reference argument;
                                                 -- CallClosure(f, args…) likewise through a closure (G8b), f passed on
                                                 -- as the body's environment parameter
operand   = Place(place, move?) | Const(value, type) | String(text)   -- move: non-Copy type or explicit `move` (M2c)
                                         -- a float Const holds the float's bits (M11 G7, type-system.md §35.6)
terminator= Goto(bb) | Branch(operand, then_bb, else_bb) | Return(operand?)
          | Switch(operand, [(value, bb)], otherwise_bb)    -- match (M2d)
          | Exit(operand)                          -- Process.exit
          | Unreachable                            -- after a call of type never
          | Panic(kind)                            -- `e!` on Err (M3a), `o!` on null (M6c); aborts
```

**Async functions** (L1, ADR 0031 §3.6 as amended). An async fn's NIR is an ordinary function body: nothing about
suspension changes its blocks or statements.
- `await f(a…)` lowers to the call `Call(f, [a…])`, marked as awaited in `NirProgram.rvalueTasks` (`taskAwait`).
  `Async.run(f(a…))` is the same call, marked `taskRun`. So the borrow, move, in-place and drop analyses see an
  ordinary call, with its loans, moves and result, and need no rule of their own (`lifetimes.md` §8).
- `Async.park()` is `Builtin(AsyncPark)`, a builtin numbered after the generated ones (`src/check/async.cee`).
- `await Async.scope(oom, body)` lowers to the oom value (a temporary), then `Eval(Builtin(AsyncScopeStart,
  [site]))`, then the body's statements inline, then `Builtin(AsyncScopeWait, [site, oom])`, whose value is the
  scope's `Result<void, E>`. `s.spawn(child)` is `Builtin(AsyncSpawn, [site, the child's HIR function, the child
  closure])`: a reference to the closure's environment, or, for `async move`, the environment itself, moved. `site`
  numbers the scope program-wide. The borrow checker keeps each spawn's environment live until its scope's wait, and
  ends it at the scope's start (`AsyncScopeStart` does nothing else; type-system.md §40.5, ADR 0033 §3.2).
- A task set's start (ADR 0053), `set.spawn(child)` or `set.trySpawn(child)`, is checked as calls of lib/core's
  `TaskSet<E>` steps around `SetSpawn(record, admitted, child)` (HIR), which lowers to `Builtin(AsyncSetSpawn, [the
  child's HIR function, the record pointer, the admission code, the child closure])`, of type `u32`: 0 started, else
  the admission's code, or 3 with no frame. `await set.spawn(c)` is `await TaskSet.spawned(set, SetSpawn(set.recordOf(),
  await set.room(), c))`; `trySpawn` uses `tryRoom` and `started`. The child's environment is moved in, so the borrow
  checker needs no rule for it: its captures hold no loans (N0349).
- `await Async.boxed(alloc, g(args))` lowers to `Builtin(AsyncBoxed, [the allocator's type, g, args…])`, of type
  `Result<T, AllocError>`; it is not an edge of the await graph. `s.spawn` in a `scopeIn` scope carries the
  allocator's type as a fourth argument. The interpreter runs `AsyncBoxed` as a call on the task's stack, wraps its
  value in `Ok`, and calls a user allocator's `allocate` and `deallocate` as C does (the frame sizes are the C
  backend's layouts).
- `await Async.race(a, b)` and `all` lower to `t = Builtin(AsyncJoin, [site, 1 for race or 0, a's function, a, b's
  function, b])`, the index (0 or 1) of the child that returned first, then `r0 = Builtin(AsyncJoinResult, [site, 0])`
  and `r1 = …[site, 1]`, which move the children's results out, then the call `settle(t, r0, r1)` of lib/std's
  `AsyncRaceJoin<T, E>` or `AsyncAllJoin<E>`, which makes the value and drops what is not used. A child is a
  reference to its closure's environment, or, for `async move` (ADR 0074 §2.3), the environment itself, moved.
- `await Async.timeout(ms, timedOut, body)` and `deadline` (ADR 0046) lower to the deadline and `timedOut` in
  temporaries, then a join of kind 2: `Builtin(AsyncJoin, [site, 2, body's function, body, until, at])`, a race whose
  second child is lib/core's `AsyncTimeoutJoin<T, E>.until`, an async fn given the deadline `at` as its argument (for
  `timeout`, `AsyncTimeoutJoin<T, E>.after(ms)`). `r1` has `until`'s type, `Result<void, AsyncError>`, and the settle
  call is `AsyncTimeoutJoin<T, E>.settle(t, r0, r1, timedOut)`.
- The **suspension points** of an async fn are its awaited calls, its `AsyncPark`s, `AsyncScopeWait`s and
  `AsyncJoin`s, numbered from 1 in block order
  and statement order (0 is the entry). The C backend (c-backend.md §3a) and the interpreter (§5) act on them.
- The effect checker gives an awaited call `suspend` and its callee's effects, `AsyncPark` `suspend`, and a `taskRun`
  call `sleep`, `alloc` and its callee's effects without `suspend` (type-system.md §32.1, §40.4).

The **coroutine pass** (`src/nir/coroutines.cee`) runs after the effect checker. It builds the graph of which async fn
awaits which and reports each awaited call on a cycle as N0343 (type-system.md §40.1). It changes nothing. Closure
specialisation and placement (`optimisations.md`) skip async functions.

The **interpreter** runs an awaited call as a call, on the same activation stack. `Async.run` runs a scheduler
(`runTasks`): the root task and each spawned child have their own activation stack, and run one at a time from a
queue, children in spawn order after their parent parks. `AsyncPark` returns to the scheduler with the stack intact;
`AsyncScopeWait` does too while children remain, and runs again when the last returns. When no task can run, the
virtual clock advances to the earliest registered deadline (type-system.md §40.8). A child gets its own copy of its
closure's environment, which in C is a field of the awaiting frame.

Lowering rules (HIR → NIR, `NirBuilder`):

- **Evaluation order is explicit.** Operands are evaluated left to right into operands or
  temporaries. If the right operand of a binary operator (or a later argument) contains a call,
  an earlier operand that reads a local is first copied into a temporary. This keeps
  left-to-right semantics when calls can mutate state (M2+).
- `&&` and `||` become branches into a boolean temporary. The right operand is evaluated
  only when needed.
- `x op= e` becomes `x = Binary(op, x, e)`; the same applies with a place such as `p.a.b`.
- A struct literal evaluates its initialisers **in source order** into operands, then builds an
  `Aggregate` with them in declaration order.
- A field read of a place is a place operand (`_3.a.b`). A field read of any other value (a call
  result) first spills the value into a temporary. A method call passes the receiver as the first
  argument, evaluated before the explicit arguments.
- `while` becomes `cond → body → cond`, and `loop` becomes `body → body`. `break` and `continue`
  jump to the innermost loop's exit or head.
- `for` over an iterator (HIR `ForNext(variable, the iterator's LocalDecl, the call of next, body)`, ADR 0074 §2.8)
  becomes: the iterator into its local, with the header's temporaries; then `option = next(&mut iterator)` with its
  own temporaries ended, `Switch(Discriminant(option))`: `null` to the exit, `Some` to `Unpack(option, Some,
  [variable])`, the body and back to the call. The iterator and the header's temporaries end at the exit.
- Statements after a `return`, `break`, `continue`, `Process.exit` or never-typed call are
  unreachable and are not lowered. Blocks with no predecessors are not emitted.
- Constants are already folded in HIR. NIR performs no optimisation in M1.

- References (M2b):
  - an explicit or implicit borrow becomes `t = Ref(k, place)`;
  - auto-deref reads `(*r)`;
  - a use of a `borrow mut` variable becomes `Ref(Mut, (*r))` (reborrow);
  - a call-site borrow of a value spills it into a temporary, which gets `StorageDead` at the end of
    the statement;
  - for a `borrow mut this` receiver, the receiver's `Ref` is emitted after the other arguments (B5);
  - every statement carries a source span for borrow diagnostics.
- The **borrow checker** (`src/nir/borrow.cee`, `lifetimes.md`) runs on NIR inside
  `Compilation.Create`, so `ceesafec check` reports borrow errors too.
- **Moves and destruction (M2c).**
  - Scopes hold locals *and* deferred statements. Leaving a scope (its end, `break`/`continue`,
    or `return`) emits, in reverse order of position, `StorageDead` for locals and a fresh
    lowering of each deferred statement.
  - By-value parameters form the outermost scope.
  - `return e` computes `e` into an unscoped temporary first, then ends the statement's
    temporaries and every scope, then returns.
  - **Move paths** (`src/nir/move_paths.cee`): each local is a tree, expanded field by field
    wherever a field is assigned or moved; all siblings are expanded, so children partition their
    parent. A forward dataflow over the leaves tracks maybe-init, maybe-uninit and maybe-moved.
    `MoveChecker` uses it for N0212/N0250–N0253.
  - **Drop elaboration** (`src/nir/drops.cee`) runs only when every check passed:
    - it inserts `Drop` before `StorageDead` of a local that needs drop, and before an assignment
      that overwrites one; the assignment becomes `t = rvalue; Drop(p); p = t`;
    - what is dropped depends on the state: whole, nothing, a flagged drop, or per field for a
      partially moved struct;
    - one bool flag per leaf is updated after every statement that changes that leaf's state.
    - In a run-once function (an `async move` child, type-system.md §36.5, ADR 0074 §2.3), `*env` (local 0's
      referent) is a move path, tracked from the entry as a box's contents are, but without an allocation leaf. At
      `StorageDead(env)` the captures still initialised are dropped, whole, flagged or field by field; the backends
      no longer drop the child's environment after the body, only that of a child that never started.

    The C backend emits drop glue `ceeD_<len><Name>(T *)`: the `drop` block, then each field that
    needs drop, in declaration order.

- **Arrays, slices and `for` (M2d).**
  - A slice type is a reference to the unsized `[T]`; `s[i]` is the place `(*s)[i]`.
  - An index is evaluated into a `usize` temporary (or kept as a constant), then checked with
    `BoundsCheck` against the array's constant length or the slice's `Length`, then used as an
    `Index` projection. Element places are never move paths: an element's init state is its
    array's.
  - An assignment evaluates its target's indexes (and checks them) before the value.
  - `a[lo..hi]` evaluates the base place, then the bounds left to right, then `RangeCheck` (none
    for `a[..]`), then `Slice`.
  - **Bulk operations (M9b).** `d.copyFrom(s)` lowers the receiver slice, then the source slice, each into a
    temporary, then `LengthCheck(Length(s), Length(d))`, then the built-in `SliceCopy(d, s)`, which copies
    with `memmove` semantics. `d.fill(v)` is the built-in `SliceFill(d, v)`. Neither is two-phase.
    `d.swap(i, j)` (M11 G8c) lowers the slice and both indices, then `BoundsCheck(i, Length(d))` and
    `BoundsCheck(j, Length(d))`, then the built-in `SliceSwap(d, i, j)`. `v.retain(keep)` lowers the vector
    reference and the closure, then a temporary `SliceMut` reborrowed from the vector's elements, a call of
    lib/core's `SliceAlgorithms<T>.retain` on it and the closure, and `VectorTruncate(v, kept)` (type-system §36.4).
  - `for` evaluates its bounds, or its sequence place and length, once into locals that live until
    the loop exits. A `usize` counter is compared with the end at the head. The body binds the
    variable in its own scope; `continue` jumps to the counter's increment, and `break` to the
    exit, which ends the header's locals.

- **Results (M3a).**
  - `e?` and `e!` move `e` into a temporary, switch on its tag, and in the `Ok` block read
    the payload `t as Ok .0`.
  - On `Err`, `?` builds `Err(t as Err .0)` of the function's return type and returns through the
    same path as `return`; `!` ends in `Panic`.
  - **Optionals (M6c)** lower the same way with `Some` (tag 1) as the success variant: on `null`,
    `?` returns the `null` of the function's return type and `!` ends in `Panic(UnwrapNull)`.
    `o == null` (HIR `IsNull`) takes a shared borrow of `o` (or of a temporary), reads
    `Discriminant(*r)` and compares it with 0, so `o` is not consumed.
  - **Vectors and strings (M6d).** A vector dereferences, like a box, to its unsized elements: `v[i]`
    is the place `v.*[i]`, with a `BoundsCheck` against `Length(v)`, and views are `Ref` of `v.*`.
    The deref is not a move path, so moving an element out is a move out of a deref (N0251).
    `push`/`append` lower their argument before the receiver's `Ref mut` (B5). `for (x in v)`
    iterates over a view held in a header temporary, so the vector stays borrowed for the whole
    loop. When the value written to a vector element contains a call, it is evaluated into a
    temporary and the element's index is checked again just before the write (the call may have
    shrunk the vector).
  - **Slot maps and stable vectors (M6e)** are built-in calls on a borrow of the container
    (`insert`, `getMut` and `remove` take `Ref mut`, with `insert` two-phase); `get`'s optional
    reference carries the map's loan like any built-in result. A stable vector dereferences like
    a vector, so `sv[i]` is the place `sv.*[i]`; the C backend renders its elements through the
    chunk table. Liveness treats the targets of `Unpack` as definitions (fixed in M6e: a reference
    payload bound by a consuming match used to be live from the function's start).
  - **Region parameters (M6x).** A loan-carrying value stored into a field or element is lowered
    into a temporary first, so the borrow checker sees a local whose loans flow into the target.
  - **String views (M6c).** A literal is a constant operand of type `StringView` (no loans); a place
    is needed only when it is spilled into a temporary. `s[lo..hi]` always emits a `RangeCheck`
    with `Text`; `s.bytes()` is a `Cast` to `Slice<u8>` that keeps the view's loans.
  - Moving a variant payload out of a local (only these lowerings do that) uninitialises the whole
    local in the move dataflow, so the consumed result is not destroyed again.
  - **Open temporaries.** The builder records the temporaries of every statement still being
    lowered, with the scope they belong to. An early exit (`return`, `break`, `continue`, `?`)
    ends them together with that scope, before its locals. Before M3a the temporaries of an
    enclosing statement, such as a `match` scrutinee, were never ended on such paths.
  - An expression statement whose value needs drop assigns it to a statement temporary, which is
    destroyed at the end of the statement (type-system.md §17.5).

- **Boxes and consuming matches (M3c).**
  - Move paths go through box derefs (`NirPlace.IsMovePath`). Once a box's contents are tracked,
    the box node has two children: its contents (fields below), then an *allocation* leaf that is
    initialised while the box owns its memory.
  - Drop elaboration destroys a partially moved box as: the initialised fields of its contents,
    then `BoxFree` of the box. `BoxFree` is flagged if the allocation is only maybe initialised.
  - A consuming `match` moves its scrutinee into a temporary. An arm that lists payloads starts
    with one `Unpack`, which moves every payload into its binding or into a hidden local of the
    arm and consumes the temporary as a whole. Other arms leave the value in the temporary, which
    is destroyed at the end of the statement.

- **Generic code (M6a).** NIR is built for generic versions too (type parameters are move types
  that may need drop), so the move and borrow checkers run on them. Instantiations skip those
  checks, which would only repeat the generic version's diagnostics. Drop elaboration and C
  emission skip generic versions.

Printed by `ceesafec build --dump-nir`.

## 4. LIR — deferred

`architecture.md` §2.4 plans a C-shaped LIR between NIR and C. In M1 the C emitter consumes NIR
directly. The C-level decisions (type names, helper selection, temporaries) are made in one place,
`src/backend/c_emitter.cee`, because NIR operands are already flat. LIR will be introduced when a
second consumer appears or when drop elaboration makes the emitter non-trivial (M2/M7).
This is a recorded deviation from the plan, not an omission.

## 5. The NIR reference interpreter (M7c)

`ceesafec run --interp <files> [-- <arguments>]` executes a program's NIR directly
(`src/interpreter/`). It serves as an oracle for the C backend. The C backend and
the interpreter consume the same checked NIR, after drop elaboration. Agreement between them
therefore tests the C backend and its runtime, not NIR lowering.

### 5.1 Observable behaviour

The interpreter must produce exactly what the native program produces:

- the bytes written to standard output and standard error, in the same order: every write to
  standard error first flushes standard output (`ceesafe_rt_hosted.c`);
- the exit status:
  - `main`'s result, or `Process.exit`'s code (the operating system keeps the low 8 bits);
  - 134 after a panic, as `abort()` gives natively;
- the panic message (`ceesafe panic: <reason> at <file>:<line>:<column>`);
- the allocation counters: `CEESAFE_FAIL_ALLOC_AT` and the `CEESAFE_ALLOC_REPORT` line. The interpreter
  counts every attempt that `ceesafe_alloc` would. This includes vector growth, stable-vector chunks,
  slot-map slots, arena and scratch chunks, pool slots, and `Process.run`'s argument copies. The
  allocation-failure sweep therefore sees the same attempts.

To get these, the runtime's algorithms are reimplemented step for step:
- `ceesafe_rt_core.c`: growth (doubling, at least 4), arena chunks with their 16-byte header,
  scratch chunks of 64 KiB, the pool slot size, and the panic and report texts;
- the emitter's helpers (`c-backend.md` §2): push, pop and clear, slot-map generations, and the
  drop-glue order.

### 5.2 Memory model

Memory is **byte-addressed**. Values have the target layout of `TypeTable.LayoutOf`, which is the
same layout the generated C asserts. Pointers are 8-byte addresses, and null is 0. Memory is a set of
*allocations*, each with a base address aligned to 16 (as `malloc` gives) and a size:

| Kind | Created by | Ends at |
|---|---|---|
| local | a function's entry (every local, zero-filled, as the C locals are) | `StorageDead`, or the function's return |
| heap | `ceesafe_alloc` | `ceesafe_free` |
| static | a string literal (read-only), the empty-vector sentinel (size 0) | never |

`StorageDead` ends a local's allocation. The next write to that local (a whole assignment, or a
write or borrow through one of its fields) gives it a *new* allocation, so an old reference to it
stays dead.

**Interpreter faults.** Each of the following stops the program with
`ceesafe interp: fault: <what> at <file>:<line>:<column>` on standard error and exit status 70:
- an access to an ended allocation, or outside an allocation's bounds;
- a dereference of null;
- a misaligned access through a raw pointer (M8a): the address is not a multiple of the pointee's
  alignment;
- a write to static memory;
- `ceesafe_free` of anything but the base of a live heap allocation;
- destroying a stack-placed box whose slot has already ended (M9e), the placed counterpart of a double free; and
  running a placed site again while its previous box is alive (`memory-model.md` §5.1);
- a *use* of uninitialised bytes. Heap memory starts uninitialised. Each byte carries an
  initialised bit, which copies carry along: copying a struct with padding is not a use.
  Reading a scalar to compute with it, to branch on it, to compare it, as a tag or as an address
  is a use.
- a bounds check that bounds-check elimination removed, and that fails (M9c, `optimisations.md` §2.8): the
  interpreter evaluates every check, and the compiled program would not have made this one
  (`an eliminated bounds check fails at …`). `ceesafec run --no-bce` turns the analysis off. Likewise an operation whose
  overflow check was eliminated (`optimisations.md` §3) and that would overflow faults (`an eliminated overflow check
  fails at …`); `ceesafec run --no-oce` turns that off.

No correct program can fault. A fault means the compiler, its runtime, or its checks let an
unsafe program through, or (since M8a) that unsafe code broke its contract (`unsafe.md` §2). This makes the interpreter a dynamic safety oracle: liveness, bounds and
initialisation. It does not track borrows (no Stacked Borrows), so it does not catch aliasing
violations that stay in bounds.

### 5.3 Execution

- Calls recurse on the host stack. The interpreter runs on its own thread with a 1 GiB stack.
- Drop glue and the runtime helpers are interpreted in the same order as the emitted C. A user
  `drop` function, for example, runs before its fields' drops.
- Arithmetic follows `IntegerSemantics` (type-system.md §6–7), the same table the checked C helpers
  implement.

### 5.4 The host

- **Output.** Standard output, standard error and standard input are streams supplied by the
  caller. The CLI uses the process's own.
- **Arguments.** `argv[0]` is the first source file's path. The arguments after `--` follow it.
- **Configuration.** The CLI reads `CEESAFE_FAIL_ALLOC_AT` and `CEESAFE_ALLOC_REPORT` as
  `ceesafe_platform_config` does (leading digits).
- **Files and sockets** use the host C library through P/Invoke (`open`, `read`, `write`,
  `close`, `unlink`, `socket`, …) on Linux and macOS, with the same flags and errno mapping as
  `ceesafe_rt_io.c`. On Windows, I/O is an interpreter fault ("unsupported").
- **Working directory.** An optional working directory resolves relative file paths and is
  where child processes start. Tests use it, since they run in parallel in one process. The CLI
  uses the process's own directory.
- **`Process.run`** starts the program with lib/std's `Process.run`, after flushing standard output.
  When the interpreter's output streams are the process's own, the child inherits them. When the
  caller captures output (tests), the child's output is copied into the captured streams.

**C code (M8b).** The interpreter runs no C. A call to an `extern "C"` function is interpreted only
for this model of the C library, used when the declaration's parameter and result types have the
expected shape (integers, `bool` or pointers of the right widths):
`strlen` (reads bytes up to a zero byte, each access checked), `puts` (writes the bytes and a
newline to standard output, returns 0), `abs`, `malloc` and `free` (C5: a heap allocation of the
interpreter's memory, not counted as CeeSafe's), `memcmp`, `memcpy` and `memset` (checked accesses;
`memcpy` with overlapping ranges is a fault, as it is undefined in C). Any other extern call is the
fault `unsupported: call to C function '<name>'`. Exported functions are ordinary CeeSafe functions.
**User allocators (C5).** A box with a user allocator gets its memory from the allocator's
`allocate` method and returns it through `deallocate`, run as CeeSafe code; `this` is a one-byte
read-only allocation per allocator type.
**Function pointers (M8d).** The C address of an extern or exported function is a value in a range
no allocation uses (from `0xFFFF000000000000`); a call through it runs the exported function, or
the C model for an extern. A call through null or any other value is a fault. Volatile accesses
are performed like other accesses (the interpreter does every access once, in order).

### 5.5a Placement and the drop trace (M5)

- `InterpreterOptions.Placement` (on by default since M9e, like the C backend's) applies the same
  `PlacementAnalysis` as the C backend. A stack-placed site stores the value in a new *local* allocation, which is
  not counted. If the site's previous allocation is still live when it runs again, that is a fault: natively the site
  has one slot (`memory-model.md` §5.1).
- A `Drop` of the site's box local destroys the contents, then ends that allocation. A `BoxFree`
  just ends it. Ending it there, where C's slot lives on until the function returns, makes a
  late use a fault: a stronger check.
- `InterpreterOptions.DropTrace`, when given a list, records every executed `Drop` statement as
  `<function> <line>:<column> <type>`, in order. M5's drop-timing check compares the traces
  with and without placement.

### 5.5 Agreement tests

- `tools/run_corpus.py --interp` runs every run-pass and run-fail program with `ceesafec run --interp` (`testing.md`
  §1). Standard output, standard error
  and exit status must equal the corpus expectations. The native runs check the same
  expectations under every C configuration.
- The allocation-failure sweep runs the interpreter beside the native ASan build, for the
  baseline and at every failure point. The outcomes must be equal.
