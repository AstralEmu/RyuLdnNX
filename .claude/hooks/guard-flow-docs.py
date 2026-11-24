#!/usr/bin/env python3
"""PreToolUse guard — the dev flow cannot change without its documentation.

The dev flow is documented in ONE wiki page (``Handoff-Dev-Flow.md``, reached
through the untracked ``docs`` symlink) and mirrored by the ``dev-flow`` skill
(``.claude/skills/dev-flow/SKILL.md``). A commit that changes the machinery of
the flow must move that documentation, or the page rots.

For a ``git commit`` Bash call this blocks (exit 2) when the staged files
include a FLOW file:

  * ``Makefile``
  * anything under ``hooks/``, ``.github/workflows/``, ``scripts/ci/`` or
    ``.claude/hooks/``
  * ``docker-compose*.yml`` at the repo root

unless the dev-flow skill is staged too, or the wiki page ``Handoff-Dev-Flow.md``
changed since the previous commit (committed after it, or modified in the wiki
working tree). The tracked ``hooks/pre-commit`` enforces the same rule at commit
time; this guard fails earlier, with the same message, before the commit runs.

Fail-closed on a matching commit: when the staged files cannot be read the
commit is BLOCKED. Anything that is not a commit of flow files is untouched.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys

_SKILL = ".claude/skills/dev-flow/SKILL.md"
_PAGE = "Handoff-Dev-Flow.md"
_EXACT = {"Makefile"}
_PREFIXES = ("hooks/", ".github/workflows/", "scripts/ci/", ".claude/hooks/")
_COMPOSE = re.compile(r"^docker-compose[^/]*\.yml$")


def _block(message: str) -> None:
    """Print the block reason to stderr and exit 2 (denies the tool call)."""
    sys.stderr.write(
        "BLOCKED by .claude/hooks/guard-flow-docs.py: " + message + "\n")
    sys.exit(2)


def _run(args: list[str]) -> str | None:
    """Run a command; its stdout, or None when it cannot be run/read."""
    try:
        out = subprocess.run(
            args, capture_output=True, text=True, timeout=10, check=False)
    except (OSError, subprocess.SubprocessError):
        return None

    return out.stdout if out.returncode == 0 else None


def _is_flow_file(path: str) -> bool:
    """Whether a repo-relative path belongs to the dev-flow machinery."""
    return (path in _EXACT or path.startswith(_PREFIXES)
            or bool(_COMPOSE.match(path)))


def _page_moved(root: str) -> bool:
    """Whether the wiki Dev-Flow page changed since the previous commit."""
    wiki = os.path.join(root, "docs")

    if not os.path.exists(os.path.join(wiki, ".git")):
        return False

    dirty = _run(["git", "-C", wiki, "status", "--porcelain", "--", _PAGE])

    if dirty and dirty.strip():
        return True

    previous = _run(["git", "-C", root, "log", "-1", "--format=%ct", "HEAD"])
    stamp = _run(["git", "-C", wiki, "log", "-1", "--format=%ct", "--", _PAGE])

    if not previous or not stamp:
        return False

    return int(stamp) > int(previous)


def _guard_commit(root: str) -> None:
    """Block a commit of flow files that does not move the flow docs."""
    staged = _run(["git", "-C", root, "diff", "--cached", "--name-only"])

    if staged is None:
        _block("cannot read the staged files of this commit (git diff failed) "
               "— a commit that may touch the dev flow cannot be verified "
               "(fail-closed).")

    files = staged.split()
    flow = sorted({path for path in files if _is_flow_file(path)})

    if not flow or _SKILL in files or _page_moved(root):
        return

    _block(
        "this commit changes the dev flow (%s) without its documentation. "
        "Edit the wiki page %s (inside docs/) and/or stage %s, then retry."
        % (", ".join(flow), _PAGE, _SKILL))


def main() -> None:
    """Read the tool call and guard Bash ``git commit`` of flow files."""
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        sys.exit(0)

    if not isinstance(payload, dict) or payload.get("tool_name") != "Bash":
        sys.exit(0)

    tool_input = payload.get("tool_input")
    command = tool_input.get("command") if isinstance(tool_input, dict) else ""

    if not isinstance(command, str):
        sys.exit(0)

    scan = re.sub(r"'[^']*'", "''", command)
    scan = re.sub(r'"[^"]*"', '""', scan)

    if not re.search(r"(?:^|[;&|]\s*)git\s+(?:-\S+\s+\S+\s+)*commit\b", scan):
        sys.exit(0)

    top = _run(["git", "rev-parse", "--show-toplevel"])

    if top is None:
        _block("cannot resolve the repository of this commit (fail-closed).")

    root = top.strip()
    superproject = _run(
        ["git", "-C", root, "rev-parse", "--show-superproject-working-tree"])

    if superproject and superproject.strip():
        sys.exit(0)

    _guard_commit(root)
    sys.exit(0)


if __name__ == "__main__":
    main()
