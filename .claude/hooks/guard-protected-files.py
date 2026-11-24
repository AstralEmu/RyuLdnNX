#!/usr/bin/env python3
"""PreToolUse guard — the gates are never edited by the agent.

A gate (git hook, coverage checker, agent guard, agent settings) that fails is
fixed by fixing the code or the tests it judges, never by changing the gate.
Editing the gate to get a commit through is a bypass, whatever the reason.

The hook inspects the pending tool call (JSON on stdin) and blocks (exit 2):

  * Edit / MultiEdit / Write / NotebookEdit aimed at a protected path.
  * Bash: a command that names a protected path together with a write
    operation (redirection, sed -i, tee, mv, cp, rm, chmod, ln, truncate,
    patch, an interpreter writing, git checkout/restore/apply/stash/reset/
    revert/rm/mv/clean). Reading (cat, grep, git diff, git add, make) passes.

Protected: hooks/, .claude/hooks/, .claude/settings.json,
.claude/settings.local.json, scripts/check-coverage.py,
scripts/hooks_path.py, .git/hooks, the wiki's .git/hooks and
.git/config. Only the human changes them.
"""

from __future__ import annotations

import json
import os
import re
import sys

_PROTECTED = re.compile(
    r"(?:^|[\s'\"=:/])(?:\./)?"
    r"(?:hooks/|\.claude/hooks/|\.claude/settings(?:\.local)?\.json"
    r"|scripts/check-coverage\.py|scripts/hooks_path\.py"
    r"|(?:\.git|docs/\.git|[\w./-]*\.wiki/\.git)/(?:hooks|config)\b)")

_WRITE = re.compile(
    r"(?:>>?|\btee\b|\bsed\b[^|;&]*\s-[A-Za-z]*i|\bperl\b[^|;&]*\s-[A-Za-z]*i"
    r"|\b(?:mv|cp|rm|unlink|chmod|chown|ln|truncate|patch|install|dd|rsync|touch)\b"
    r"|\bgit\s+(?:checkout|restore|apply|am|stash|reset|revert|rm|mv|clean|config)\b"
    r"|\.write(?:_text|_bytes)?\s*\(|\bopen\s*\([^)]*['\"][wa+]"
    r"|\bsubprocess\b|\bos\.(?:remove|rename|replace|chmod|unlink)\b"
    r"|\bshutil\.|\bsponge\b)")

_PATH = re.compile(
    r"^(?:.*/)?(?:hooks/[^/].*|\.claude/hooks/.*|\.claude/settings(?:\.local)?\.json"
    r"|scripts/check-coverage\.py|scripts/hooks_path\.py)$")


def _block(what: str) -> None:
    """Print the block reason to stderr and exit 2 (denies the tool call)."""
    sys.stderr.write(
        "BLOCKED by .claude/hooks/guard-protected-files.py: " + what + " "
        "The gates (git hooks, coverage checker, agent guards, agent settings) "
        "are never edited by the agent: a failing gate is fixed by fixing the "
        "code or the tests it judges. Report the failure and stop; the human "
        "changes the gate if it must change.\n")
    sys.exit(2)


def _rel(path: str, root: str) -> str:
    """Normalise a tool path to a repo-relative POSIX path when inside the repo."""
    full = os.path.realpath(path if os.path.isabs(path) else os.path.join(root, path))
    base = os.path.realpath(root)
    if full == base or full.startswith(base + os.sep):
        return os.path.relpath(full, base).replace(os.sep, "/")
    return full.replace(os.sep, "/")


def main() -> None:
    """Read the pending tool call from stdin and apply the guard."""
    try:
        event = json.load(sys.stdin)
    except (ValueError, OSError):
        # Unreadable input on a guard: refuse rather than let it through.
        _block("the tool call could not be parsed.")
        return

    tool = event.get("tool_name", "")
    args = event.get("tool_input") or {}
    root = os.environ.get("CLAUDE_PROJECT_DIR") or event.get("cwd") or os.getcwd()

    if tool in ("Edit", "MultiEdit", "Write", "NotebookEdit"):
        path = args.get("file_path") or args.get("notebook_path") or ""
        if path and _PATH.match(_rel(path, root)):
            _block("editing the protected file %s is forbidden." % _rel(path, root))
        return

    if tool == "Bash":
        command = args.get("command", "")
        if _PROTECTED.search(command) and _WRITE.search(command):
            _block("this command writes to a protected gate file.")


if __name__ == "__main__":
    main()
