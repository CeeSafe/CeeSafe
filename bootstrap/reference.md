# Stage 0 and the fixed points

Updated only by an explicit change of stage 0 (below), never silently.

## Release stage 0 (Decision 0009)

CeeSafe builds itself. A checkout's first `ceesafec`, and every release's, comes from a released `ceesafec`:
`tools/bootstrap_release.sh STAGE0`, with the chain recorded there for the stage 0's version.

| Item | Value |
|---|---|
| Compiler | `ceesafec 0.2.0`, `https://github.com/vkuttyp/CeeSafe/releases/download/v0.2.0/ceesafec-linux-x64.tar.gz` |
| sha256 | `2e898a239f36fb7d0c39262d8ac59c98d8b98219e1fc4f0e4993136aec3e18cd` (v0.2.0's `SHA256SUMS`); osx-arm64 `514ddaaf29289fb6549cc68c5701c5f910c4ae42e8c44f1726f5686040ce039a` |
| Chain | `7d2a041` (the newest commit 0.2.0 builds), then `7d2a041` with `291246d`'s `src/library` (the bridge: 291246d uses the `AsyncSockets` it adds), then the tree |
| Checked | 2026-11-01 at `70da046`: macOS arm64 (Apple clang) and Debian 12 x86-64 (gcc 12, `-D_GNU_SOURCE` for the chain), fixed point reached, seed `main.c` `d1ba9faf…`, identical to the C of the compiler 0.3.0 was built with; the v0.3.1 workflow's seed and builds on six platforms |

A platform with no release builds a first `ceesafec` from a release's C seed (`ceesafec-<version>-seed.tar.gz`,
`bootstrap/seed/README.md`) with a C compiler alone.

### Changing stage 0

1. Pick a released `ceesafec`, normally the newest.
2. Find the chain from it to the tree: the newest commit it builds, a bridge (that commit with the next commit's
   `src/library`) where a commit uses library code it adds, and so on; often the release builds the tree at once and
   the chain is empty. Record it in `tools/bootstrap_release.sh` under the release's `--version`.
3. Record the version, URL and hashes above; keep the old row in the history below.
4. Run the bootstrap and the fixed point on macOS and Linux, and note the result in ROADMAP §10.

## History

| Date | Stage 0 | Reason |
|---|---|---|
| 2026-10-30 | the reference compiler 0.3.1 to 0.6.0 | the port: CeeSafe was built by the compiler it was ported from until it built itself (P6) |
| 2026-11-01 | ceesafec 0.2.0 (Decision 0009) | the reference compiler's releases are no longer public |

## Fixed points

Each verified fixed point (ROADMAP P6) archives its manifest in `bootstrap/fixed-point/`: the commit, the stage-0
binary's hash, the C compiler, every output's sha256 and the subprocess audit. Rerun with
`python3 tools/fixed_point.py --stage0 <stage 0> --commit <commit> --work <dir> --manifest <file>`; the C hashes must
equal the archived ones.

- 2026-10-30, `1b1aae4`, the reference compiler 0.6.0 as stage 0: macOS arm64 (Apple clang 21) and Linux x86-64
  (GCC 12), same C (`main.c` `40374dbf…`).
- 2026-10-30, `8941c10`: fixed point on CeeSafe's names with a bootstrapped ceesafec as stage 0, on macOS arm64
  (Apple clang 21) and Linux x86-64 (GCC 12): gen 1 = gen 2 = gen 3 = stage 0, C the same on both hosts
  (`main.c` `4b73077c…`); manifests `bootstrap/fixed-point/2026-10-30-p6r-*.md`.
- Later ones: `bootstrap/fixed-point/2026-10-3*-*.md` (P7, P8, the specification), each with a ceesafec as stage 0.
