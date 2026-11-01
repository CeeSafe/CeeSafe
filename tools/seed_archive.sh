#!/bin/sh
# The release's C seed (docs/release.md, ROADMAP P9): the C of ceesafec in SEED_DIR (`ceesafec build --emit c`), with
# bootstrap/seed/README.md for VERSION and the licences, as OUT_DIR/ceesafec-VERSION-seed.tar.gz, a directory
# ceesafec-VERSION-seed/ that builds ceesafec with a C compiler alone.
#
#   sh tools/seed_archive.sh SEED_DIR VERSION OUT_DIR
set -eu
seed=$1
version=$2
out=$3
root=$(cd "$(dirname "$0")/.." && pwd)
name="ceesafec-$version-seed"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/$name" "$out"
cp -R "$seed"/. "$work/$name/"
sed "s/@VERSION@/$version/g" "$root/bootstrap/seed/README.md" > "$work/$name/README.md"
cp "$root/COPYING" "$root/COPYING.RUNTIME" "$work/$name/"
# Reproducible: sorted names, fixed owner and times (GNU tar). On macOS, bsdtar without extended attributes or
# AppleDouble files.
export COPYFILE_DISABLE=1
(cd "$work" && find "$name" -type f | LC_ALL=C sort > files &&
    { tar -cf - --owner=0 --group=0 --numeric-owner --mtime=@0 -T files 2>/dev/null ||
        tar -cf - --no-xattrs --no-mac-metadata --uid 0 --gid 0 -T files; }) |
    gzip -n > "$out/$name.tar.gz"
echo "$out/$name.tar.gz"
