# ADR 0036 — L1 A4: async methods through `dyn`

- Status: **Accepted** by the owner, 2026-11-01 (every question as proposed). Phase 2 of what is left of L1 A4
  (ROADMAP, the owner's "in a multiphased way").
- Scope: making an interface with async methods dyn-compatible, and what `await x.m(…)` on a `dyn I` means, checks
  and lowers to. Lifts N0331's async case (type-system.md §40.1, `src/check/dyn.cee:73-91`).

## 1. Context

Today `dyn I` is N0331 when `I` has an async method: "a call through a table does not know its frame's size yet".
Through a type parameter (`T: I`) each instance awaits the concrete method, whose frame is known, and that frame is
embedded in the awaiter's (type-system.md §40.6: a frame is placed once and never moves; c-backend.md §3a: waiting
storage is the union `cee_u`).

Through a table, the awaiter's frame is laid out without knowing which `T` it will meet. What makes this tractable in
CeeSafe:

- **The program is closed.** ceesafec compiles a program and all its packages at once (generics are instantiated,
  tables are emitted per pair (`T`, `I`) that a coercion names, type-system.md §37.4). `--split` divides the C into
  several translation units, but the compiler still sees every coercion. So the set of types that can stand behind a
  `dyn I` is known at compile time.
- **Frame sizes are computed by ceesafec** (`frameReport`, `src/backend/c_async.cee`) and asserted in the C.
- **`Async.boxed(alloc, …)` already exists** (§40.1): a frame from an allocator, the awaiter holding a pointer.

## 2. Options

| | A. Room for the largest (proposed) | B. A frame from an allocator per call | C. Keep N0331 |
|---|---|---|---|
| `await x.m(…)` | `T`, as through `T: I` | `Result<T, AllocError>`, or only inside `Async.boxed` | not allowed |
| Storage | the awaiter's frame, sized for the largest frame of `I.m` among the coerced types | the heap or a given allocator, each call | — |
| Allocation | none | one per call | — |
| Cost | an awaiter as large as the largest impl | an allocation and a failure path per call | users write enum dispatch |
| Open world | needs the whole program (which CeeSafe has) | works with separate compilation | — |

**A is proposed.** It keeps "allocation failure is a value" honest without adding a failure to every call (ADR 0032
§1). It keeps `await x.m()` the same type as through `T: I`, and it is the frame model of §40.6 with a union in
place of one member. B is still available, explicitly, through `Async.boxed` (§3.4). It caps the awaiter's size, or
breaks a cycle.

## 3. Proposed decision

1. **Dyn-compatibility.** An async method no longer makes an interface incompatible. N0331's other rule stands: a method
   taking `this` by value.
2. **Checking.** `await x.m(args)` with `x` a `borrow dyn I`, `borrow mut dyn I` or `Box<dyn I, A>` is checked as
   §37.3's call and §40.2's `await`. The receiver rules are §37.3's. The receiver and the arguments are lent into the
   callee's frame until it returns (§40.1), so `x` stays borrowed across the `await`. A bare `x.m(args)` is N0342, as
   for any task.
3. **Effects.** As §37.3: a call through `dyn I` has the effects of `I.m`'s list. Without a list, it has `suspend`
   and `alloc` (an async body cannot `sleep`, §40.1). An `impl` method whose effects exceed the list is N0295, at the
   `impl`.
4. **Storage.** For each async method `m` of each dyn-used `I`, ceesafec forms the set `S(I, m)` of the impls
   `T.m` for every `T` coerced to `dyn I` anywhere in the program. An await site through the table gets a member of
   the waiting union `cee_u`: a union of `S(I, m)`'s frames (a byte if the set is empty: no value can reach the call).
   `--explain-frames` names the member and its largest impl.
   - **`await Async.boxed(alloc, x.m(args))`** is allowed: the frame comes from `alloc`, its size and alignment read
     from the table, and the awaiter holds a pointer, as for a direct call.
5. **Recursion.** If an impl in `S(I, m)` awaits, directly or not, a call through `dyn I.m` (or anything whose frame
   contains `S(I, m)`'s union), the frames would contain themselves: **N0343**. The cycle is named through the table
   ("… awaits `dyn Walker.walk`, which may be `Tree.walk`, …"). Its sure fix is the same: `Async.boxed(SystemHeap
   {}, x.m(…))!`.
6. **The table** (§37.4) gains, per async method, the frame's size and alignment and two adapters taking `void *`:
   `init(frame, receiver, args…)` and `resume(frame)`. The frame is the callee's own `ceeAF_<T.m>`. An adapter is
   called through a pointer of its own type, so no function is called through a pointer of another type
   (c-backend.md §2). Cancellation, drops and the debugger's `tasks` command see the callee's frame as for a direct
   await: the resume chain is the same, one indirect call deeper.
7. **Interpreter.** The awaited call pushes the concrete method's activation, as a direct await does.

## 4. Consequences

- `src/check/dyn.cee` drops the async rejection. The checker's call through a table and `await` handle a `dyn`
  receiver. `src/nir/coroutines.cee` builds `S(I, m)` and checks recursion through it. `src/backend/c_dyn.cee`
  emits the async entries, and `c_async.cee` emits the union members and the indirect await.
- Spec: type-system.md §37.1, §37.3, §37.4 and §40.1; c-backend.md §2 and §3a; diagnostics.md N0331 and N0343.
- Tests:
  - run-pass: two impls of very different frame sizes behind `Box<dyn I>` and `borrow dyn I` in a `Vector`; a
    borrowed receiver across the `await`; cancellation inside a dyn-awaited method; `Async.boxed` on a dyn call;
    drops in the callee's frame;
  - check-fail: N0343 through the table; N0342 for a bare dyn call; N0295 for an impl exceeding the list; N0248 for
    a `borrow mut this` method through `borrow dyn I`;
  - frames: a `.frames` snapshot showing the union;
  - the cancel sweep and `--check-leaks` over the run-pass programs, C and interpreter agreeing.

## 5. Questions for the owner

1. Storage, A (room for the largest frame, no allocation)? **Yes.**
2. `Async.boxed` on a call through `dyn`: allowed, sizes from the table? (Proposed: yes.) **Yes.**
3. Effects of an async method through `dyn` with no list: `suspend, alloc`? (Proposed: yes.) **Yes.**

## 6. Implementation (2026-11-01)

- **Checking** (§3.1–3.3): `src/check/dyn.cee` no longer rejects async methods. The call and `await` needed no
  change: a dyn method is declared from the interface's signature, so it is async exactly when the interface's is.
  `effectsOf` (`src/nir/effect_check.cee`) gives an async dyn method without a list `suspend` and `alloc`.
- **The frame** (§3.4, §3.6): `struct ceeAF_dyn_<D…>_m<k>` as c-backend.md §3a describes, with `ceeAI_`, `ceeAR_`,
  `ceeAT_` and `ceeAN_` like any async fn's, so the await, `Async.run` and `Async.boxed` paths are unchanged except
  for the boxed frame's size. Its members are the methods in slot `k` of every table of the dyn type
  (`dynAsyncImpls`), so of every type a coercion names. The frame is emitted once every member's is, and its size
  and the union's offset are asserted against ceesafec's arithmetic (`dynFrameParts`). `--explain-frames` reports it
  at each await (`explainDynAwaits`).
- **The table** (§3.6): an async slot's entry is `void (*m<k>)(void *frame, void *self, args…)` with `uint64_t
  s<k>`, the frame's size for that type, beside it. The entry stores the resume adapter `…_<k>_r` and the size in
  the frame, then starts the method in its member.
- **Recursion** (§3.5): the coroutine pass follows a call through `dyn` to every impl it may reach. N0343 says so:
  "contain itself through 'dyn Node.sum', which may be its own". Only coerced types count, so a type that
  is never made a `dyn Node` cannot close a cycle.
- **The interpreter** needed no change: `prepareCall` already resolves a dyn call to the receiver's method.
- **Tests:**
  - run-pass/async-dyn: boxes in a `Vector`, references, a void method, drops in the callee's frame, `Async.run` of
    a call through the table, a cancelled callee, `Async.boxed` with a counting allocator, and a tree of
    `Box<dyn Node>` summed recursively through `Async.boxed`;
  - check-fail/async-dyn (N0342, N0248) and check-fail/async-dyn-frames (N0343 through the table, N0295 for an impl
    exceeding the list);
  - frames/async-dyn (the `.frames` snapshot with the dyn line);
  - check-fail/async-interface no longer expects N0331.
