#!/bin/sh
# build-deb.sh - build the cpumon .deb via the real Debian packaging in
# debian/ (dpkg-buildpackage), then copy the result into pool/main/ and
# regenerate the flat apt repository index (Packages, Packages.gz, Release)
# at the repo root so `apt update && apt install cpumon` works against this
# repo as-is.
#
# Requires: dpkg-dev, debhelper, libncursesw5-dev (or libncurses-dev), pkg-config,
# apt-utils (for apt-ftparchive).
#   sudo apt-get install build-essential debhelper libncurses-dev pkg-config dpkg-dev apt-utils
#
# Usage: ./scripts/build-deb.sh
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

echo "==> Building package with dpkg-buildpackage"
dpkg-buildpackage -us -uc -b

VERSION="$(dpkg-parsechangelog -SVersion)"
ARCH="$(dpkg --print-architecture)"
DEB="../cpumon_${VERSION}_${ARCH}.deb"

if [ ! -f "$DEB" ]; then
    echo "error: expected build output '$DEB' not found" >&2
    exit 1
fi

echo "==> Publishing to pool/main/ and regenerating repo index"
mkdir -p "$HERE/pool/main"
cp "$DEB" "$HERE/pool/main/"

cd "$HERE"
dpkg-scanpackages --multiversion pool/ > Packages
gzip -9 -n -f -k Packages

# Release describes the repo (and checksums Packages/Packages.gz) so apt
# accepts it as a source at all - without one, `apt update` fails with
# "repository ... does not have a Release file". Generate it from a clean
# staging copy holding only the published repo contents, not the full
# checkout (debian/, src/, .git/, etc. don't belong in the index), and
# write the output outside that directory so apt-ftparchive doesn't see
# (and checksum) its own not-yet-finished output file.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp -r Packages Packages.gz pool "$STAGE/"
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=cpumon \
    -o APT::FTPArchive::Release::Label=cpumon \
    -o APT::FTPArchive::Release::Suite=stable \
    -o APT::FTPArchive::Release::Codename=stable \
    -o APT::FTPArchive::Release::Architectures=amd64 \
    -o APT::FTPArchive::Release::Description="cpumon flat apt repository" \
    release "$STAGE" > "$HERE/Release"

echo ""
echo "Built and published: pool/main/cpumon_${VERSION}_${ARCH}.deb"
echo "Repo index refreshed: Packages, Packages.gz, Release"
echo ""
echo "Install locally with:  sudo apt install $HERE/pool/main/cpumon_${VERSION}_${ARCH}.deb"
