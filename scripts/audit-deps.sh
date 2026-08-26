#!/usr/bin/env bash
# Nothing linked beyond what MZ.md §3 argues for.
set -euo pipefail
cd "$(dirname "$0")/.."
status=0

check_one() {
    local BIN="$1"
    [ -x "$BIN" ] || { echo "FAIL: no binary at $BIN"; return 1; }
    echo "== $BIN =="
    local links; links=$(readelf -d "$BIN" | awk '/NEEDED/ {gsub(/[\[\]]/,"",$5); print $5}')
    echo "$links" | sed 's/^/    /'

    # Anchored. The old list carried a bare "libm", which matched libmagic and
    # libmount — any new dependency whose name happened to contain it walked
    # straight through the gate meant to catch it.
    local allowed='^(libQt6Widgets|libQt6Gui|libQt6Core|libQt6PrintSupport|libQt6Network|libpoppler-qt6|libqpdf|libGLX|libOpenGL|libstdc\+\+|libm|libgcc_s|libc|ld-linux-x86-64)\.so'
    local banned='^(libKF6|libkio|libgtk|libglib-2|libgio|libcurl|libsqlite3|libQt6Qml|libQt6Quick|libQt6WebEngine|libQt6DBus|libQt6Sql)'

    local bad; bad=$(echo "$links" | grep -E "$banned" || true)
    if [ -n "$bad" ]; then echo "$bad" | sed 's/^/    /'; echo "FAIL: a banned library is linked"; return 1; fi

    local unexpected; unexpected=$(echo "$links" | grep -vE "$allowed" || true)
    if [ -n "$unexpected" ]; then
        echo "$unexpected" | sed 's/^/    /'
        echo "FAIL: linked against something §3 does not argue for"; return 1
    fi
    echo "  ok"
    return 0
}

# Both installed binaries, not just the viewer.
#
# The helper is looked for beside whichever viewer was named rather than under
# build/ unconditionally. The Debian package builds in obj-<triplet>/, and with
# the path fixed here that build had its viewer audited while its helper was
# quietly skipped — a check that reports "ok" having examined one of the two
# binaries it names.
VIEWER="${1:-build/mergen}"
check_one "$VIEWER" || status=1
HELPER="$(dirname "$VIEWER")/mergen-open"
[ -x "$HELPER" ] && { check_one "$HELPER" || status=1; }
exit $status
