#!/usr/bin/env bash
# Every colour is derived from the reader's palette. MERGEN names none — the
# one exception is presentation's black surround, which is argued in MZ.md §6.
set -uo pipefail
cd "$(dirname "$0")/.."
status=0

echo "== hardcoded hex or rgb literals =="
if git grep -InE 'QColor\s*\(\s*(0x|[0-9]{1,3}\s*,)|#[0-9a-fA-F]{6}' -- 'src/*.cpp' 'src/*.h'; then
    echo "FAIL: a colour is named in code"
    status=1
else
    echo "clean"
fi

echo "== Qt named colours (Qt::black is allowed only in the presentation surround) =="
hits=$(git grep -InE 'Qt::(white|red|green|blue|yellow|cyan|magenta|gray|darkGray|lightGray)' -- 'src/*.cpp' 'src/*.h' || true)
if [ -n "$hits" ]; then
    echo "$hits"
    echo "FAIL: a named colour outside the palette"
    status=1
else
    echo "clean"
fi

exit $status
