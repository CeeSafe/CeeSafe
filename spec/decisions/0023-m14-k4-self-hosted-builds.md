# ADR 0023 — M14 K4: building a reference kernel where the reference compiler does not run

> **Inherited.** ADR 0023 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29, taking every recommendation in §5). The wording is the agent's.
- Scope: M14 K4, OD-31. CosmoOS's roadmap 2.0 makes two choices that conflict once its kernel is the reference. Its rule 7
  stops committing generated C, so every build runs the reference compiler. Its SW0 makes CosmoOS build itself. The reference compiler does not run on
  CosmoOS.

## 1. Context

- **The reference compiler** is a .NET program, distributed as NativeAOT binaries for Linux, macOS and Windows on x64 and arm64
  (`release.md`). NativeAOT needs a .NET runtime port for each operating system, and CosmoOS has none. It runs Linux
  programs through its personality, but not the .NET runtime's full use of Linux: threads, `mmap` and signal-based GC
  suspension (inferred; untested).
- **Today** CosmoOS commits the generated C beside each `.cee` file, so any machine with a C compiler builds the
  kernel. That is what makes a self-hosted build possible now.
- **Generated C is a pure function** of the sources and the reference compiler's version (c-backend §1; CosmoOS checks it in CI).

## 2. Options

| | A. Source releases carry the generated C (proposed) | B. The reference compiler on CosmoOS | C. A reference compiler in the reference | D. Keep committing generated C |
|---|---|---|---|---|
| Self-hosted build | from a release tarball, no the reference compiler | yes | yes | yes |
| Changing a `.cee` file on CosmoOS | needs a host with the reference compiler | yes | yes | needs a host with the reference compiler |
| Cost | a `make dist` target and a CI check | a .NET runtime port, or the Linux personality running .NET | years | the tree doubles as the kernel becomes the reference |

## 3. Proposed decision: A

- The repository holds `.cee` sources only, as roadmap 2.0's rule 7 says. `make dist` (CosmoOS) writes a source
  tarball with the generated C beside each `.cee` file, made by the pinned the reference compiler. A build from the tarball runs no
  The reference compiler.
- CI checks that the tarball builds without the reference compiler on both architectures, and that its C matches a fresh generation
  (the reproducibility check rule 7 already plans).
- A self-hosted CosmoOS builds its kernel from a release tarball. Developing the kernel's CeeSafe code on CosmoOS itself
  needs a machine with the reference compiler until B or C exists. Both stay open as later decisions; C is the long-term direction for a
  self-hosting the reference.
- The reference needs nothing new for A: `ceesafec build --emit c` already writes the files, with ADR 0022's `--split` layout when
  the kernel uses it.

## 4. Consequences

- SW0's self-hosting acceptance builds the kernel from the release tarball.
- CosmoOS's roadmap 2.0 and `docs/build/ceesafe.md` record the choice (a CosmoOS PR once this is accepted).

## 5. Questions for the owner

1. Option A? (Recommended.)
2. Record B (the reference compiler on CosmoOS) and C (a reference compiler written in the reference) as open decisions for later? (Recommended.)
