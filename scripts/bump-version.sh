#!/bin/sh
# bump-version.sh - the one command to bump cpumon's version.
#
# Updates, in lockstep, the three places a version has to be recorded:
#   - VERSION                (compiled into the binary as `cpumon --version`)
#   - debian/changelog       (new entry, drives the package's version)
#   - man/cpumon.1           (.TH line's version + date)
#
# Usage: ./scripts/bump-version.sh <new-version> ["changelog message"] ["debian-revision"]
#   new-version       e.g. 1.1   (no "v" prefix, no debian revision)
#   changelog message defaults to "New release."; pass multiple lines by
#                     calling with a single quoted argument containing them
#   debian-revision   defaults to 1 (i.e. produces version "<new-version>-1")
#
# After running this, build and publish as usual:
#   ./scripts/build-deb.sh
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

NEW_VERSION="${1:?usage: $0 <new-version> [\"changelog message\"] [debian-revision]}"
MESSAGE="${2:-New release.}"
DEBIAN_REV="${3:-1}"

case "$NEW_VERSION" in
    [0-9]*.[0-9]*) : ;;
    *)
        echo "error: version '$NEW_VERSION' doesn't look like X.Y (or X.Y.Z)" >&2
        exit 1
        ;;
esac

MAINTAINER="$(sed -n 's/^Maintainer: //p' debian/control)"
DATE_RFC2822="$(date -R)"
TODAY="$(date +%Y-%m-%d)"

echo "==> Updating VERSION: $(cat VERSION) -> $NEW_VERSION"
printf '%s\n' "$NEW_VERSION" > VERSION

echo "==> Prepending debian/changelog entry for ${NEW_VERSION}-${DEBIAN_REV}"
{
    echo "cpumon (${NEW_VERSION}-${DEBIAN_REV}) unstable; urgency=medium"
    echo
    printf '  * %s\n' "$MESSAGE"
    echo
    echo " -- ${MAINTAINER}  ${DATE_RFC2822}"
    echo
    cat debian/changelog
} > debian/changelog.new
mv debian/changelog.new debian/changelog

echo "==> Updating man/cpumon.1 .TH line"
sed -i "1s/.*/.TH CPUMON 1 \"${TODAY}\" \"cpumon ${NEW_VERSION}\" \"User Commands\"/" man/cpumon.1

echo ""
echo "Done. Now:"
echo "  - review/edit the new debian/changelog entry if the one-liner isn't enough"
echo "  - rebuild and publish:  ./scripts/build-deb.sh"
