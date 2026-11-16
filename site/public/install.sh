#!/bin/sh
# Installs ceesafec, the CeeSafe compiler, from a GitHub release (https://github.com/CeeSafe/CeeSafe; docs/release.md).
#
#   curl -fsSL https://ceesafe.github.io/CeeSafe/install.sh | sh
#
# CEESAFE_VERSION=v0.1.0   install that release instead of the latest
# CEESAFE_INSTALL=<dir>    install into <dir>/bin instead of ~/.ceesafe/bin
#
# The archive is checked against the release's SHA256SUMS before anything is installed.
set -eu

repo="CeeSafe/CeeSafe"
version="${CEESAFE_VERSION:-latest}"
root="${CEESAFE_INSTALL:-$HOME/.ceesafe}"

fail() {
    echo "ceesafec install: $*" >&2
    exit 1
}

case "$(uname -s)" in
    Darwin) os=osx ;;
    Linux) os=linux ;;
    FreeBSD) os=freebsd ;;
    *) fail "no prebuilt ceesafec for $(uname -s); on Windows use install.ps1, elsewhere build from source (guide/installation.md)" ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) arch=x64 ;;
    arm64 | aarch64) arch=arm64 ;;
    *) fail "no prebuilt ceesafec for $(uname -m)" ;;
esac
# FreeBSD: x86_64 only (ADR 0149).
[ "$os-$arch" != freebsd-arm64 ] || fail "no prebuilt ceesafec for FreeBSD on $(uname -m); build it from the seed (guide/installation.md)"
asset="ceesafec-$os-$arch.tar.gz"

# CEESAFE_DOWNLOAD_BASE (a URL of a directory holding the assets) is for tests.
if [ -n "${CEESAFE_DOWNLOAD_BASE:-}" ]; then
    base="$CEESAFE_DOWNLOAD_BASE"
elif [ "$version" = latest ]; then
    base="https://github.com/$repo/releases/latest/download"
else
    base="https://github.com/$repo/releases/download/$version"
fi

# Named apart from the commands they call: 0.53.0's script named these fetch() and sha256(), the names of FreeBSD's
# own commands, so on FreeBSD each called itself until the shell overflowed its stack.
download() {
    if command -v curl > /dev/null 2>&1; then
        curl -fsSL "$1" -o "$2" || fail "cannot download $1"
    elif command -v wget > /dev/null 2>&1; then
        wget -q "$1" -O "$2" || fail "cannot download $1"
    elif command -v fetch > /dev/null 2>&1; then
        fetch -q -o "$2" "$1" || fail "cannot download $1" # FreeBSD's base system has fetch, not curl
    else
        fail "needs curl, wget or fetch"
    fi
}

checksum() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d ' ' -f 1
    elif command -v sha256 > /dev/null 2>&1; then
        sha256 -q "$1" # FreeBSD's base system
    else
        fail "needs sha256sum, shasum or sha256"
    fi
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

echo "ceesafec install: downloading $asset ($version)"
download "$base/$asset" "$work/$asset"
download "$base/SHA256SUMS" "$work/SHA256SUMS"
expected=$(grep "  $asset\$" "$work/SHA256SUMS" | cut -d ' ' -f 1)
[ -n "$expected" ] || fail "SHA256SUMS has no line for $asset"
[ "$(checksum "$work/$asset")" = "$expected" ] || fail "$asset does not match its SHA-256 in SHA256SUMS; nothing was installed"

mkdir -p "$work/unpacked"
tar -xzf "$work/$asset" -C "$work/unpacked"
mkdir -p "$root/bin"
cp "$work/unpacked/ceesafec" "$root/bin/ceesafec.new"
chmod 755 "$root/bin/ceesafec.new"
mv "$root/bin/ceesafec.new" "$root/bin/ceesafec" # replaces a running ceesafec safely
cp "$work/unpacked/COPYING" "$work/unpacked/COPYING.RUNTIME" "$root/"

echo "ceesafec install: installed $("$root/bin/ceesafec" --version) in $root/bin"
case ":$PATH:" in
    *":$root/bin:"*) ;;
    *)
        echo "ceesafec install: add it to your PATH, for example in ~/.zshrc or ~/.bashrc:"
        echo "    export PATH=\"$root/bin:\$PATH\""
        ;;
esac
echo "ceesafec install: ceesafec build needs a C compiler (clang or gcc); check, run --interp, fmt, lsp and mcp do not."
