"""Formatting and documentation gate for every C/C++ file of the project.

``check`` prints ``path:line: message`` for every violation and exits non-zero:

  * **format** -- the file differs from ``clang-format`` output (rules in
    ``.clang-format``: 80 columns, 4 spaces);
  * **line length** -- a line is wider than 80 columns (comments and strings
    are not reflowed by clang-format, so they are checked separately);
  * **documentation** -- Doxygen reports an undocumented member, an
    undocumented parameter or return value, or a malformed comment. Everything
    is documented, private members and tests included.

``format`` rewrites the files in place with ``clang-format``; ``--dry-run``
only lists the files it would change. Nothing here is a special case: a file
is either clean or listed.

Run through ``docker compose run --rm style`` (``make style``) so the
clang-format version is the one pinned in ``lint.Dockerfile``; the script
itself works anywhere both tools are installed.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_COLUMNS = 80
_SUFFIXES = (".c", ".cpp", ".h", ".hpp")
# Tracked paths that make up the checked surface (git decides what is
# vendored: submodules and build output are never listed by ``git ls-files``).
_PATHSPECS = ("sysmodule/source", "overlay/source", "tests")
# Doxygen entry points: the checked surface minus fixtures that are data.
_EXCLUDE_PATTERNS = "*/Atmosphere-libs/* */build/* */sdmc:/*"
# Project-specific Doxygen tags used in the sources. ``@gdb{...}`` is parsed
# by scripts/gdb_codegen.py (free-form ``key="value"`` list, one alias per
# arity).
_ALIASES = (
    r"gdb{1}=\par GDB tracepoint^^\1",
    r"gdb{2}=\par GDB tracepoint^^\1, \2",
    r"gdb{3}=\par GDB tracepoint^^\1, \2, \3",
    r"license=\par License^^",
    r"role=\par Role^^",
    r"modified_by=\par Modified by^^",
    r"thread_safety=\par Thread safety^^",
)


class Style:
    """Run the formatting and documentation checks over the tracked sources."""

    def __init__(self) -> None:
        """Create the checker (holds no state)."""

    @staticmethod
    def files() -> list[str]:
        """List the tracked C/C++ files of the checked surface.

        Returns:
            Repo-relative paths, sorted.

        Raises:
            RuntimeError: If git cannot list the files.
        """
        result = subprocess.run(
            ["git", "ls-files", "--", *_PATHSPECS],
            cwd=_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        if result.returncode != 0:
            raise RuntimeError(f"git ls-files failed: {result.stderr.strip()}")

        return sorted(
            path
            for path in result.stdout.splitlines()
            if path.endswith(_SUFFIXES)
            and os.path.isfile(os.path.join(_ROOT, path))
        )

    @staticmethod
    def _need(tool: str) -> str:
        """Return the path of a required tool.

        Args:
            tool: The executable name.

        Returns:
            The resolved path.

        Raises:
            RuntimeError: If the tool is not installed.
        """
        path = shutil.which(tool)

        if path is None:
            raise RuntimeError(f"{tool} is not installed")

        return path

    @staticmethod
    def format_violations(files: list[str]) -> list[str]:
        """Report the files that clang-format would change.

        Args:
            files: Repo-relative paths to check.

        Returns:
            One ``path:1: ...`` line per file that is not formatted.
        """
        tool = Style._need("clang-format")
        out: list[str] = []

        for path in files:
            result = subprocess.run(
                [tool, "--dry-run", "--Werror", path],
                cwd=_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )

            if result.returncode != 0:
                first = re.search(r"^[^:]+:(\d+):", result.stderr, re.M)
                line = first.group(1) if first else "1"
                out.append(f"{path}:{line}: not clang-formatted")

        return out

    @staticmethod
    def width_violations(files: list[str]) -> list[str]:
        """Report every line wider than the column limit.

        Args:
            files: Repo-relative paths to check.

        Returns:
            One ``path:line: ...`` entry per over-wide line.
        """
        out: list[str] = []

        for path in files:
            with open(os.path.join(_ROOT, path), encoding="utf-8") as handle:
                for number, text in enumerate(handle, start=1):
                    width = len(text.rstrip("\n"))

                    if width > _COLUMNS:
                        out.append(
                            f"{path}:{number}: line is {width} columns "
                            f"(limit {_COLUMNS})"
                        )

        return out

    @staticmethod
    def _doxyfile(files: list[str], log: str, out: str) -> str:
        """Return the Doxygen configuration of the completeness check.

        Args:
            files: Repo-relative paths to analyse.
            log: Path of the warning log Doxygen writes.
            out: Scratch output directory.

        Returns:
            The configuration text, fed on stdin.
        """
        return "\n".join([
            "INPUT = " + " ".join(files),
            f"EXCLUDE_PATTERNS = {_EXCLUDE_PATTERNS}",
            "ALIASES = " + " ".join(f'"{alias}"' for alias in _ALIASES),
            "EXTRACT_ALL = NO",
            "EXTRACT_PRIVATE = YES",
            "EXTRACT_STATIC = YES",
            "EXTRACT_ANON_NSPACES = YES",
            "EXTRACT_LOCAL_CLASSES = YES",
            "HIDE_UNDOC_MEMBERS = NO",
            "GENERATE_XML = YES",
            # Doxygen 1.9.x (the lint image) only records @param/@return as
            # documented while a rendering backend runs: with HTML off it
            # reports every fully documented function. The output goes to the
            # scratch directory and is discarded.
            "GENERATE_HTML = YES",
            "GENERATE_LATEX = NO",
            "XML_PROGRAMLISTING = NO",
            "WARN_IF_UNDOCUMENTED = YES",
            "WARN_NO_PARAMDOC = YES",
            "WARN_IF_DOC_ERROR = YES",
            "WARN_IF_INCOMPLETE_DOC = YES",
            "WARN_AS_ERROR = NO",
            f"WARN_LOGFILE = {log}",
            "WARN_FORMAT = \"$file:$line: $text\"",
            "QUIET = YES",
            f"OUTPUT_DIRECTORY = {out}",
            "",
        ])

    @staticmethod
    def doc_violations(files: list[str]) -> list[str]:
        """Report what Doxygen finds undocumented or malformed.

        Args:
            files: Repo-relative paths to analyse.

        Returns:
            One entry per Doxygen warning, with the root stripped.

        Raises:
            RuntimeError: If Doxygen fails to run.
        """
        tool = Style._need("doxygen")

        with tempfile.TemporaryDirectory() as scratch:
            log = os.path.join(scratch, "warnings.log")
            result = subprocess.run(
                [tool, "-"],
                cwd=_ROOT,
                input=Style._doxyfile(files, log, scratch),
                capture_output=True,
                text=True,
                check=False,
            )

            if result.returncode != 0:
                raise RuntimeError(f"doxygen failed: {result.stderr.strip()}")

            if not os.path.exists(log):
                return []

            with open(log, encoding="utf-8") as handle:
                lines = handle.read().splitlines()

        prefix = _ROOT + os.sep

        return sorted({line.replace(prefix, "") for line in lines if line})

    @staticmethod
    def run_check() -> int:
        """Print every violation and return how many there are.

        Returns:
            The number of violations.
        """
        files = Style.files()
        issues = (
            Style.format_violations(files)
            + Style.width_violations(files)
            + Style.doc_violations(files)
        )

        for line in issues:
            print(line)

        return len(issues)

    @staticmethod
    def run_format(dry_run: bool) -> int:
        """Format the files in place (or only list them with ``dry_run``).

        Args:
            dry_run: When true, write nothing.

        Returns:
            The number of files that are (or would be) changed.
        """
        files = Style.files()
        changed = Style.format_violations(files)
        tool = Style._need("clang-format")

        for entry in changed:
            path = entry.split(":", 1)[0]

            if dry_run:
                print(path)

                continue

            subprocess.run([tool, "-i", path], cwd=_ROOT, check=True)

        return len(changed)


def main() -> int:
    """Dispatch to ``check`` (default) or ``format``.

    Returns:
        The process exit status: 1 when ``check`` finds a violation.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("check", help="list every violation (default)")
    fmt = sub.add_parser("format", help="clang-format the files in place")
    fmt.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.command == "format":
        Style.run_format(args.dry_run)

        return 0

    return 1 if Style.run_check() else 0


if __name__ == "__main__":
    sys.exit(main())
