#!/usr/bin/env bash
# Every colour is derived from the reader's palette. MERGEN names none — the
# one exception is presentation's black surround, argued in MZ.md §6.
set -euo pipefail
cd "$(dirname "$0")/.."
git rev-parse --git-dir >/dev/null 2>&1 || {
    echo "FAIL: not a git repository — this audit cannot run here"; exit 2; }
status=0

# The old pattern required "(" immediately after QColor, so it saw
# QColor(255,0,0) and missed QColor x(255,0,0) — the ordinary declaration form,
# and the likeliest way anyone would actually write it.
construct='QColor[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)?[[:space:]]*[({][[:space:]]*(0x[0-9a-fA-F]+|[0-9]{1,3}[[:space:]]*,)'
byname='QColor[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)?[[:space:]]*\([[:space:]]*"'
# Only when handed literals. A helper fed computed values is deriving a colour
# from the palette, which is the thing §6 wants, not naming one.
helpers='(setRgb|setRgbF|setHsv|setHsl|setNamedColor|fromRgb|fromRgbF|fromHsv|fromHsl|qRgb|qRgba)[[:space:]]*\([[:space:]]*(0x[0-9a-fA-F]+|[0-9]{1,3}[[:space:]]*[,)])'
hexlit='#[0-9a-fA-F]{3,8}"'
named='Qt::(white|black|red|darkRed|green|darkGreen|blue|darkBlue|cyan|darkCyan|magenta|darkMagenta|yellow|darkYellow|gray|darkGray|lightGray)'

echo "== constructed or named colours =="
hits=$(git grep -InE "$construct|$byname|$helpers|$hexlit" -- 'src/*.cpp' 'src/*.h' || true)
if [ -n "$hits" ]; then echo "$hits"; echo "FAIL: a colour is named in code"; status=1
else echo "clean"; fi

echo "== Qt named colours =="
# Qt::black is permitted only in the presentation surround (MZ.md §6). Anything
# else, including a second Qt::black, is a finding.
allhits=$(git grep -InE "$named" -- 'src/*.cpp' 'src/*.h' || true)
offenders=$(echo "$allhits" | grep -v '^src/pageview.cpp:[0-9]*: *return Qt::black;' || true)
if [ -n "$offenders" ]; then echo "$offenders"; echo "FAIL: a named colour outside the palette"; status=1
else echo "clean (only the argued presentation surround)"; fi

exit $status
