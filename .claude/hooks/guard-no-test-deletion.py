#!/usr/bin/env python3
"""PreToolUse guard — hard block on deleting/weakening a test to pass a check.

Doctrine (never delete tests to pass a check, ZERO TOLERANCE): a test file, a
test case, or a single assertion is NEVER removed to turn a check (lint, type,
coverage) green. When a test trips a rule, rewrite the assertion compliantly or
fix the code — keep the coverage.

Tests of this repository are the host C++ suites ``tests/*_tests.cpp``. The hook
inspects the pending tool call (JSON on stdin) and blocks (exit 2):

  * Bash: ``rm`` / ``unlink`` / ``git rm`` / ``git clean`` aimed at a test file
    or the ``tests/`` directory, or a redirection truncating a test file.
  * Edit / MultiEdit: a replacement that drops test cases or assertions from a
    test file (net decrease within the edit).
  * Write: new content for an existing test file with fewer test cases or
    assertions than the file on disk.

A rename keeps the counts equal (old name removed, new name added in the same
edit) and passes. A test that must go because its source was removed is deleted
by the human, not edited away silently.
"""

from __future__ import annotations

import json
import os
import re
import sys

# A test case is a TEST(...)/TEST_CASE(...)-style macro or a ``test_*`` function
# used by the suites' own lightweight harness; an assertion is any ASSERT_/EXPECT_/
# CHECK/REQUIRE/assert( call.
_TEST_DEF = re.compile(
    r"^\s*(?:TEST(?:_CASE|_F)?\s*\(|void\s+test_\w+\s*\()", re.MULTILINE)
_ASSERT = re.compile(
    r"^\s*(?:ASSERT_\w+|EXPECT_\w+|CHECK(?:_\w+)?|REQUIRE(?:_\w+)?|assert)\s*\(",
    re.MULTILINE)
_TEST_FILE = re.compile(r"(?:^|/)tests/[^/]+\.(?:cpp|hpp|h)$")


def _is_test_path(path: str) -> bool:
    """Whether a path is a C++ test source under tests/."""
    return bool(path) and bool(_TEST_FILE.search(path.replace(os.sep, "/")))


def _counts(text: str) -> tuple[int, int]:
    """Return the (test-case, assertion) counts in a chunk of text."""
    return len(_TEST_DEF.findall(text)), len(_ASSERT.findall(text))


def _block(message: str) -> None:
    """Print the block reason to stderr and exit 2 (denies the tool call)."""
    sys.stderr.write(
        "BLOCKED by .claude/hooks/guard-no-test-deletion.py: " + message
        + " Doctrine (ZERO TOLERANCE): never delete or weaken a test to pass a "
        "check. Rewrite the assertion so it still proves the same thing, or fix "
        "the code — keep the coverage. If a test must go because its source was "
        "removed, the human does it.\n")
    sys.exit(2)


def _guard_bash(command: str) -> None:
    """Block a shell command that removes or truncates a test file."""
    scan = re.sub(r"'[^']*'", "", command)
    scan = re.sub(r'"[^"]*"', "", scan)

    removes = (re.search(r"\b(?:rm|unlink)\b", scan)
               or re.search(r"\bgit\s+(?:rm|clean)\b", scan))
    hits_test = re.search(r"tests/[\w.*-]*(?:_tests\.cpp|\.cpp|\.hpp)?|/tests\b",
                          scan)

    if removes and hits_test:
        _block("removing a test file/dir via the shell is forbidden.")

    if re.search(r">\s*\S*tests/[\w.-]+\.(?:cpp|hpp|h)", scan):
        _block("truncating/overwriting a test file via redirection is "
               "forbidden.")


def _guard_edit(old: str, new: str) -> None:
    """Block an edit that drops test cases or assertions from a test."""
    old_tests, old_asserts = _counts(old)
    new_tests, new_asserts = _counts(new)

    if new_tests < old_tests or new_asserts < old_asserts:
        _block("this edit removes %d test case(s) and %d assertion(s) from a "
               "test file." % (max(0, old_tests - new_tests),
                               max(0, old_asserts - new_asserts)))


def _guard_write(path: str, content: str) -> None:
    """Block a write that reduces an existing test file's tests/assertions."""
    try:
        with open(path, encoding="utf-8") as handle:
            old = handle.read()
    except OSError:
        return

    _guard_edit(old, content)


def main() -> None:
    """Read the tool call and dispatch to the matching guard."""
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        sys.exit(0)

    if not isinstance(payload, dict):
        sys.exit(0)

    tool = payload.get("tool_name", "")
    tool_input = payload.get("tool_input", {}) or {}

    if tool == "Bash":
        _guard_bash(tool_input.get("command", "") or "")
        sys.exit(0)

    path = tool_input.get("file_path", "") or ""

    if not _is_test_path(path):
        sys.exit(0)

    if tool == "Edit":
        _guard_edit(tool_input.get("old_string", "") or "",
                    tool_input.get("new_string", "") or "")
    elif tool == "MultiEdit":
        for edit in tool_input.get("edits", []) or []:
            _guard_edit(edit.get("old_string", "") or "",
                        edit.get("new_string", "") or "")
    elif tool == "Write":
        _guard_write(path, tool_input.get("content", "") or "")

    sys.exit(0)


if __name__ == "__main__":
    main()
