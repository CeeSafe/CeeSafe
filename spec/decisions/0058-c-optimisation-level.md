# ADR 0058 — The generated C compiled at `-O3`

- Status: **Withdrawn** by the owner, 2026-11-03, at the gate of §3.2 ("Lets stop it there. I don't want to land
  slow."): `-O2` stays the default. It had been accepted earlier the same day ("I accept both ADRs with your
  recommendations"), subject to that gate, and §7 is what the gate found. `--cflag -O3` remains available to anyone
  who wants it. From the independent benchmark's analysis (pasted by the owner): the optimisation
  level of the generated C is the largest gap between CeeSafe's default build and Rust's release build.
- Scope: `ceesafec build` (src/host/build.cee, src/host/toolchain.cee), tooling.md §8, the guide's performance page,
  the fixed point's record. No language, runtime or library change.

## 1. Context

`ceesafec build` compiles its C at `-O2` with gcc and clang (`src/host/build.cee:158`, `src/host/toolchain.cee:145`)
and at `/O2` with MSVC, which has nothing above it. `-g` builds use `-O0`. A user's `--cflag -O3` comes after ours, so
it wins. Rust's release profile is `opt-level=3`, LLVM's `-O3`. The benchmark (`benchmarks/independent/`) built
every workload both ways:

| Workload | M1, Apple clang 21: `-O2` → `-O3` | Linux VM, clang 18: `-O2` → `-O3` |
|---|---|---|
| matmul | 1158 → 333 ms (0.29) | 1512 → 688 ms (0.46) |
| nbody | 1057 → 565 ms (0.53) | 1233 → 1198 ms (0.97) |
| spectral-norm | 827 → 627 ms (0.76) | 3231 → 1322 ms (0.41) |
| text-parse | 715 → 617 ms (0.86) | 1152 → 996 ms (0.86) |
| hashmap-same | 682 → 595 ms (0.87) | 1071 → 1045 ms (0.98) |
| **dyn-dispatch** | **417 → 726 ms (1.74)** | 1109 → 1082 ms (0.98) |
| the other seven | 0.97–1.03 | 0.90–1.03 |
| **geometric mean, × `rust-checked`** | **1.43 → 1.24** | **1.42 → 1.20** |

What `-O3` adds is chiefly vectorisation (`matmul`, `nbody` on arm64) and more inlining: at `-O2`, clang 18 leaves
`spectral-norm`'s small helper as a call in the hot loop.

**One regression.** `dyn-dispatch` (200 M calls through `Box<dyn Shape>`) is 1.74× slower at `-O3` with Apple clang 21.
That was re-measured on 2026-11-03: 0.41 s against 0.74 s, three runs each. It does not happen with clang 18 on Linux.
The calls go through the table's thunks as at `-O2`, and the cause is not yet known.

**Costs, measured 2026-11-03 on the M1:**

- ceesafec builds itself in 44.4 s at `-O3` against 42.1 s at `-O2` (+5%), and its binary is 5793 KiB against
  5497 KiB (+5%). The thirteen benchmark programs compile in 5.0 s against 4.9 s, with binaries of 53 KiB either way.
- ceesafec at `-O3` is no faster at its own work: it writes its own C in 4.8 s at either level.
- Floating point is unchanged: `-ffp-contract=off` stays, and neither level relaxes IEEE semantics. Every
  checksum of the benchmark agreed across all five configurations.

## 2. Options

| | A. `-O3` by default (proposed) | B. Keep `-O2`, document `--cflag -O3` | C. A `--release` flag for `-O3` |
|---|---|---|---|
| Default build | 1.24× / 1.20× Rust (geometric mean) | 1.43× / 1.42× | 1.43× unless asked |
| Cost | +5% C compile time and binary size for large programs | none | a flag to learn; two kinds of build to test |
| Risk | the `dyn-dispatch` regression on Apple clang (§3.2) | none | as A, when asked |

**A is proposed**, with the regression gated (§3.2). `ceesafec build` is CeeSafe's release build: there is already
`-g` for debugging, and a user who wants `-O2` passes `--cflag -O2`.

## 3. Proposed decision

### 3.1 The level

- gcc and clang: `-O3` in place of `-O2` for `ceesafec build`, `--emit exe` and `--emit obj`, hosted and freestanding.
  `-g` keeps `-O0`. `--cflag` still comes last, so `--cflag -O2` restores today's build.
- MSVC: `/O2` stays, as its highest level.
- The release workflow builds ceesafec itself at `-O3` too (`.github/workflows/release.yml` seed builds), so the
  compiler is built the way it builds programs. The fixed point's record (`docs/p6-fixed-point.md`) states the flags
  and is re-recorded. Its C is unchanged, since the level does not change what ceesafec writes.

### 3.2 The gate

Before the switch, the `dyn-dispatch` regression is explained, and either removed in the C ceesafec writes or bounded
on the toolchains CeeSafe releases for. The acceptance check is the benchmark at `-O2` and `-O3` on macOS arm64, Linux
x86-64 (gcc and clang) and Windows (MSVC unchanged). No workload may be slower at `-O3` than its interquartile spread
allows. If the cause is Apple clang's alone and cannot be steered from the C, this ADR comes back to the owner with the
measurement before anything changes.

## 4. Consequences

- The default build gains about 15% in the geometric mean, and much more on loop-heavy numeric code.
- Builds of large programs take about 5% longer in the C compiler, and their binaries grow about 5%.
- Performance numbers in `guide/performance.md` and `benchmarks/README.md` (`benchmarks/run.py` builds at `-O2`
  through `--cc clang`) are re-measured and restated.
- New diagnostics: none.

## 5. Questions for the owner

1. `-O3` as `ceesafec build`'s default with gcc and clang, `/O2` with MSVC, `-O0` with `-g`? Recommended: yes.
2. The gate of §3.2: the switch waits until the `dyn-dispatch` regression is explained and no workload regresses?
   Recommended: yes.
3. ceesafec itself built at `-O3` in the release workflow, with the fixed point re-recorded? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Find the `dyn-dispatch` regression's cause (the loop's assembly at both levels with Apple clang 21 and clang 18),
   and change the emitted C if that removes it.
2. `src/host/build.cee`, `src/host/toolchain.cee`: `-O3`; tooling.md §8; the guide.
3. The release workflow's seed builds; `docs/p6-fixed-point.md` re-recorded; `bootstrap/reference.md` if it names a
   level.
4. The benchmark at both levels on the three platforms (§3.2), recorded in `benchmarks/independent/README.md`; the
   contract suite.
5. ROADMAP §10.

## 7. The gate's finding (2026-11-03)

`dyn-dispatch`'s slowdown is not caused by `-O3`. It comes from where the code lands in memory, and `-O2` builds land
just as badly.

- **The same code at both levels.** The generated C compiled with Apple clang 21 at `-O2` and at `-O3` gives the same
  hot loop, instruction for instruction (`ldp`, `ldr` of the table entry, `blr`, `fadd`), and the same thunks and
  `area` methods. Only `main`'s cold paths differ, by 12 lines of assembly, which moves the code after them.
- **The time follows the layout, not the level.** The same C was built several ways and timed, best of 3:

  | Build | Hot loop's `ldr` (address mod 64) | Time |
  |---|---|---|
  | `ceesafec build` (`-O2`) | 12 | 0.44 s |
  | `ceesafec build --cflag -O3` | 36 | 0.70 s |
  | clang `-O2`, all files on one command line | 4 | 0.96 s |
  | clang `-O3`, the same | 44 | 0.92 s |
  | clang `-O2 -falign-loops=64` | 4 | 0.45 s |
  | clang `-O3 -falign-loops=64` | 4 | 0.74 s |
  | clang `-O2 -falign-functions=64` | 36 | 1.04 s |
  | clang `-O3 -falign-functions=64` | 60 | 1.03 s |

  The same `-O2` code takes anything from 0.44 s to 1.04 s. Two builds whose loop starts at the same offset (4) take
  0.45 s and 0.96 s, so the loop's alignment alone does not decide it. The loop makes one indirect call per element to
  one of three thunks, and the M1's indirect-branch prediction is sensitive to where the call and its targets sit.
- **Not on Linux.** The benchmark author's clang 18 on x86-64 showed nothing similar: 1109 ms at `-O2` against 1082
  ms at `-O3`.
- **Not steerable from the C.** Nothing in the emitted C decides these addresses. An alignment flag moves the swing
  around but does not remove it: `-falign-loops=64` helps one build and not the other.

So the regression in the table of §1 is one unlucky layout, not something `-O3` does, and `-O2` has unlucky layouts of
its own. §3.2 asked for the cause to be explained and the regression removed or bounded. It is explained, but it
cannot be removed from the C, and it is no worse than what `-O2` already does. As §3.2 requires, the owner decides
whether `-O3` becomes the default with this known, or waits. Nothing has changed in `src/host` yet.

## 8. Outcome (2026-11-03)

The owner kept `-O2` after §7's finding: a default that can land 1.7× slower on a common workload is not taken for a
15% gain elsewhere. Nothing in `src/host`, the release workflow or the fixed point changed. If the C compilers' layout
sensitivity is ever bounded (for example by a layout that does not depend on unrelated code), this can be proposed
again with new measurements.
