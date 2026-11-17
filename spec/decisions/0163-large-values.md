# ADR 0163 — Large values in a function's frame: reported first, then kept in one place

- Status: **Proposed**, 2026-11-17, at the owner's word ("Go ahead with 013 first then 09": ClayWind's finding O13,
  then O9). Nothing is implemented. Every number here is read from a binary built at `-O2`, which is what a
  release build uses; ADR 0162 was withdrawn because its numbers were read from the C.
- Scope: in three parts, each decided on its own (§2): a report (`src/main.cee`, the C backend's layouts,
  tooling.md); the calling convention for large values that are moved into a call (`c-backend.md` §2,
  `src/backend/`); a matched payload used where it is (`src/nir/`, `optimisations.md`). No change to the language or
  to a library.

## 1. Context

### 1.1 The measurement

A program with one value of 64 KiB, in the shape that filled ClayWind's GPU host's `main` (ClayWind's finding O13):

```
var big = match (make(n)) {          // make: fn(u32) -> Result<Big, AllocError>
    Ok(r) => r,
    Err(_) => { return 1; }
};
big.n += 1;
let holder = hold(big);              // hold: fn(Big) -> Holder, a struct with a Big in it
Console.writeLine(sum(holder));
```

The C declares five variables of that size in `main` (`r`, `big`, `holder`, the match's value, the call's
`Result`). **The binary's frame holds three** (196,640 bytes; clang, `-O2`, the callees not inlined): clang already
merges the two that only pass the value on, which is why ADR 0162 gained nothing.

The generated C was then edited by hand, one change at a time, and each binary measured:

| Change to the C | `main`'s frame |
|---|---|
| none | 3 values |
| no `= {0}` on the three large locals | 3 |
| the binding assigned straight to `big` (no `r`, no temporary) | 3 |
| `big` is the `Result`'s payload, used where it is (no copy out) | 3 |
| `hold` takes a pointer to `big` and not a copy | 3 |
| the last two together | **2** |
| the last two, with one copy out of the `Result` kept | 3 |

So:

- **Three is the `Result`, `big` and `holder`.** clang passes `big` to `hold` without a copy when `big` is a whole
  local that is dead afterwards.
- **Using the payload where it is saves nothing alone**: the value is then a part of the `Result`, and clang copies
  a part of an object to pass it by value (the callee may write its parameter, and the rest of the object is not
  provably dead), so the copy that went comes back at the call.
- **Passing by pointer saves nothing alone**: the copy out of the `Result` is still made.
- **Together they save one of three.** Two is the least this shape can have while a function returns a `Result` by
  value and another returns a `Holder` by value: one object for each.
- `make` itself keeps the value once (65,552 bytes) besides the `Result` it returns into: `Ok(Big { … })` builds the
  `Big`, then the `Result` around it.

### 1.2 What it means

- **The copies that are left are the language's own**: a value returned inside a `Result`, matched out of it, and
  moved into a struct by value. No peephole removes them.
- **Removing one needs two changes at once**, in the calling convention and in how a payload is taken out.
- **The case is narrow**: values of tens of kilobytes held by value. ClayWind had one such type; its two buffers are
  in its own memory now, and its `main` went from 745 KiB to 10 KiB by that change alone.
- **Nothing told the programmer.** The frame was 745 KiB for months on systems whose main thread has 8 MiB; it was
  found on a phone, as a crash with no message. `--explain-frames` reports async task frames only (type-system.md
  §40.6).

## 2. Decision

Three parts. **The first is proposed for acceptance now; the other two are proposed as designs to be accepted or
left, after the first shows how often the case arises.**

### 2.1 Part 1: the report (proposed now)

- **`ceesafec build --explain-frames` also lists sync functions**: for each function of the package whose locals
  and temporaries of 4 KiB or more add up to 64 KiB or more, its name, that sum, and each such local with its type
  and size, largest first. The sum is an **upper bound**, and the report says so: the C compiler may merge some
  (§1.1). The libraries' functions are left out, as in the other reports.
- **A warning, N04xx, on the same condition at 256 KiB**: "this function's locals add up to N KiB: … (the largest:
  `masks`, a `Raster` of 123 KiB). A main thread's stack is 1 MiB on iOS and Windows, and a thread's is smaller on
  some systems." With `--deny-warnings` it is an error, as other warnings are (ADR 0073 §2.4). The two thresholds
  are constants of the compiler, not options.
- It changes no generated code. It would have named ClayWind's `main` on the first build after `Raster` grew.

### 2.2 Part 2: a large value that is moved into a call is passed where it is (a design, not yet proposed for
implementation)

- A by-value parameter whose type is larger than a threshold (the report's 4 KiB) is, in the C, a pointer to the
  value. Where the argument is **moved** and its place is a whole local or a part of one that is dead afterwards
  (the move checker knows), the caller passes that place's address and makes no copy. Where the argument is
  **copied** (a Copy type the caller goes on using), the caller makes the copy, as the C compiler does now.
- The callee uses the pointee as its parameter, and may write it: the caller no longer has the value.
- It applies to CeeSafe's own functions only. An `extern "C"` function, an `export`, a function whose address is
  taken, a closure's call through its environment, a `dyn` method and an async fn's frame each have a C signature
  that something else relies on (c-backend.md §2, §28); each is either left as it is or given a thunk, and saying
  which is this part's main work.

### 2.3 Part 3: a matched payload is used where it is (a design, not yet proposed for implementation)

- Where a `match`, an `if let` or `!`/`?` moves the payload out of an enum value that is a temporary with no other
  use after its discriminant is read, the binding, and a local the binding is then moved to whole, **name the
  payload's place** in that temporary; no copy is made, and the temporary lives as long as they do.
- Only when every assignment of the binding and of that local is such a move from one temporary: a local assigned
  in two arms from two values keeps its own storage.
- A debugger shows the local at the payload's address; `--dump-nir` shows the NIR before the change.

### 2.4 Order and gates

1. **Part 1**, alone: a release with the report and the warning.
2. **A survey with it**: `--explain-frames` over CeeSafe's own compiler, ClayWind, CeeSafe.Web, the HTTP client and
   the chat client; how many functions it names, and how many of them have this shape.
3. **Parts 2 and 3 together or not at all** (§1.1: neither gains alone), decided on the survey. If accepted, a
   prototype first, measured on the binary of §1.1's program and of ClayWind's host at `7315bac`, before the
   corpus's snapshots are touched: the expected result is three values to two, and 745 KiB to about 500 KiB.

## 3. Alternatives considered

- **Temporary forwarding** (ADR 0162, withdrawn): removes what clang already removes.
- **Coalescing by liveness** (ADR 0162 §3): merges locals that are never live together. In §1.1's program `r`, the
  temporary and `big` are already one slot in the binary; what is left is not three locals but a `Result`, a local
  and a struct, which are live together or differ in type. It would not reach two.
- **Returning large values through a pointer to where they are going** (so that `make` writes its `Big` straight
  into `big`). The value is inside a `Result`, whose other variant is not a `Big`; the destination would have to be
  a `Result`. It is Part 3 seen from the callee.
- **Only the calling convention, or only the payload.** Measured: no gain (§1.1).
- **A larger stack**: what ClayWind's packager asks for on iOS. It is the right answer for an app, and no answer
  for a library's caller on a thread whose stack it did not choose.
- **Doing nothing in the compiler.** Defensible for Parts 2 and 3: the remedy in a program is one line (keep the
  large part behind a pointer or in an allocator's memory), and Part 1 says where.

## 4. Consequences

- **Part 1**: one more thing `--explain-frames` prints, and a warning that a few programs will see. A function that
  holds a large buffer on purpose (`var buffer = [0u8; 1048576];`) is told so, and silences it by putting the
  buffer on the heap, or accepts the warning; there is no suppression syntax.
- **Parts 2 and 3**, if taken: a change to the C of every call with a large by-value argument, new cases in the
  backend for each kind of function a pointer does not fit, and an optimisation more to keep correct
  (`optimisations.md` §1), for one value fewer in frames of this shape.
- **Not covered**: the copy inside the callee (`make` building its `Big` before its `Result`); the zero-filling of
  large locals at a function's start; async task frames, which `--explain-frames` already reports.

## 5. What an implementation must show

- **Part 1**: the report for §1.1's program (three locals of 64 KiB, one upper bound of five); the warning's text
  and its absence below the threshold; that the libraries' functions are not reported; the survey of §2.4.
- **Parts 2 and 3**: the frames of §1.1's program and of ClayWind's host, **read from the binary at `-O2`**, before
  and after; the corpus natively, interpreted and with the change switched off; the fixed point and the release
  chain; and the number of functions whose C signature changed.
