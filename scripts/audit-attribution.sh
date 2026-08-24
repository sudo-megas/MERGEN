#!/usr/bin/env bash
# No tooling attribution anywhere: tree, commits, or About text — MZ.md §5, §11.
# The rule bans credit and traces, not use.
set -euo pipefail
cd "$(dirname "$0")/.."

# An environmental failure must not read as a clean audit. Assert the thing the
# checks below depend on before running any of them.
git rev-parse --git-dir >/dev/null 2>&1 || {
    echo "FAIL: not a git repository — this audit cannot run here"; exit 2; }

pattern='co-authored-by|generated (with|by)|written by (an? )?(ai|llm|model)|claude|anthropic|openai|chatgpt|gpt-[0-9]|gemini|copilot|codex|assisted[- ]by|\bLLM\b|\bAI[- ](generated|assisted|written|authored)\b'
status=0

echo "== tracked files =="
# Every tracked path, including build/docs — §5 says "anywhere in the repo".
if git grep -IniE "$pattern" -- ':!scripts/audit-attribution.sh' ':!docs/*'; then
    echo "FAIL: attribution found in the tree"; status=1
else echo "clean"; fi

echo "== commit messages =="
if git log --format='%H%n%B' | grep -inE "$pattern"; then
    echo "FAIL: attribution found in history"; status=1
else echo "clean"; fi

echo "== authors AND committers, name and email =="
# %cn/%ce as well as %an/%ae: a committer trailer is attribution too.
if git log --format='%an <%ae>%n%cn <%ce>' | sort -u | grep -viE '^sudo-megas <'; then
    echo "FAIL: a commit from another identity"; status=1
else echo "clean"; fi

exit $status
