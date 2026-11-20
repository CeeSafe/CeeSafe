# ADR 0192 — 32-bit targets: Android's armeabi-v7a, and 32-bit Linux to test them

- Status: **Accepted** by the owner, 2026-11-20 ("I accept ADR 0187 and ADR 0188 as written"; this ADR was written as
  0187, renumbered 0191 because main's ADR 0187, library package builds, was accepted first, and 0192 because main's
  ADR 0191, a busy executor, was released first). **Implemented** in 0.84.0. As built: a narrowing `as` that does not
  fit traps with its own runtime panic kind, `CEE_PANIC_NARROWING` (20; 19 is ADR 0186's `CEE_PANIC_OVERRAN`), which
  the language reports as `PanicKind.Other`; the 32-bit Linux job (`.github/workflows/linux32.yml`, i386 with
  `-msse2 -mfpmath=sse` so floats round as on the 64-bit targets) runs on release tags and by hand, the corpus without
  its six 64-bit-only programs (`tools/run_corpus.py --exclude`); the runtime builds without `-Wshorten-64-to-32`
  warnings, and `Process.capture`'s argument array has a `usize` length. IPos runs on the Sunmi V2 (Android 7.1.1,
  armeabi-v7a): sign-in against the production API, invoice search, Bluetooth printing.
  Asked for by the owner ("Start the CeeSafe work for the V2"), to run IPos on a
  Sunmi V2: a handheld till with a printer built in, whose processor runs 32-bit ARM code only (`armeabi-v7a`, checked
  over adb: `ro.product.cpu.abilist=armeabi-v7a,armeabi`) on Android 7.1.1. Its Android-release half is ADR 0188.
- Scope: the compiler's target (a new `--arch` option beside ADR 0171's `--target`), the type table's and the C
  backend's layouts, the C emitted for lengths and for `as`, the async frames' layouts, the runtime's few word-size
  assumptions, the specification's "64 on every supported target"; claywind-pack (ClayWind's repository) passes the
  ABI it builds for. No new syntax; one rule for `as` on 32-bit targets (2.4).

## 1. Context

- **What fails.** A ClayWind app built for `armeabi-v7a` (claywind-pack's `--abi armeabi-v7a`, the NDK's
  `armv7a-linux-androideabi` clang) stops at the generated C's first line, `_Static_assert(sizeof(void *) * 8 == 64,
  "ceesafec: generated for a 64-bit target")`, and at every layout assert after it (`sizeof(Window) == 8`,
  `sizeof(claywind.Raster) == 504`…): ceesafec lays types out for 64-bit pointers only.
- **What is there already.** The type table is made from a `TargetInfo { pointerBits, longBits }`
  (`src/types/types.cee`); `isize` and `usize` take `pointerBits`, and `Target.pointerBits()` folds from it. But
  `targetFor` always gives 64, and the layouts write 8 for every pointer: references, slices (16), boxes, raw and
  function pointers, closures, threads, vectors (24), the regions, the async frames' runtime parts (`c_async.cee`); the
  C backend's `cLayout` repeats the table. Slices', vectors' and `cee_str`'s lengths are `uint64_t` in the C. The
  specification says `usize` is "pointer width of the target (64 on every supported target)" (type-system.md §1).
- **The runtime.** Its arena, buffer and pool asserts are already under `#if UINTPTR_MAX == UINT64_MAX`; `__int128` has
  a 32-bit fallback; the window runtime compiled for armv7 when ADR 0157 checked it. Left: 64-bit atomics on `uint64_t`
  (ARMv7-A has `LDREXD`/`STREXD`, so clang inlines them; to be confirmed, with `-latomic` if not), and the leak scanner
  of `ceesafec test`, which steps 8 bytes over memory where pointers are 4 apart (`ceesafe_rt_core.c`).
- **The interpreter** (`run --interp`, `test --interp`) runs on the host, which is 64-bit; it is not touched.

## 2. Decision

### 2.1 Choosing the width: `--arch`

`ceesafec build` (and `check`, so the checks are the target's) takes `--arch <name>`: `arm64`, `x86_64` (64-bit, as
today), `armv7` and `x86` (32-bit). It names the processor, as `--target` names the system (ADR 0171 §3.2 stays: one
option, one question). Without it the arch is the host's. A 32-bit arch is accepted with `--target android`, and with
`--target linux` for tests (2.6); elsewhere it is an error (N-code to be given): there is nothing to run it on.
`TargetInfo` gains the arch; `targetFor` gives `pointerBits: 32` for `armv7` and `x86`.

claywind-pack passes `--arch` with each ABI it builds: `arm64-v8a` → `arm64`, `armeabi-v7a` → `armv7`, `x86_64`,
`x86`.

### 2.2 Layouts from the pointer width

Every fixed 8 in `layoutOf` and `cLayout` becomes the target's pointer size (P: 4 or 8), and every 16 or 24 that is
"pointer + length" or "pointer + length + capacity" becomes 2 or 3 words of P: a slice is (P, P) ×2, a `Box` of `dyn`
two pointers, a `Vector` three words, `StableVector` and `SlotMap` and the regions' records their fields' sum. The async
frames' runtime parts (`scopeLayout`, `taskSlotLayout`, `blockingLayout`, the boxed frame's pointer) are computed from
P with the runtime's structs' layouts, and the runtime's asserts check them (they are `<=` today; they stay so).
The alignment of `i64`, `u64` and `f64` is the arch's: 8 on `armv7` (AAPCS) as on the 64-bit arches, 4 inside
structs on `x86` (the i386 System V ABI), so that the C compiler's own layout and the asserts agree without flags that
would change the ABI against the system's libraries.

### 2.3 Lengths are `usize`

A slice's, a vector's and a string's length (and a vector's capacity) are `usize` in the language already; the C makes
them `cee_usize` (a typedef the preamble makes `uint32_t` or `uint64_t` from P) instead of `uint64_t`, in the four
struct emitters, `cee_str` and the helpers that take or give a length. On 64-bit targets the generated C is the same
types under another name. The preamble's assert becomes `sizeof(void *) * 8 == P * 8`, with P the target's.

### 2.4 `as` on a 32-bit target

`as` converts only without loss (ADR 0074), which until now was the same answer everywhere: `u64 as usize` and
`i64 as isize` were lossless because `usize` was 64 bits. The check stays the same on every target, so that a program
that checks on one checks on all (some 6,000 `as usize` in the compiler and lib alone). On a 32-bit target, a
conversion that is lossless on 64-bit but narrows on 32-bit (`u64` or `i64` to `usize` or `isize`) is emitted as a
checked conversion: a value that does not fit stops the program, as an index
out of range does, with "conversion to usize loses information on this 32-bit target". `as` never loses information:
it either is exact or stops. `checkedTruncate<usize>` and `truncate<usize>` mean what they say on every target.

### 2.5 The runtime

- The leak scanner steps by `sizeof(uintptr_t)`.
- 64-bit atomics: built for armv7 and x86 with the suite's flags; where clang does not inline them, the runtime links
  `-latomic` (the NDK and Linux both have it).
- Opaque records sized in `u64` words (async, channels) stay as they are: larger than needed on 32-bit, checked by the
  runtime's `<=` asserts.
- `cee_target_is_*` gains `cee_target_bits()` for C that wants it.

### 2.6 Tests

- 32-bit Linux (`--target linux --arch x86`, gcc or clang `-m32`) is the test vehicle: the contract suite's build-and-
  run corpus, `ceesafec test`'s own packages, and the emitted-C corpus with a 32-bit variant of each layout assert, run
  in CI on Linux. It is not a release target.
- Android armv7: the window checks and ADR 0188's API level 25 image (its 32-bit ARM ABI where the emulator has it,
  else x86 32-bit), and claywind-pack's `--abi armeabi-v7a` building ClayWind's demo and IPos.
- On the device, by the owner: IPos on the Sunmi V2, a receipt printed on its own printer.

## 3. Options not taken

- **The width from `--cc`'s triple.** The compiler would have to know every C compiler's triple spelling, and `check`
  has no C compiler. An option says what it means.
- **`--target android-armv7`.** ADR 0171 made `--target` the system and the manifests' `[target.X]` tables key on it;
  folding the processor into it would multiply those tables.
- **Lengths left `uint64_t`.** Smaller change, but a slice would be 16 bytes on a 32-bit machine and every `.length`
  would convert between widths in the C.
- **`as` checked per target** (`u64 as usize` an error on 32-bit). The same source would check on one target and not
  another, and the compiler and library would need thousands of edits to build for 32-bit at all.
- **Only Android armv7, tested on the device.** The suite would not run on 32-bit, and every later change could break
  it unseen.

## 4. Consequences

- ceesafec builds for 32-bit Android; with ADR 0188, IPos runs on the Sunmi V2.
- The specification says `usize` is the target's pointer width, 32 or 64; type-system.md §1, §26 (`Target.pointerBits`),
  the LP64 notes on `Box`, `Vector` and raw pointers, c-backend.md's preamble, and the guide's basics are updated;
  `type-system.md`'s "ceesafec has no target option" (out of date since ADR 0171) goes.
- The CI gains a 32-bit Linux job.
- A program's 32-bit build can stop where its 64-bit build did not, at an `as usize` of a value over 4 GiB: the
  message says why.

## 5. Order of work

1. `--arch` and `TargetInfo` (2.1); layouts from P (2.2); `cee_usize` (2.3); the preamble.
2. Checked narrowing `as` on 32-bit (2.4).
3. The runtime (2.5); 32-bit Linux in CI with the suite (2.6).
4. claywind-pack's `--arch`; Android armv7 (with ADR 0188's floor); IPos on the Sunmi V2.
