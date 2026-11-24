#!/usr/bin/env python3
"""Fail when a source line is covered by neither unit nor integration tests.

Everything the unit tests cannot reach (or only reach through a mock) has to
be exercised by the integration tests; a line that no run executes is an
error, not an allowance. ``gcovr`` merges every ``.gcda`` found under the
root, so running the unit suite and the integration suite before this check
yields the union of both.

Two things are enforced for every tracked ``sysmodule/source`` ``.c``/``.cpp``
file:

  * the file is measured at all (a file no test binary links is an error, it
    is never silently skipped);
  * its line coverage reaches ``--min`` (default 100).

Usage:
    python3 scripts/check-coverage-surface.py [--min 100] [--root .]

Exit status: 0 when every file passes, 1 on a coverage failure, 2 when the
tooling itself fails.
"""

import argparse
import json
import subprocess
import sys

_SURFACE = "sysmodule/source/"
_SUFFIXES = (".c", ".cpp")


def tracked_sources(root: str) -> list[str]:
    """List the tracked compilation units of the checked surface.

    Args:
        root: Repository root passed to git.

    Returns:
        Repo-relative paths of the tracked ``.c``/``.cpp`` files.

    Raises:
        RuntimeError: If git cannot list the files.
    """
    result = subprocess.run(
        ["git", "-C", root, "ls-files", "--", _SURFACE],
        capture_output=True,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        raise RuntimeError(f"git ls-files failed: {result.stderr.strip()}")

    return sorted(
        path for path in result.stdout.splitlines() if path.endswith(_SUFFIXES)
    )


def measured(root: str) -> dict[str, tuple[int, list[int]]]:
    """Read the merged line coverage of every instrumented source.

    Args:
        root: Repository root handed to gcovr.

    Returns:
        Per repo-relative path: (executable lines, lines never executed).

    Raises:
        RuntimeError: If gcovr fails or prints something unreadable.
    """
    result = subprocess.run(
        [
            "gcovr",
            "--merge-mode-functions=merge-use-line-max",
            f"--root={root}",
            "--gcov-ignore-errors=no_working_dir_found",
            "--json",
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        raise RuntimeError(f"gcovr failed: {result.stderr.strip()}")

    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"unreadable gcovr output: {error}") from error

    out: dict[str, tuple[int, list[int]]] = {}

    for entry in data.get("files", []):
        name = entry.get("file") or entry.get("filename", "")

        if _SURFACE not in name:
            continue

        lines = entry.get("lines", [])
        missed = [line["line_number"] for line in lines if line["count"] == 0]
        out[_SURFACE + name.split(_SURFACE, 1)[1]] = (len(lines), missed)

    return out


def main() -> int:
    """Run the check and print every failure.

    Returns:
        The process exit status.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--min", type=float, default=100.0)
    parser.add_argument("--root", default=".")
    args = parser.parse_args()

    try:
        sources = tracked_sources(args.root)
        data = measured(args.root)
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)

        return 2

    failed = 0

    for path in sources:
        if path not in data:
            print(f"FAIL: {path} is not measured by any test")
            failed += 1

            continue

        total, missed = data[path]
        rate = 100.0 * (total - len(missed)) / total if total else 100.0

        if rate < args.min:
            shown = ", ".join(str(line) for line in missed[:20])
            print(
                f"FAIL: {path} {rate:.1f}% (need {args.min:g}%), "
                f"not executed: {shown}"
            )
            failed += 1

    print(f"Checked {len(sources)} source files at {args.min:g}%")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
