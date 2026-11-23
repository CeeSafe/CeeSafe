# Releasing ceesafec

A tag `v<version>` pushed to GitHub runs `.github/workflows/release.yml`, which publishes `ceesafec` for seven platforms
as a GitHub release, with `SHA256SUMS`. `site/public/install.sh` and `install.ps1` (served at
`https://ceesafe.github.io/CeeSafe/install.sh` and `install.ps1`) install the latest release from it.

## How the binaries are made

One C seed for every platform, so every binary is built from the same C and checked against it:

1. **seed** (Ubuntu 22.04): downloads ceesafec 0.94.5 from this repository's release (Decision 0009;
   `bootstrap/reference.md`, "Release stage 0"; the archive's SHA-256 is checked), bootstraps ceesafec through the
   chain recorded for it (`tools/bootstrap_release.sh`), rebuilds it with itself, and writes ceesafec's own C (`build --emit
   c`, the default form). The C of the bootstrapped and the rebuilt compiler must be identical: the fixed point.
   `tools/seed_archive.sh` packs it, with `bootstrap/seed/README.md` and the licences, as
   `ceesafec-<version>-seed.tar.gz` (ROADMAP P9).
2. **seed-check**, on Ubuntu with gcc, macOS with clang and Windows with `cl`: the archive alone, built by its README's
   commands, gives a ceesafec that reports the version and emits the seed again from the tag's sources.
3. **build**, per platform: generation 1 is the seed compiled by the platform's C compiler (`cc`, or `cl` on Windows);
   generation 1 rebuilds ceesafec with its own `build` (the platform's clang, gcc or `cl`, as `ceesafec build` finds
   them); generation 1 and generation 2 must both emit the seed byte for byte. The macOS binaries are then signed with
   the Developer ID and notarized. A smoke test runs `--version`, the interpreter, a native build and the MCP server.
   `osx-x64` is built on an arm64 runner by ceesafec's own driver (`--cflag -arch --cflag x86_64`) and run under
   Rosetta when the runner has it.
   **freebsd** does the same in a FreeBSD 14.4 x86_64 VM inside an Ubuntu job (`vmactions/freebsd-vm`, pinned to a
   commit; ADR 0149), since GitHub has no FreeBSD runner: generation 1 by FreeBSD's `cc`, generation 2, the seed byte for
   byte, the smoke test, `ceesafec-freebsd-x64.tar.gz`. No secret reaches that job.
4. **extension**: the VS Code extension's tests (the language server's run against the linux-x64 `ceesafec` just
   built) and its `.vsix`, `ceesafe-language-<version>.vsix`.
5. **zed**: the Zed extension (`editors/zed`, tooling.md §2.5): its versions in `extension.toml` and `Cargo.toml`
   must be the release's, `editors/zed/LICENSE` the MIT licence, the grammar's `rev` an ancestor of the tag whose
   `editors/tree-sitter-ceesafe` is the tag's, `src/` what `grammar.js` generates (tree-sitter-cli 0.27.2), every
   source of the compiler, the libraries and the corpus's passing families parse (`tools/tree_sitter_check.py`), and
   the extension build for `wasm32-wasip2`, the target Zed builds it for. A failure holds the release back.
6. **publish**: `SHA256SUMS` (the archives, the seed and the `.vsix`) and the GitHub release.
7. **stores**: the same `.vsix` on the VS Code Marketplace and on Open VSX, publisher (namespace) `ceesafe`. It runs
   after the GitHub release, so a store's failure never holds a release back, and `--skip-duplicate` makes a rerun
   harmless.
8. **zed-registry**: `.github/workflows/zed-extension.yml`, which opens a pull request to
   [zed-industries/extensions](https://github.com/zed-industries/extensions) that moves the `ceesafe` submodule to the
   tag's commit and sets its version (`huacnlee/zed-extension-action`, pinned to v2.0.0's commit, the action Zed's
   "Updating an Extension" page names), pushed to the fork the variable `ZED_EXTENSIONS_FORK` names. It runs after
   the GitHub release, so its failure never holds a release back, and it is off while the variable is unset. It can
   also be run by hand for a tag already released (Actions, **zed-extension**, "Run workflow", the tag). Zed's
   maintainers merge the pull request.

   It only updates. CeeSafe's **first** entry in the registry is a pull request a person opens by hand, after trying
   the extension in Zed at the commit it names: Zed's publishing guide asks for that test, and zed-industries/extensions'
   `AI_POLICY.md` allows no autonomous agent to contribute and wants the pull request in its author's own words. The
   first pull request adds the submodule `extensions/ceesafe` (`https://github.com/CeeSafe/CeeSafe.git`, at the
   release's commit, which is on `main`) and, to `extensions.toml`, `[ceesafe]` with `submodule =
   "extensions/ceesafe"`, `path = "editors/zed"` and the version; `pnpm sort-extensions` sorts both files.

| Archive | Built on | Contents |
|---|---|---|
| `ceesafec-linux-x64.tar.gz`, `ceesafec-linux-arm64.tar.gz` | Ubuntu 22.04 (glibc 2.35) | `ceesafec`, `COPYING`, `COPYING.RUNTIME` |
| `ceesafec-osx-arm64.tar.gz`, `ceesafec-osx-x64.tar.gz` | macOS (arm64), signed and notarized | the same |
| `ceesafec-win-x64.zip`, `ceesafec-win-arm64.zip` | Windows | `ceesafec.exe`, `COPYING`, `COPYING.RUNTIME` |
| `ceesafec-freebsd-x64.tar.gz` | FreeBSD 14.4 (x86_64), in a VM | `ceesafec`, `COPYING`, `COPYING.RUNTIME` |
| `ceesafe-language-<version>.vsix` | Ubuntu | the VS Code extension (`guide/editors.md`) |
| `ceesafec-<version>-seed.tar.gz` | Ubuntu, checked on all three systems | ceesafec's C and runtime, `README.md` (how to build it with `cc` or `cl` alone), `COPYING`, `COPYING.RUNTIME` |

## Making a release

1. Set the version in `src/backend/c_program.cee`: `ceesafeVersion()` and `compilerBanner()` (`ceesafec <version>`)
   must agree, and the tag must name it; the workflow checks both. The version is a constant so that the generated C
   is reproducible. Set the same version in `editors/vscode/package.json`, `editors/zed/extension.toml` and
   `editors/zed/Cargo.toml` (and `cargo update -p zed_ceesafe` for its lock): the workflow checks them too. If
   `editors/tree-sitter-ceesafe` changed since the commit `editors/zed/extension.toml`'s grammar `rev` names, commit
   the grammar first and set `rev` to that commit, which must reach `main` with the release.
2. Run the contract suite on the commit: `python3 tools/contract_tests.py --ceesafec out/ceesafec` (`spec/testing.md`;
   it includes the fixed point). Then build the commit as the workflow's seed job does, from the release stage 0:
   `sh tools/bootstrap_release.sh <ceesafec 0.94.5> HEAD <dir>/ceesafec`. Stage 0 builds the tree directly, so the
   compiler's own sources may use only what 0.94.5 accepts (0.78.1 until 2026-11-23; before 2026-11-20 the chain ended
   in 0.8.0). Every workflow that builds the tree (`checks.yml`, `windows.yml`, `freebsd.yml`, `linux32.yml`) starts
   from the same release. A newer feature in `src/` (0.18.0's first tag: a two-phase
   `mut` argument, from 0.17.0) builds with `out/ceesafec` but fails the seed.
   The contract suite does this build too, as its `release chain` step, when it has the stage 0
   (`--release-stage0 <ceesafec 0.94.5>`, `$CEESAFE_RELEASE_STAGE0`, or `~/.cache/ceesafe/stage0/ceesafec`), so such
   a feature fails the suite on the commit that adds it; without a stage 0 the step says it was skipped.
3. Tag and push: `git tag v<version> && git push origin v<version>`. Only such a tag runs the workflow (there is no
   manual or branch trigger); it builds, checks, signs and notarizes everything and publishes only if every job passes.
4. Move `main` to the next development version (`<next>-dev`).

## Secrets

The `zed-registry` job needs the variable `ZED_EXTENSIONS_FORK` (`owner/extensions`, a fork of
zed-industries/extensions, on a personal account as Zed's guide asks) and the secret `ZED_COMMITTER_TOKEN`, a personal
access token of that account with the scopes `repo` and `workflow`. Until the variable is set, the job does not run.

The macOS steps need the repository secrets `APPLE_CERTIFICATE_P12` (the Developer ID Application certificate and key,
base64), `APPLE_CERTIFICATE_PASSWORD`, and for `notarytool` `ASC_P8` (the App Store Connect API key, base64),
`ASC_KEY_ID` and `ASC_ISSUER` (the issuer UUID). They were set on 2026-10-31.

The `stores` job publishes to the Marketplace with no secret, by Trusted Publishing: the publisher `ceesafe`
(marketplace.visualstudio.com/manage/publishers/ceesafe, **Trusted Publishing**) has the policy "Release workflow
(CeeSafe organization)" for repository owner `CeeSafe` (#351822535), repository `CeeSafe` (#1440301724), workflow
`release.yml`, no environment, and the job, with `id-token: write`, runs `vsce publish --oidc`. The Marketplace
records the owner and the repository by their numeric ids, and a policy cannot be edited: renaming the workflow file
or moving the repository means creating a new policy, then deleting the old one. Moving the repository from `vkuttyp`
to the `CeeSafe` organisation (2026-11-03) did: the old policy, still for `vkuttyp` (#146238), refused v0.10.1's token
until it was replaced.

Open VSX is the same, with no secret: the namespace `ceesafe` (owned by `vkuttyp`, its claim
EclipseFdn/open-vsx.org#15060 approved) has a trusted publisher for owner `CeeSafe`, repository `CeeSafe`, workflow
`release.yml`, no environment (open-vsx.org, Settings, Trusted Publishers), and the job runs
`ovsx publish --trusted-publishing`, which exchanges the job's OIDC token for a five-minute token scoped to the
extension. Open VSX registers a trusted publisher only for an extension that exists, so 0.3.2, the first version there,
was published by the owner with an access token (2026-11-01). A registration cannot be edited: renaming the workflow
file means deleting it and registering again. It compares the owner and the repository with their case: a
registration for `ceesafe/ceesafe` refused the job's `CeeSafe/CeeSafe` (v0.10.1, 2026-11-03).
