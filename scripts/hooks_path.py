#!/usr/bin/env python3
"""Refuse a git hooks path that does not resolve to THIS repo's hooks/.

Git is silent about a hooks directory that does not exist or belongs to another
clone: every hook (doc-sync, tests, wiki publication) is then off without a
single signal. This reads the effective value (local over global, as git
resolves it) and fails loudly. Unset is refused unless CI is set (a CI checkout
is never wired by `make hooks` and commits nothing).
"""

import os
import subprocess
import sys


def git(repo, *args):
    done = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, check=False)
    return done.returncode, done.stdout.strip()


def problem(repo, env):
    code, top = git(repo, "rev-parse", "--show-toplevel")
    if code != 0:
        return "not inside a git repository"
    top = os.path.realpath(top)
    expected = os.path.join(top, "hooks")
    code, value = git(repo, "config", "--get", "core.hooksPath")
    if code != 0 or not value:
        return None if env.get("CI") else "no hooks path is set: no hook of this repo will run"
    resolved = os.path.realpath(os.path.join(top, os.path.expanduser(value)))
    if resolved != expected:
        return f"hooks path {value!r} resolves to {resolved}, not {expected}"
    if not os.path.isdir(resolved):
        return f"hooks path {resolved} does not exist"
    return None


def main(argv):
    repo = argv[0] if argv else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    reason = problem(repo, dict(os.environ))
    if reason is None:
        return 0
    sys.stderr.write(f"✗ git hooks are NOT wired to this repo: {reason}.\n  Run `make hooks`, then retry.\n")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
