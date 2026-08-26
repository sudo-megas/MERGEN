#!/usr/bin/env bash
# Build the Debian package from packaging/debian.
#
# Usage: scripts/build-deb.sh
#
# Leaves the .deb, the .buildinfo and the .changes in dist/, and leaves the
# build tree in place so the audits can be pointed at it afterwards.
set -euo pipefail
cd "$(dirname "$0")/.."

# debhelper reads debian/ from the root of the tree it is building, and this
# repository keeps packaging under packaging/ beside the PKGBUILD. Copying it
# up is the whole of the difference; nothing in debian/ is written by hand at
# build time. The copy is ignored by git, so a local run leaves no trace in
# `git status`.
copy_packaging() {
    rm -rf debian
    cp -r packaging/debian debian
    # cp does not always carry the execute bit across a filesystem that lacks
    # it, and dpkg-buildpackage fails late and unhelpfully when rules is not
    # executable.
    chmod +x debian/rules
}

# One version, named in three files.
#
# CMakeLists.txt sets what the About dialog shows, PKGBUILD sets what pacman
# installs, and debian/changelog sets what dpkg installs. A release that
# updates two of the three produces a package whose name disagrees with the
# program inside it, and nothing about the build would say so — the same shape
# of defect audit-policy.sh exists to catch for the helper path.
check_versions() {
    local from_cmake from_pkgbuild from_changelog upstream
    from_cmake=$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\)[[:space:]]*$/\1/p' \
                 CMakeLists.txt | head -1)
    from_pkgbuild=$(sed -n 's/^pkgver=//p' packaging/PKGBUILD | head -1)
    from_changelog=$(dpkg-parsechangelog -l packaging/debian/changelog -S Version)
    # The Debian revision is this packaging's own counter and is expected to
    # differ; only the upstream part has to agree.
    upstream=${from_changelog%-*}

    printf "  CMakeLists.txt      %s\n" "${from_cmake:-<not found>}"
    printf "  PKGBUILD            %s\n" "${from_pkgbuild:-<not found>}"
    printf "  debian/changelog    %s (upstream %s)\n" "$from_changelog" "$upstream"

    if [ -z "$from_cmake" ] || [ -z "$from_pkgbuild" ]; then
        echo "FAIL: could not read a version out of CMakeLists.txt or PKGBUILD"
        return 1
    fi
    if [ "$from_cmake" != "$upstream" ] || [ "$from_pkgbuild" != "$upstream" ]; then
        echo "FAIL: the three files name different versions"
        return 1
    fi
    echo "  ok"
}

echo "== one version in three files =="
check_versions

echo "== staging debian/ =="
copy_packaging

echo "== dpkg-buildpackage =="
# Binary only: no source package is wanted here, so no orig tarball is needed
# despite the 3.0 (quilt) format. Unsigned, because CI holds no key — the
# release page is the point of trust, as it already is for the Arch package.
dpkg-buildpackage --build=binary --no-sign

echo "== collecting =="
# dpkg-buildpackage writes beside the source tree rather than into it.
mkdir -p dist
mv -f ../mergen_*.deb ../mergen-dbgsym_*.d*eb ../mergen_*.buildinfo ../mergen_*.changes dist/ 2>/dev/null || true
# The .deb itself is the artefact that must exist; the rest are welcome extras.
if ! ls dist/mergen_*.deb >/dev/null 2>&1; then
    echo "FAIL: no .deb was produced"
    exit 1
fi
ls -la dist/
