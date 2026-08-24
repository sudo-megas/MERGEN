#!/usr/bin/env bash
# Nothing linked beyond what MZ.md §3 argues for.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN="${1:-build/mergen}"
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 1; }

# Direct dependencies only; the transitive closure is their business.
# libGLX and libOpenGL are not MERGEN's asking: Qt6Gui carries them in its own
# link interface on Linux, and they arrive with qt6-base, which §3 argues for.
allowed='libQt6Widgets|libQt6Gui|libQt6Core|libQt6PrintSupport|libQt6Network|libpoppler-qt6|libqpdf|libGLX|libOpenGL|libstdc\+\+|libm|libgcc_s|libc\.so|ld-linux'
banned='libKF6|libkio|libgtk|libglib-2|libgio|libcurl|libsqlite3|libQt6Qml|libQt6Quick|libQt6WebEngine|libQt6Dbus|libQt6Sql'

echo "== direct links =="
links=$(readelf -d "$BIN" | awk '/NEEDED/ {gsub(/[\[\]]/,"",$5); print $5}')
echo "$links" | sed 's/^/    /'
status=0

echo "== banned =="
if echo "$links" | grep -E "$banned"; then
    echo "FAIL: a banned library is linked"
    status=1
else
    echo "none"
fi

echo "== unexpected =="
unexpected=$(echo "$links" | grep -vE "$allowed" || true)
if [ -n "$unexpected" ]; then
    echo "$unexpected" | sed 's/^/    /'
    echo "FAIL: linked against something §3 does not argue for"
    status=1
else
    echo "none"
fi

exit $status
