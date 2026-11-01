# Decision 0009 — A release bootstraps from CeeSafe's own release

- Status: **Accepted** by the owner, 2026-11-01 ("Bootstrap from 0.2.0", asked how the release should get its
  stage-0 compiler after the v0.3.0 workflow failed).
- Relates to: Decision 0001 (identity and bootstrap), Decision 0002 §3 (the bootstrap anchor), Decision 0007 (CeeSafe's
  own expectations), Decision 0010 (the predecessor retired), `bootstrap/reference.md`, `docs/release.md`.

## Context

The release workflow's seed job downloaded the reference compiler 0.6.0, the compiler CeeSafe was ported from, from
that project's GitHub release, and bootstrapped ceesafec from it. That repository became private, and the download now
returns 404: the v0.3.0 tag's run (41302947031) failed in its first step, and nothing was published. ceesafec 0.2.0, a
public release of this repository, is a compiler that needs nothing outside it.

ceesafec 0.2.0 does not build today's tree at once. `lib/` is embedded in the compiler (`src/library`), and a commit that
adds library code and uses it in the compiler's own sources needs a compiler that already embeds it: 291246d (L1 A2d)
adds `AsyncSockets` to lib/std and uses it in the interpreter.

## Decision

1. **The release's stage 0 is ceesafec 0.2.0**, `ceesafec-linux-x64.tar.gz` from this repository's v0.2.0 release,
   checked against the SHA-256 of that release's `SHA256SUMS` (`bootstrap/reference.md`, "Release stage 0").
2. **`tools/bootstrap_release.sh`** builds the tree from it through a chain recorded per stage-0 version: commits it
   builds in order, and **bridges**, a commit's sources built with a later commit's `src/library`, which cross a
   commit that uses library code it adds. For 0.2.0: 7d2a041, then 7d2a041 with 291246d's library, then the tree.
3. **The fixed point is the check**, as before: the bootstrapped compiler's C must equal the C of its own rebuild. The
   route to it does not enter the result: from 0.2.0 the seed at 70da046 was byte-identical to the C of the compiler
   0.3.0 had been built with (2026-11-01, macOS and Debian).
4. It is also how a checkout gets its first ceesafec. The bootstrap from the reference compiler is retired with it
   (Decision 0010).
5. Each later release may move the stage 0 to a newer CeeSafe release, recording its chain (often none) and hash.

## Consequences

- Releasing needs nothing outside this repository.
- The chain's old compilers predate 844e755, whose runtime asks glibc for POSIX; on Linux the script builds every step
  with `-D_GNU_SOURCE`. That changes how a compiler binary is built, not the C it emits; the release's binaries come
  from the seed.
- v0.3.0 stays a tag whose release failed, with no assets; the first release made this way is 0.3.1, with the same
  compiler as 0.3.0 but for its version.
