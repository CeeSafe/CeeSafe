---
title: Installation
summary: How to install ceesafec on macOS, Linux and Windows, check that it works, update it, and what else it needs (a C compiler, for build).
keywords: [ceesafec, install, update, PATH, C compiler, "--version"]
---

# Installation

**This section teaches** how to install `ceesafec`, the CeeSafe compiler, and check that it works. It assumes
nothing but a terminal.

`ceesafec` is one program. The compiler, the formatter, the test runner, the libraries and the language's
specification are all inside it; there is nothing else to install to write and run CeeSafe.

## Installing on macOS and Linux

Run this in a terminal:

```sh
curl -fsSL https://ceesafe.github.io/CeeSafe/install.sh | sh
```

The script downloads the latest release of `ceesafec` for your machine, checks it against its published SHA-256,
and puts it in `~/.ceesafe/bin`. It then prints the line to add to your shell's profile so that directory is on
your `PATH`. Add it, and open a new terminal.

## Installing on Windows

Run this in PowerShell:

```powershell
irm https://ceesafe.github.io/CeeSafe/install.ps1 | iex
```

It installs `ceesafec.exe` in `%USERPROFILE%\.ceesafe\bin` and adds that directory to your `PATH`. Open a new
terminal afterwards.

## Checking the installation

```sh
ceesafec --version
```

You should see the version, in this form:

```text
ceesafec 0.40.0
```

If the command is not found, the directory is not on your `PATH`: check the line the installer printed, and that
you opened a new terminal.

## A C compiler, for `build`

CeeSafe compiles to C. To turn a program into an executable file, `ceesafec build` runs a C compiler, so you need
one for that command:

| System | What to install |
|---|---|
| macOS | the Xcode command line tools: `xcode-select --install` (clang) |
| Linux | `gcc` or `clang`, from your distribution's packages |
| Windows | Visual Studio's C++ build tools (MSVC), or clang |

You do not need one to follow most of this book. `ceesafec check` finds every error, and `ceesafec run --interp`
runs a program in the compiler's own interpreter; neither uses a C compiler.

## Updating and removing

To update, run the installer's line again: it replaces `ceesafec` with the latest release. To install a particular
release, set `CEESAFE_VERSION` first, for example `CEESAFE_VERSION=v0.40.0`.

To remove CeeSafe, delete the `.ceesafe` directory in your home directory, and the `PATH` line you added.

## Reading this book offline

The specification is inside `ceesafec`: `ceesafec explain N0100` prints what a diagnostic means, with no network.
This book as one file is at `llms-full.txt` beside this page, and its source is in the repository under `book/`.

## Summary

- `ceesafec` is one program: install it with the script for your system.
- `ceesafec --version` shows that it is installed, and which version.
- `ceesafec build` needs a C compiler; `check` and `run --interp` do not.
- Running the installer again updates it.

Next: [a first program](./ch01-02-hello-world).
