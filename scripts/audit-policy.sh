#!/bin/bash
# The three names for the privileged helper must be one name.
#
# The binary invokes a path under pkexec, the generated policy authorises a
# path, and install() puts the file at a path. If any two disagree, elevation
# fails on a missing file and the reader is told the document cannot be read —
# which is indistinguishable, from the outside, from the bug 2.0.2 fixed.
#
# They disagreed once already: MERGEN_HELPER_PATH was `set(... CACHE ...)` with
# a computed default, which assigns only when the cache lacks the variable, so a
# build tree reconfigured with a new prefix kept the old path while install()
# moved the file. Fixed in CMakeLists; checked here so it stays fixed.
#
# Usage: audit-policy.sh [build-dir]   (default: build)
set -u

B=${1:-build}
fails=0
say() { if [ "$1" = 1 ]; then printf "  ok    %s\n" "$2"; else printf "  FAIL  %s\n" "$2"; fails=$((fails+1)); fi; }

BIN="$B/mergen"
POLICY=$(find "$B" -maxdepth 1 -name '*.policy' | head -1)

echo "== the helper path compiled into the binary =="
if [ ! -x "$BIN" ]; then
    echo "  FAIL  no binary at $BIN — build first"
    exit 1
fi
# QStringLiteral bakes it as UTF-16, which plain strings(1) will not see.
FROM_BIN=$(strings -e l "$BIN" | grep -E '^/.*/mergen-open$' | head -1)
printf "    %s\n" "${FROM_BIN:-<none found>}"
say "$([ -n "$FROM_BIN" ] && echo 1 || echo 0)" "the binary names a helper path"

echo "== the path the polkit action authorises =="
if [ -z "$POLICY" ]; then
    echo "  FAIL  no generated .policy in $B"
    exit 1
fi
FROM_POLICY=$(grep -o 'org\.freedesktop\.policykit\.exec\.path">[^<]*' "$POLICY" | sed 's/.*">//')
printf "    %s\n" "${FROM_POLICY:-<none found>}"
say "$([ -n "$FROM_POLICY" ] && echo 1 || echo 0)" "the policy names a helper path"

echo "== they must be the same path =="
say "$([ "$FROM_BIN" = "$FROM_POLICY" ] && echo 1 || echo 0)" \
    "the binary invokes exactly what polkit authorises"

echo "== and it is where install() puts the helper =="
# cmake_install.cmake records the real destination, still carrying
# ${CMAKE_INSTALL_PREFIX} unexpanded; the cache holds what that resolves to.
# Matched on the line that installs mergen-open specifically — `bin/mergen` is
# a different target and matches a looser pattern.
DEST=$(sed -n 's/.*file(INSTALL DESTINATION "\([^"]*\)".*FILES "[^"]*\/mergen-open".*/\1/p' \
       "$B/cmake_install.cmake" 2>/dev/null | head -1)
PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$B/CMakeCache.txt" 2>/dev/null | head -1)
DEST=${DEST//\$\{CMAKE_INSTALL_PREFIX\}/$PREFIX}
if [ -n "$DEST" ]; then
    printf "    install destination: %s\n" "$DEST"
    say "$([ "$(dirname "$FROM_BIN")" = "$DEST" ] && echo 1 || echo 0)" \
        "the helper is installed in the directory the binary looks in"
else
    echo "  FAIL  no install destination for mergen-open found in cmake_install.cmake"
    fails=$((fails+1))
fi

# pkexec runs whatever the policy names, so it had better not be group- or
# world-writable in the tree we are about to install from.
echo "== the helper is not writable by anyone but its owner =="
if [ -e "$B/mergen-open" ]; then
    MODE=$(stat -c '%a' "$B/mergen-open")
    printf "    build/mergen-open is %s\n" "$MODE"
    say "$([ "$((8#$MODE & 8#022))" -eq 0 ] && echo 1 || echo 0)" \
        "not group- or world-writable"
fi

if [ "$fails" -gt 0 ]; then
    printf "\n%d CHECK(S) FAILED\n" "$fails"
    exit 1
fi
printf "\nclean\n"
