# Installation

## Install ceesafec

On macOS and Linux:

```sh
curl -fsSL https://vkuttyp.github.io/CeeSafe/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://vkuttyp.github.io/CeeSafe/install.ps1 | iex
```

The script downloads the latest release of `ceesafec` for your machine from
[GitHub](https://github.com/vkuttyp/CeeSafe/releases), checks its SHA-256, and installs it in `~/.ceesafe/bin`
(`%USERPROFILE%\.ceesafe\bin`). On macOS and Linux it then tells you how to add that directory to your `PATH`; on
Windows it adds it for you. `CEESAFE_VERSION=v0.1.0` installs a given release instead. Run the same line again to
update.

Releases are built for macOS (arm64 and x64, signed and notarized by Apple), Linux (x64 and arm64; glibc 2.35 or newer:
Ubuntu 22.04, Debian 12, Fedora 36 and later) and Windows (x64 and arm64). You can also download an archive from the
[releases page](https://github.com/vkuttyp/CeeSafe/releases) yourself: each holds `ceesafec` (`ceesafec.exe`), and
`SHA256SUMS` lists their checksums. `ceesafec` is one native executable: its runtime, its libraries and this
specification are inside it, and it needs nothing else to run.

```sh
ceesafec --version
```

## Requirements

| | For | Notes |
|---|---|---|
| A C compiler: clang, gcc or MSVC | `ceesafec build`, and building `ceesafec` from source | clang and gcc are fully supported; MSVC is best-effort (`/std:c11`). `check`, `run --interp`, `fmt`, `lsp` and `mcp` need none |
| git | dependencies from git repositories | only for [packages](packages.md) that use them |
| Node.js and [Vite+](https://viteplus.dev/) | the VS Code extension | optional |

## Build from source

To work on CeeSafe itself, or on a platform without a release, build `ceesafec` from the repository. It is written in
CeeSafe and builds itself, so the first build of a checkout needs a `ceesafec`: a release's, or one built from a
release's C seed with a C compiler alone.

With a released `ceesafec` (any platform's archive from the
[releases](https://github.com/vkuttyp/CeeSafe/releases), or the one `install.sh` put in `~/.ceesafe/bin`):

```sh
git clone https://github.com/vkuttyp/CeeSafe.git
cd CeeSafe
sh tools/bootstrap_release.sh ~/.ceesafe/bin/ceesafec HEAD out/ceesafec
out/ceesafec --version
```

`tools/bootstrap_release.sh` builds, with the release, the commits recorded for it in the script (a compiler cannot
always build a tree whose own sources use library code added after it), then the tree you checked out.

On a platform with no release, build a `ceesafec` from `ceesafec-<version>-seed.tar.gz`, attached to every release:
it is ceesafec's own C, and its `README.md` gives the one `cc` (or `cl`) command. That `ceesafec` builds the tree of
its release's tag directly.

After that, `ceesafec` rebuilds itself:

```sh
out/ceesafec build -o out/ceesafec.new && mv out/ceesafec.new out/ceesafec
```

`ceesafec` is one native executable: its runtime, its libraries and this specification are inside it, so it can be
copied anywhere, for example onto your `PATH` as `~/.local/bin/ceesafec`.

To check everything, run the contract suite (the repository's `spec/testing.md`; it compiles and runs the corpus
many times, checks the fixed point, and sweeps allocation failures):

```sh
python3 tools/contract_tests.py --ceesafec out/ceesafec
```

## Check that it works

```sh
cat > hello.cee <<'EOF'
fn main() {
    Console.writeLine("Hello, CeeSafe");
}
EOF
ceesafec build hello.cee -o hello && ./hello      # through C, to a native program
ceesafec run --interp hello.cee                   # or in the reference interpreter
```

`ceesafec build` uses the C compiler in `$CC`, or `cc`; `--cc clang` picks one. On Windows it finds one itself: clang
(`winget install LLVM.LLVM`), MinGW's gcc, or Visual Studio's `cl`, and it sets up Visual Studio's headers and
libraries for clang and `cl` when you are not in a developer prompt. So with LLVM and Visual Studio's "Desktop
development with C++" workload (or its Build Tools) installed, `ceesafec build` works from any terminal and makes
`out\hello.exe` ([tooling](../spec/tooling.md) §8).

## Editor support

The repository has a VS Code extension in `editors/vscode`: syntax highlighting, and through `ceesafec lsp` errors as you
type, hover, go to definition, the outline and formatting. See [Editors](editors.md) to build and install it, and to
use the language server from other editors.
