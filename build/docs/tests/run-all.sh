#!/bin/bash
# Compile and run every acceptance suite, from the repository root.
#
#     bash build/docs/tests/run-all.sh [build-dir]
#
# The suites are not part of the CMake build — each links the objects the real
# build produced, with main.cpp.o swapped for the test's own main(). See
# BUILD.md, which this script is the executable form of.
#
# Exit code is the number of suites that failed.
set -u

B=${1:-build}
T=build/docs/tests

if [ ! -d "$T" ]; then
    echo "run-all.sh: run me from the repository root" >&2
    exit 1
fi
if [ ! -x "$B/mergen" ]; then
    echo "run-all.sh: no binary at $B/mergen — build first" >&2
    exit 1
fi

# Not a preference. Several checks turn on a directory this account cannot
# traverse, and root ignores permission bits entirely: as root, stat() on a
# mode-000 directory succeeds, Presence::Unreadable never occurs, and every
# check about the elevation path passes without testing anything. A suite that
# cannot fail is worse than no suite, so refuse rather than report green.
if [ "$(id -u)" -eq 0 ]; then
    echo "run-all.sh: refusing to run as root — the permission checks would pass" >&2
    echo "            vacuously. Run as an ordinary user." >&2
    exit 1
fi

VERSION=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt | head -1)
DATE=$(sed -n 's/.*MERGEN_RELEASE_DATE "\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)
HELPER="$PWD/$B/mergen-open"
OBJS=$(find "$B/CMakeFiles/mergen.dir" -name '*.o' ! -name 'main.cpp.o')

if [ -z "$OBJS" ]; then
    echo "run-all.sh: no object files under $B — build first" >&2
    exit 1
fi

echo "== fixtures =="
python3 "$T/make-fixtures.py" | tail -3

CXXFLAGS_PKG=$(pkg-config --cflags --libs Qt6Widgets Qt6PrintSupport Qt6Network \
                                          poppler-qt6 libqpdf)

export QT_QPA_PLATFORM=offscreen
failed=0
ran=0

for src in "$T"/z*check.cpp; do
    name=$(basename "$src" .cpp)
    printf '\n== %s ==\n' "$name"
    # shellcheck disable=SC2086
    if ! g++ -std=c++20 -O2 -DNDEBUG -fPIC -o "$T/$name" "$src" $OBJS \
        -I src -I "$B" -I "$B/mergen_autogen/include" \
        -DMERGEN_VERSION="\"$VERSION\"" \
        -DMERGEN_HELPER_PATH="\"$HELPER\"" \
        -DMERGEN_OPEN_PATH="\"$HELPER\"" \
        -DMERGEN_RELEASE_DATE="\"$DATE\"" \
        $CXXFLAGS_PKG 2>"$T/$name.log"; then
        echo "  COMPILE FAILED"
        grep -E 'error:' "$T/$name.log" | head -5
        failed=$((failed + 1))
        continue
    fi
    ran=$((ran + 1))
    # Run from the repository root: the suites open their fixtures by relative
    # name, and make-fixtures.py wrote them here.
    if out=$("./$T/$name" 2>&1); then
        printf '%s\n' "$out" | grep -cE '^  PASS' | sed 's/^/  passed: /'
    else
        printf '%s\n' "$out" | grep -E '^  FAIL|CHECK\(S\) FAILED'
        failed=$((failed + 1))
    fi
done

# The socket suite drives real processes rather than linking the objects.
printf '\n== z8check (control socket) ==\n'
ran=$((ran + 1))
if out=$(MERGEN="$PWD/$B/mergen" bash "$T/z8check.sh" 2>&1); then
    printf '%s\n' "$out" | grep -cE '^  PASS' | sed 's/^/  passed: /'
else
    printf '%s\n' "$out" | grep -E '^  FAIL|CHECK\(S\) FAILED'
    failed=$((failed + 1))
fi

printf '\n== %d suites run, %d failed ==\n' "$ran" "$failed"
exit "$failed"
