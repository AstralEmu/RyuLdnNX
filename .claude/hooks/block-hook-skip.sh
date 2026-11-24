#!/usr/bin/env bash
# PreToolUse(Bash) — hard block on ANY attempt to skip git hooks / verification gates.
# Doctrine (doctrinal-integrity): never fake or skip a check. NO justification is valid.
# Forbidden: --no-verify, -n (on commit/push/merge/rebase), --no-hooks, SKIP_DOCS_GATE,
# HUSKY=0 / HUSKY_SKIP_HOOKS, and any core.hooksPath override that disables hooks.
set -euo pipefail

input="$(cat)"
cmd="$(printf '%s' "$input" | python3 -c 'import json,sys
try:
    print(json.load(sys.stdin).get("tool_input", {}).get("command", ""))
except Exception:
    pass')"

# Fallback: if command extraction failed, scan the raw payload.
[ -z "$cmd" ] && cmd="$input"

# Strip shell-quoted substrings ('...' and "...") so that a commit MESSAGE or an echo
# that merely MENTIONS a forbidden flag is not blocked — only an actual, unquoted flag is.
scan="$(printf '%s' "$cmd" | sed -E "s/'[^']*'//g; s/\"[^\"]*\"//g")"

if printf '%s' "$scan" | grep -Eiq -- '(--no-verify|--no-hooks|SKIP_DOCS_GATE|HUSKY=0|HUSKY_SKIP_HOOKS|core\.hooksPath|git[^|&;]*\b(commit|push|merge|rebase)\b[^|&;]*[[:space:]]-n([[:space:]]|$))'; then
  echo "BLOCKED by .claude/hooks/block-hook-skip.sh: skipping git hooks / verification gates is forbidden (doctrinal-integrity — never fake or skip a check; NO justification is valid). Remove --no-verify / -n / --no-hooks / SKIP_DOCS_GATE / HUSKY=0 / core.hooksPath. Make the code PASS the hook (make gate / make integration), or hand the commit to the human." >&2
  exit 2
fi
exit 0
