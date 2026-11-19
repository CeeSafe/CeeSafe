# ADR 0184 — Apple frameworks in the manifest

- Status: **Accepted** by the owner, 2026-11-20 (asked whether to accept the designs proposed under "fix all findings": "Accept all four"); proposed 2026-11-19, done under the owner's instruction to fix every finding of CeeUsb ("Now we focus on CeeSafe and fix all findings"). Implemented 2026-11-19; released
  in 0.79.0.
- Scope: tooling.md §4.1 (the manifest's system libraries). Finding F2.

## 1. Context

A macOS back end links frameworks (`-framework IOKit -framework CoreFoundation`). `links` names libraries (`-lNAME`),
and a framework is not one. CeeUsb's users had to pass `--cflag -framework --cflag IOKit` on every build.

## 2. Decision

- `[target.macos]` and `[target.ios]` take `frameworks = ["IOKit", "CoreFoundation"]`, named as `links` are. They
  reach the executable's link as `-framework NAME`, from every package in the build, each once.
- `frameworks` in `[package]` or another system's table is N0321, naming the two tables: only Apple's linkers have
  frameworks.

## 3. Options not taken

- **`links = ["framework:IOKit"]`**: a second meaning inside a field whose values are file names.
- **Frameworks in `[package]`**: a package that builds on Linux would fail to link there.

## 4. Implementation and tests (2026-11-19)

- `Manifest.frameworks` (src/packages/manifest.cee), `NativeBuild.frameworks` and the link flags
  (src/host/toolchain.cee), their collection in src/main.cee.
- `tools/package_tests.py` check 43, `manifest_frameworks`: the link flags for each target, the refusals, and a
  program calling IOKit linked on macOS. Passed (40 checks).
