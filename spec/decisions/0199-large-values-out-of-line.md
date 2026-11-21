# ADR 0199 — A function that takes or returns a large value by value is kept out of line

- Status: **Accepted** by the owner, 2026-11-21 ("Yes, I accept the ADR. Answer to your questions: 1. 1 Kib=yes,
  2. yes"), after ClayWind's CEESAFE-FINDINGS.md Q4 ("CeeSafe fix instead"); implemented (§6).
- Scope: the C backend's function definitions and prototypes (`src/backend`), `rt.h`'s `CEE_NOINLINE`, the attribute
  `@outOfLine` (`src/check`), `--explain-frames`, c-backend.md §4.2, appendix A, corpus cases. No library or runtime
  behaviour changes.

## 1. Context

0.84 writes a call's result straight into its destination instead of through a temporary (`t_2 = f();` where 0.83
wrote `t_1 = f(); t_2 = t_1;`). That removes a copy and is the better code. For ClayWind it had a cost no test saw:
its `Style` is a 5,696-byte struct whose builders (`Style.box().w(...).h(...).bg(...)`) each take one by value and
return one, and without the temporary each builder fell below the inlining threshold of Apple clang 17 (Xcode 16,
GitHub's macos-15 runner) and of the Windows runner's LLVM. Inlined, every builder call copies whole `Style`s at its
call site, and a UI function makes dozens of such calls.

Measured on GitHub's macos-15 runner, Apple clang 17.0.0, ClayWind's headless host's `main.c` compiled alone with
ceesafec's release flags (ClayWind's `tools/compare_codegen.sh`, 2026-11-21):

| The generated C | Code | Compile |
|---|---|---|
| 0.78.0's | 655,601 bytes | 18 s |
| 0.84.1's | 1,663,009 bytes | 53 s |
| 0.84.1's, `-fno-inline-functions` | 488,185 bytes | 4 s |
| 0.84.1's, the 113 functions that take or return a `Style` by value `noinline` | 583,801 bytes | 9 s |

The whole binaries, stripped: macOS 0.76 MB with 0.78.0 and 1.78 MB with 0.84.1; Windows 1.06 and 2.24 MB; ClayWind
had to raise its CI's size and build-time budgets. Apple clang 21 and upstream clang 17 do not inline those builders,
but the rule helps there too: with Apple clang 21 the same `noinline` takes the code from 846,752 to 538,501 bytes and
the compile from 17 s to 8 s, since even where a builder is not inlined, its callers' frames hold fewer copies.

No CeeSafe attribute keeps a function out of line (`@inline` is N0290's example of an unknown attribute), and a
library cannot fix this for its users: the inlining is chosen per call site by the C compiler.

## 2. Decision

1. **The rule.** The C backend marks a function never inlined when one of its parameters (the receiver included) or
   its result is passed by value and is an aggregate (a struct, an enum with a payload, an optional or a result, an
   array) whose size (`cLayout`) is more than **1,024 bytes**. A `borrow` parameter is a pointer and does not count.
   The mark goes on its definition and its prototype: `__attribute__((noinline))` for clang and gcc,
   `__declspec(noinline)` for `cl`, behind one macro in `rt.h`, `CEE_NOINLINE`.
2. **What it leaves alone.** Functions with only smaller values stay as they are, so a struct of a few hundred bytes
   (ClayWind's `Decl` of 384 and `Props` of 476) is inlined as the C compiler chooses. `@export`ed functions, extern
   functions and a closure's body follow the same rule (an export is called from C; marking it changes nothing there).
   A debug build (`-g -O0`) inlines nothing anyway; the mark is written all the same, so the C is the same text.
3. **The threshold.** 1,024 bytes is where a by-value copy costs about as much as a call: a copy of that size is a
   `memcpy` call or a long run of moves in either case. It is a constant of the backend, documented in c-backend.md,
   not a flag. A program can see which of its functions it marks: ceesafec build `--explain-frames` (ADR 0163) lists
   them with their sizes.

## 3. Consequences

- ClayWind's hosts are smaller than with 0.78.0 on every compiler measured and build several times faster on the
  runners; its budgets go back to their old values.
- A program whose hot path passes large values by value loses the C compiler's option to inline that path. Its
  remedy is the one the language already prefers, a `borrow`: the mark does not apply to a pointer.
- The generated C changes for every function past the threshold, so emit-C snapshots that contain one are
  regenerated.

## 4. Implementation

- `src/backend`: where a function's prototype and definition are written, `isOutOfLine(c, function)` (its
  parameters' and result's by-value `cLayout` sizes against 1,024) adds `CEE_NOINLINE `; `rt.h` defines it for clang,
  gcc and `cl` (and empty elsewhere).
- `--explain-frames` lists the functions kept out of line.
- Tests: a run-pass case whose builder on a 2 KiB struct runs as before; an emit-C check that the mark is on a 2 KiB
  struct's builder and not on a 512-byte one's; ClayWind's `tools/compare_codegen.sh` on the macos-15 runner, before
  and after, in the PR.

## 5. Questions answered by the owner

1. The threshold: **1,024 bytes**.
2. An explicit attribute as well: **yes**. It is `@outOfLine`, on a function or a method, with no arguments; a
   function so marked is kept out of line whatever its values.

## 6. As built

- `src/backend/c_body.cee`: `isOutOfLine` (the attribute, or a by-value parameter or result whose `cLayout` is over
  `OUT_OF_LINE_BYTES`, 1,024), `CEE_NOINLINE ` on the definition (`emitFunction`) and the prototype
  (`c_skeleton.cee`); `explainOutOfLine` for `--explain-frames`. Async functions are left as they are: their frames
  are the task's, not the C stack's.
- `src/check`: `@outOfLine` is known on functions, methods and an enum's methods, and takes no arguments (N0290).
- Corpus: `emit-c/adr0199-out-of-line` (the mark on a 2 KiB struct's builders, a function returning one and one
  taking one, and on an `@outOfLine` function; not on a 512-byte struct's or a `borrow`'s), `run-pass` the same
  program, `frames/adr0199-out-of-line` the listing, `check-fail/adr0199-out-of-line` (arguments; a struct).
  `frames/large-values`'s snapshot gains its three functions that return 64 KiB values: an intended change.
- ClayWind's headless host, stripped, macOS arm64: 989,312 bytes with 0.84.1, 675,648 with this rule (Apple clang
  21); 855,144 and 706,640 with upstream clang 17. 127 of its functions are kept out of line.
