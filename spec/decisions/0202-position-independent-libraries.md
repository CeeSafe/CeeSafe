# ADR 0202 — `--emit lib` compiles position-independent code on ELF systems

- Status: **Accepted** by the owner, 2026-11-21 ("I accept ADRs 0201 and 0202"); implemented (§5). Found by
  ClayWind (CEESAFE-FINDINGS.md Q3).
- Scope: `ceesafec build --emit lib`'s C flags (`src/host`), tooling.md, embed_check. No language, library or runtime
  change.

## 1. Context

`--emit lib` (ADR 0088) makes a static archive for a host program. Its objects are compiled with the C compiler's
default code model, which on Linux, FreeBSD and Android is position-dependent unless the compiler was built to default
to PIE (Ubuntu's gcc is; Fedora's gcc and clang are not), and the runtime's thread-locals then use the local-exec model.
Such an archive goes only into a position-dependent executable. Two of a host's usual links are not:

- **A shared library**: a JNI library (ClayWind's Kotlin binding: "relocation R_X86_64_TPOFF32 against
  `cee_budget_now' can not be used when making a shared object; recompile with -fPIC", CI's Ubuntu 24.04), a Python
  extension, a plugin.
- **A PIE executable**: Rust links one by default ("relocation R_X86_64_32S against `.rodata' can not be used when
  making a PIE object; recompile with -fPIE", the owner's Fedora 45 PC), as does every Linux distribution's default
  gcc build of a program.

The remedy today is `--cflag -fPIC` on every such build, which a host has to know about.

## 2. Decision

1. On ELF systems (Linux, FreeBSD, Android), `--emit lib` compiles with `-fPIC`, so its archive goes into an executable,
   a PIE or a shared library alike. Thread-locals then take the general-dynamic model, which is what a shared library
   loaded with `dlopen` needs.
2. macOS and iOS (Mach-O, position-independent always) and Windows (COFF) are unchanged.
3. `--emit obj` (ADR 0121) is unchanged: its object goes into a kernel or a firmware image as often as into a program,
   and its user gives the code model. `--emit exe` is unchanged: the executable is ceesafec's own link.
4. A `--cflag` the user gives comes after it, so `--cflag -fno-pic` still wins.

## 3. Consequences

- ClayWind's bindings build on Linux without `--cflag -fPIC`; its CI and scripts drop it.
- A position-independent archive is a little slower on x86-64 in a position-dependent executable (a GOT load for a
  global's address, and general-dynamic thread-locals: a call where local-exec was an offset). The runtime's hot
  thread-locals (a task's budget, the poll word) are read through one pointer each per call; benchmarks/run.py
  measures it before the release.

## 4. Implementation

- `src/host/build.cee`: `-fPIC` among `--emit lib`'s compile flags for clang and gcc on ELF targets (the target as the
  build is for: `--target android`, or the host).
- tooling.md (`--emit lib`); embed_check: the archive linked into a shared library and into a PIE on Linux.
- ClayWind: CI's Kotlin step and its scripts without `--cflag -fPIC`; the Rust and Kotlin bindings on the Fedora PC.

## 5. As built

- `src/host/build.cee` (`compileIn`): `-fPIC` for an archive build with clang or gcc when `manifestTarget()` is
  `linux`, `freebsd` or `android`, before the user's `--cflag`s. tooling.md §8 (`--emit lib`).
- embed_check on Linux: the archive linked into a PIE (`-fPIE -pie`) and into a shared library (`-shared`, the whole
  archive): 86 passed on Fedora 45 with clang 23. Its host fixture asks glibc for `strdup` and `usleep`
  (`_POSIX_C_SOURCE`, `_DEFAULT_SOURCE`), which `-std=c11` hid there.
- ClayWind's bindings on Fedora 45 without `--cflag -fPIC`: the Rust example (a PIE) and the Kotlin JNI library link.
- Not measured yet: benchmarks/run.py with a position-independent archive (the ADR's §3); none of its programs is
  an archive build, so their numbers do not change.
