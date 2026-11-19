# ADR 0179 — Target conditions and configured C blocks and statics

- Status: **Accepted** by the owner, 2026-11-19, for the target conditions (asked about CeeUsb's findings F3 and F5:
  "Yes, both as described"). The configured blocks and statics (finding F6), proposed 2026-11-19 under the owner's
  instruction to fix every finding ("Now we focus on CeeSafe and fix all findings"), **accepted** by the owner
  2026-11-20 ("Accept all four"). Implemented 2026-11-19; released in 0.79.0.
- Scope: type-system.md §28.4 (configured code), §39.4 (C statics); c-backend.md §5; the interpreter's `cfg`. Findings
  F3 and F6 of CeeUsb's `CEESAFE_FINDINGS.md`.

## 1. Context

CeeUsb, a USB library with a back end per system, chose each back end with `if (cfg("__linux__"))`,
`cfg("__APPLE__")` and `cfg("_WIN32")`. Three problems:

- **F3.** A compiler's own macros are not one meaning: Android defines `__linux__`, iOS `__APPLE__`, and the macro for
  "macOS, not iOS" needs `<TargetConditionals.h>`, which nothing included. The interpreter took every `cfg` as false,
  so `run --interp` took no back end at all.
- **F6.** `@cfg` was refused on a C static, and IOKit's constants (`kIOMasterPortDefault`, `kCFRunLoopDefaultMode`)
  are statics. Their header exists only on Apple systems, and `@header` on a block was unconditional, so a block that
  named `<IOKit/IOKitLib.h>` broke every other build. CeeUsb read them through C shims in `native.c`.
- **Also found** while doing this: an `await` inside a configured block left its resume label inside `#if`, while the
  async dispatch's `case` that jumps to it was outside: an undeclared label in every build without the block.

## 2. Decision

1. **Target conditions.** `cfg("target.linux")`, `target.android`, `target.macos`, `target.ios`, `target.windows`,
   `target.freebsd` and `target.unix`, each also with `!`, wherever a `cfg` name is allowed. They are compiled to the
   test each C compiler agrees on (type-system.md §28.4 lists them); `rt.h` includes `<TargetConditionals.h>` on Apple
   systems. Another `target.` name is N0292 with the list; a compiler macro that a target condition replaces is N0292
   naming it. Inside a block of any Unix target, code configured for `target.unix` may be used.
2. **The interpreter** answers a target condition for its own host (macOS, Linux, Windows or FreeBSD), so the program
   it runs takes the branch the host's build would. Other macros stay 0.
3. **Configured blocks.** `@cfg("NAME")` on an `extern "C"` block configures its functions and statics and wraps its
   `@header` includes in `#if`, in the generated C and in `ceesafe_exports.h`. Headers are kept per (path, condition).
   Such a block declares only functions and statics (N0290 otherwise), and its members carry no `@cfg` of their own.
4. **Configured statics.** `@cfg` on a C static (or its block's) makes it configured code: used only inside a block of
   its condition (N0350). It is read-only (a configured `static mut` is N0290). Its address is taken at the use,
   inside the `#if`, not hoisted to the function's start where every build would see it.
5. **Async dispatch.** A suspension point inside a configured block has its `case` inside the same `#if`.

## 3. Options not taken

- **A `target_os` value** (`if (Target.os() == Os.Linux)`): run time, so every build would compile every back end's
  calls, and Linux's C would need IOKit's declarations.
- **Teaching every compiler macro** (`cfg("__linux__")` meaning "Linux, not Android"): it would silently change what
  C's `#if __linux__` means in CeeSafe, against §28.4's rule that `cfg` is C's `#if`.
- **Configured structs and constants in a block**: their checks (`_Generic`, `_Static_assert`) would need the same
  wrapping and a story for uses outside configured code. Not needed by any finding; left out.

## 4. Implementation and tests (2026-11-19)

- Checker: `checkCfgName` (src/check/body.cee) for every `cfg` name; `cfgImplies` for `target.unix`; block `@cfg` in
  src/check/ffi.cee; `HirStatic.cfgName`; the use of a configured static is `Deref(StaticAddress)`. Back end:
  `appendCfgTest`'s target tests (c_interop.cee), configured includes (c_skeleton.cee, the exports header), the
  dispatch (c_async.cee). Interpreter: `hostTargetHolds`.
- Corpus: `run-pass/adr0179-target-conditions.cee`, `run-pass/adr0179-configured-await.cee` (also `--flat-c`, and
  interpreted), `check-fail/adr0179-configured-c.cee` (N0290, N0292, N0200, N0350). Full corpus on macOS: every
  family passes (2026-11-19, `tools/run_corpus.py`); run-pass interpreted 419/419.
- A package reading `kCFRunLoopDefaultMode` through a configured block, linked with ADR 0184's frameworks: printed
  `macos/true/unix` on macOS (arm64) and `linux/true/unix` on Ubuntu 24.04 (x86-64, gcc 13).
