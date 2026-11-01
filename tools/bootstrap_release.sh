#!/bin/sh
# Builds ceesafec from a checkout with only a released ceesafec (Decision 0009): the release workflow's stage 0, and
# the way to a first ceesafec from a fresh checkout.
#
# A released compiler cannot always build the tree at once: a commit that adds library code (lib/, embedded in
# src/library) and uses it in the compiler's own sources needs a compiler that already embeds it. The chain below
# crosses each such commit, recorded per stage-0 version (bootstrap/reference.md, "Release stage 0"):
#
#   <commit>             the commit's ceesafe.toml and src/, built by the current compiler
#   <commit>+<library>   the same, with <library>'s src/library: a bridge, the older compiler with the newer library
#
# Each step's compiler builds the next; the last builds REVISION. The workflow then checks the fixed point (the C that
# compiler emits equals the C its own rebuild emits), which is what makes the chain's route irrelevant to the result.
#
# usage: tools/bootstrap_release.sh STAGE0 [REVISION] [OUTPUT]
#        defaults: HEAD, out/ceesafec
set -eu

STAGE0=$1
REVISION=${2:-HEAD}
OUTPUT=${3:-out/ceesafec}

version=$("$STAGE0" --version)
CHAIN_FLAGS=""
case "$version" in
"ceesafec 0.3.2")
    # The release's stage 0 since 2026-11-01: it builds the tree at once (checked at 8cccac0, macOS and Debian).
    CHAIN=""
    ;;
"ceesafec 0.2.0")
    # 7d2a041: the newest commit 0.2.0 builds (ADR 0032, channels). 291246d (L1 A2d) adds AsyncSockets to lib/std
    # and uses it in the interpreter, so 7d2a041 is built again with 291246d's library; that compiler builds the tree
    # (checked 2026-11-01 up to 70da046: the same C as the compiler it was released from).
    CHAIN="7d2a041bc276da110253e32731176129e018e12d
7d2a041bc276da110253e32731176129e018e12d+291246db19892ed2c20c8bb2376b16fb6a35d658"
    # Before 844e755 (L1 A4a) the hosted async runtime used CLOCK_MONOTONIC without asking glibc for POSIX, which
    # gcc's -std=c11 hides; the chain's compilers predate it (and so the runtime they build REVISION with), so on
    # Linux every build here defines _GNU_SOURCE, as the runtime now does itself. The flag changes how the C compiler
    # builds a compiler, not the C it emits: the fixed point and the release's binaries come from the seed.
    if [ "$(uname -s)" = Linux ]; then
        CHAIN_FLAGS="--cflag -D_GNU_SOURCE"
    fi
    ;;
*)
    echo "bootstrap_release: no chain recorded for '$version' (bootstrap/reference.md, \"Release stage 0\")" >&2
    exit 1
    ;;
esac

repo=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

compiler=$STAGE0
n=0
for step in $CHAIN "$REVISION"; do
    n=$((n + 1))
    commit=${step%%+*}
    mkdir -p "$work/tree$n"
    git -C "$repo" archive "$commit" ceesafe.toml src | tar -x -C "$work/tree$n"
    case "$step" in
    *+*)
        rm -rf "$work/tree$n/src/library"
        git -C "$repo" archive "${step#*+}" src/library | tar -x -C "$work/tree$n"
        ;;
    esac
    echo "bootstrap_release: step $n: $step, built by $("$compiler" --version)"
    # shellcheck disable=SC2086 # CHAIN_FLAGS is a list of words
    (cd "$work/tree$n" && "$compiler" build $CHAIN_FLAGS -o "$work/stage$n")
    compiler=$work/stage$n
done

mkdir -p "$(dirname "$OUTPUT")"
cp "$compiler" "$OUTPUT"
"$OUTPUT" --version
