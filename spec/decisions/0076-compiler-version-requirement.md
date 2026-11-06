# ADR 0076 — A package's compiler requirement: `ceesafec = ">=0.19.1"`

- Status: **Accepted** by the owner, 2026-11-06 ("yes", to adding the key so CosmoStore's manifest can pin 0.19.1).
  Raised by the CosmoStore port (`CeeSafeApps/cosmostore/CEESAFE_GAPS.md` G23).
- Scope: tooling.md §4.1 and §4.5, `src/packages/manifest.cee`, guide/packages.md, the package tests.

## 1. Context

CosmoStore needs ceesafec 0.19.1: `File.readAt` and the other durable methods take `borrow this` from that release on
(ADR 0075 §8). Under 0.19.0, `ceesafec check` reports 12 errors in `durable.cee`, the first one N0248, and nothing in
them says that a newer compiler would accept the package. A manifest has no way to state the compiler it needs.

## 2. Decision

1. `[package]` takes `ceesafec = "<requirement>"`, a version requirement in the dependencies' syntax (§4.7):
   - comparators separated by commas, all of which must hold (`>=0.19.1`, `>=0.19.1, <0.20.0`);
   - `1.4` or `^1.4` is a caret requirement;
   - `*` is any version.
2. When the manifest is read (`check`, `build`, `run`, `test`, `fetch`, and a dependency's manifest as the root's), a
   compiler whose version does not satisfy the requirement reports N0321 at the value, before anything is checked:
   `this package needs ceesafec >=0.19.1; this is ceesafec 0.19.0`. A requirement that does not parse is N0321
   with the field's shape.
3. A development version (`0.20.0-dev`) is compared as its MAJOR.MINOR.PATCH.
4. A compiler older than this ADR reports `unknown field 'ceesafec' in [package]` (N0321) at the same line, which
   still points the reader at the requirement.

## 3. Not included

- Choosing or downloading a compiler: the field states a requirement, and the reader acts on it.
- A requirement on lib/core or lib/std alone: they ship with the compiler, so its version covers them.
