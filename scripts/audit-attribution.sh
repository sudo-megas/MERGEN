#!/usr/bin/env bash
# No tooling attribution anywhere: tree, commits, or About text — MZ.md §5, §11.
# The rule bans credit and traces, not use.
set -uo pipefail
cd "$(dirname "$0")/.."

# Word-boundary matched, so "paints" and "against" are not mistaken for "ai".
pattern='co-authored-by|generated with|claude|anthropic|openai|chatgpt|copilot|\bLLM\b|\bAI-(generated|assisted|written)\b'
status=0

echo "== tracked files =="
if git grep -IniE "$pattern" -- \
      ':!scripts/audit-attribution.sh' ':!build/docs/*' ':!docs/*'; then
    echo "FAIL: attribution found in the tree"
    status=1
else
    echo "clean"
fi

echo "== commit messages =="
if git log --format='%H%n%B' | grep -inE "$pattern"; then
    echo "FAIL: attribution found in history"
    status=1
else
    echo "clean"
fi

echo "== commit trailers and authors =="
if git log --format='%an <%ae>' | sort -u | grep -viE '^sudo-megas '; then
    echo "FAIL: a commit from another account"
    status=1
else
    echo "clean"
fi

exit $status
