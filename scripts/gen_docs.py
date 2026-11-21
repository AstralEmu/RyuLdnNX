"""Build the GitHub wiki under ``docs/`` — everything generable, nothing prose.

Deterministic. The C++ reference is produced from **Doxygen** XML (the real
docstring extractor: it parses the sources, this script only renders its XML as
Markdown), one flat page per source file. The navigation (``_Sidebar.md``,
``_Footer.md``, the table of contents in ``Home.md`` and ``Design.md``) is
generated so the wiki is browsable.

Two hard constraints shape the layout:
  * **GitHub wikis are flat** — subdirectories are not navigable, so every page
    lives at the wiki root with a flat name (``Reference-ldn.ldn_state.hpp``,
    ``Handoff-Changes`` ...).
  * **Prose is never generated** — inside an otherwise hand-written page, only
    the region between ``<!-- gen:NAME start -->`` / ``<!-- gen:NAME end -->``
    is replaced; prose outside the markers is preserved.

Doxygen runs on the host when installed, otherwise in the ``docs`` compose
service (the dev image ships it). Run by ``make docs``; the pre-commit hook
refuses the commit when the result is not committed in the wiki.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_WIKI = os.path.join(_ROOT, "docs")
_REF_PREFIX = "Reference-"
# Source roots documented by Doxygen; the first path segment labels the group.
_SOURCES = ("sysmodule/source", "overlay/source")
_FILE_PATTERNS = "*.hpp *.h *.cpp *.c"
_EXCLUDE_PATTERNS = "*/Atmosphere-libs/* */build/*"
# Compose service that runs this script with --xml-only when the host has no
# doxygen, and the wiki-relative scratch dir the XML is written to.
_COMPOSE_SERVICE = "docs"
_XML_DIR = os.path.join(_ROOT, ".cache", "doxygen-xml")

# Hand-written reference pages: pages that no single source file produces.
# They share the ``Reference-`` prefix, so they must be named here or
# ``_prune_ref`` deletes them on the next ``make docs``.
_HANDWRITTEN_REF = (
    "Reference-IPC-Commands.md",
    "Reference-Protocol.md",
)

# Wiki sections: (flat-name prefix, sidebar title, one-line explanation). The
# explanation is generated prose (owned here, not hand-edited in the wiki).
_SECTIONS = (
    (
        "Handoff-",
        "Handoff — live state & impact map",
        "Compact real-time state, the changes log, and the per-module impact "
        "map. Read this first when resuming work.",
    ),
    (
        "Technique-",
        "Technical docs",
        "How the system is built, for a contributor: the architecture, the "
        "file map, configuration and integrations, plus the technical "
        "changelog.",
    ),
    (
        "User-",
        "User docs",
        "User-facing prose and the user changelog, in plain language.",
    ),
    (
        "Design-",
        "Direction — design, vision & roadmap",
        "The project's direction: design notes on non-obvious mechanisms, "
        "the standing decisions, and the planned path.",
    ),
)

# Pages whose durable content is Technique-grade but which KEEP their
# ``Handoff-`` filename (stable resume anchors named in the pre-commit hook and
# the doc-sync skill). They are only RECLASSIFIED in the navigation: one URL
# per page, never a duplicate, and no Handoff file is renamed.
_TECHNIQUE_DISPLAY = (
    "Handoff-Architecture",
    "Handoff-Features",
    "Handoff-File-Map",
    "Handoff-Configuration",
    "Handoff-Integrations",
)

# The Roadmap is the planned direction, not "how it is built": it is displayed
# under the Direction section with a short nav label. Nav-only.
_DIRECTION_ROADMAP = {
    "Roadmap-Critical-Path": "Roadmap",
}

_KIND_ORDER = (
    "define", "typedef", "enum", "friend", "func", "function", "variable",
)


class WikiDocs:
    """Regenerate the C++ reference (Doxygen XML) and the wiki navigation."""

    def __init__(self) -> None:
        """Create the generator (holds no state)."""

    def run(self) -> int:
        """Regenerate the reference and the navigation; return page count.

        Returns:
            The number of source-file reference pages written.
        """
        WikiDocs._doxygen()
        pages = WikiDocs._render_all()

        WikiDocs._prune_ref(pages)
        WikiDocs._write_ref_index(pages)
        WikiDocs._write_design_index()
        WikiDocs._write_sidebar()
        WikiDocs._write_footer()
        WikiDocs._write_home()

        return len(pages)

    # --- Doxygen -------------------------------------------------------

    @staticmethod
    def _config() -> str:
        """Return the Doxygen configuration (fed on stdin, no tracked file).

        Returns:
            The Doxygen configuration text.
        """
        return "\n".join([
            f"INPUT = {' '.join(_SOURCES)}",
            "RECURSIVE = YES",
            f"FILE_PATTERNS = {_FILE_PATTERNS}",
            f"EXCLUDE_PATTERNS = {_EXCLUDE_PATTERNS}",
            "EXTRACT_ALL = YES",
            "EXTRACT_STATIC = YES",
            "EXTRACT_PRIVATE = NO",
            "GENERATE_XML = YES",
            "GENERATE_HTML = NO",
            "GENERATE_LATEX = NO",
            "XML_PROGRAMLISTING = NO",
            "SHOW_INCLUDE_FILES = NO",
            "QUIET = YES",
            "WARN_IF_UNDOCUMENTED = NO",
            f"OUTPUT_DIRECTORY = {_XML_DIR}",
            "XML_OUTPUT = .",
            "",
        ])

    @staticmethod
    def _doxygen() -> None:
        """Produce the Doxygen XML, on the host or in the compose service.

        Raises:
            RuntimeError: If neither a host doxygen nor docker is available, or
                Doxygen fails.
        """
        shutil.rmtree(_XML_DIR, ignore_errors=True)
        os.makedirs(_XML_DIR)

        if shutil.which("doxygen"):
            WikiDocs._run(["doxygen", "-"], WikiDocs._config())

            return

        if not shutil.which("docker"):
            raise RuntimeError(
                "neither doxygen nor docker is installed: cannot build "
                "the reference"
            )

        env = {
            **os.environ,
            "DOCS_UID": str(os.getuid()),
            "DOCS_GID": str(os.getgid()),
        }
        WikiDocs._run(
            ["docker", "compose", "run", "--rm", _COMPOSE_SERVICE], None, env
        )

    @staticmethod
    def _run(
        command: list[str],
        stdin: str | None,
        env: dict[str, str] | None = None,
    ) -> None:
        """Run a command from the repo root, failing loudly.

        Args:
            command: The argv to run.
            stdin: Text fed on standard input, or ``None``.
            env: Environment override, or ``None`` for the current one.

        Raises:
            RuntimeError: If the command exits non-zero.
        """
        result = subprocess.run(
            command,
            cwd=_ROOT,
            input=stdin,
            capture_output=True,
            text=True,
            check=False,
            env=env,
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"{' '.join(command)} failed: {result.stderr.strip()}"
            )

    # --- reference rendering ------------------------------------------

    @staticmethod
    def _render_all() -> dict[str, str]:
        """Render one page per documented source file.

        Returns:
            Page stem (``Reference-...``) mapped to its Markdown text.
        """
        pages: dict[str, str] = {}

        for path in sorted(os.listdir(_XML_DIR)):
            if not path.endswith(".xml") or path == "index.xml":
                continue

            root = ET.parse(os.path.join(_XML_DIR, path)).getroot()
            node = root.find("compounddef")

            if node is None or node.get("kind") != "file":
                continue

            name = WikiDocs._page_name(node)

            if name is None:
                continue

            body = WikiDocs._render_file(node)

            if body is not None:
                pages[name] = body

        for name, body in pages.items():
            WikiDocs._write(os.path.join(_WIKI, f"{name}.md"), body)

        return pages

    @staticmethod
    def _page_name(node: ET.Element) -> str | None:
        """Return the flat page stem for a file compound.

        Args:
            node: The ``compounddef`` of kind ``file``.

        Returns:
            ``Reference-<root>.<path>`` with dots for slashes, or ``None`` when
            the file lies outside every documented source root.
        """
        location = node.find("location")

        if location is None:
            return None

        rel = os.path.relpath(location.get("file", ""), _ROOT) if os.path.isabs(
            location.get("file", "")
        ) else location.get("file", "")

        for source in _SOURCES:
            if rel.startswith(source + "/"):
                label = source.split("/", 1)[0]
                tail = rel[len(source) + 1:].replace("/", ".")

                return f"{_REF_PREFIX}{label}.{tail}"

        return None

    @staticmethod
    def _render_file(node: ET.Element) -> str | None:
        """Render one file compound as a Markdown page.

        Args:
            node: The ``compounddef`` of kind ``file``.

        Returns:
            The page text, or ``None`` when the file declares nothing.
        """
        location = node.find("location")
        rel = location.get("file", "") if location is not None else ""
        lines = [f"# `{rel}`", ""]
        intro = WikiDocs._doc(node)

        if intro:
            lines.extend([intro, ""])

        members = WikiDocs._members(node)
        classes = WikiDocs._classes(node)

        if not members and not classes:
            return None

        for compound in classes:
            lines.extend(WikiDocs._render_class(compound))

        if members:
            lines.extend(["## Functions and declarations", ""])
            lines.extend(members)

        return "\n".join(lines).rstrip() + "\n"

    @staticmethod
    def _classes(node: ET.Element) -> list[ET.Element]:
        """Return the class/struct compounds declared by a file.

        Args:
            node: The ``compounddef`` of kind ``file``.

        Returns:
            The referenced ``compounddef`` nodes, ordered by name.
        """
        found: list[ET.Element] = []

        for inner in node.findall("innerclass"):
            ref = inner.get("refid", "")
            path = os.path.join(_XML_DIR, f"{ref}.xml")

            if not os.path.exists(path):
                continue

            compound = ET.parse(path).getroot().find("compounddef")

            if compound is not None:
                found.append(compound)

        return sorted(found, key=lambda c: c.findtext("compoundname", ""))

    @staticmethod
    def _render_class(compound: ET.Element) -> list[str]:
        """Render one class or struct compound.

        Args:
            compound: The class ``compounddef``.

        Returns:
            The Markdown lines for the class.
        """
        kind = compound.get("kind", "class")
        name = compound.findtext("compoundname", "")
        lines = [f"## {kind} `{name}`", ""]
        text = WikiDocs._doc(compound)

        if text:
            lines.extend([text, ""])

        lines.extend(WikiDocs._members(compound))

        return lines

    @staticmethod
    def _members(node: ET.Element) -> list[str]:
        """Render every public member definition of a compound.

        Args:
            node: A ``compounddef`` (file or class).

        Returns:
            The Markdown lines, one ``###`` block per documented member.
        """
        lines: list[str] = []

        for section in node.findall("sectiondef"):
            if section.get("kind", "").startswith(("private", "protected")):
                continue

            for member in section.findall("memberdef"):
                lines.extend(WikiDocs._render_member(member))

        return lines

    @staticmethod
    def _render_member(member: ET.Element) -> list[str]:
        """Render one ``memberdef`` as a block with its signature and docs.

        Args:
            member: The ``memberdef`` element.

        Returns:
            The Markdown lines for the member.
        """
        name = member.findtext("name", "")
        kind = member.get("kind", "")
        signature = WikiDocs._signature(member)
        lines = [f"### `{name}`", "", "```cpp", signature, "```", ""]
        text = WikiDocs._doc(member)

        if text:
            lines.extend([text, ""])

        params = WikiDocs._params(member)

        if params:
            lines.extend(["**Parameters**", "", *params, ""])

        returns = WikiDocs._simplesect(member, "return")

        if returns:
            lines.extend([f"**Returns:** {returns}", ""])

        if kind == "enum":
            values = [
                f"- `{v.findtext('name', '')}`"
                + (
                    f" — {WikiDocs._text(v.find('briefdescription'))}"
                    if WikiDocs._text(v.find("briefdescription")) else ""
                ) for v in member.findall("enumvalue")
            ]
            lines.extend([*values, ""] if values else [])

        return lines

    @staticmethod
    def _signature(member: ET.Element) -> str:
        """Build a one-line C++ signature from a ``memberdef``.

        Args:
            member: The ``memberdef`` element.

        Returns:
            The signature text.
        """
        kind = member.get("kind", "")
        name = member.findtext("name", "")
        rtype = WikiDocs._text(member.find("type"))
        args = member.findtext("argsstring", "") or ""

        if kind == "define":
            params = ", ".join(
                WikiDocs._text(p) for p in member.findall("param")
            )
            initial = WikiDocs._text(member.find("initializer"))
            head = f"#define {name}" + (f"({params})" if params else "")

            return f"{head} {initial}".strip()

        if kind == "typedef":
            return f"{WikiDocs._text(member.find('definition'))}{args}".strip()

        if kind == "enum":
            return f"enum {name}"

        return f"{rtype} {name}{args}".strip()

    @staticmethod
    def _params(member: ET.Element) -> list[str]:
        """Return the documented parameters as bullet lines.

        Args:
            member: The ``memberdef`` element.

        Returns:
            One ``- `name` — description`` line per documented parameter.
        """
        lines: list[str] = []

        for plist in member.iter("parameterlist"):
            if plist.get("kind") != "param":
                continue

            for item in plist.findall("parameteritem"):
                names = ", ".join(
                    WikiDocs._text(n) for n in item.iter("parametername")
                )
                desc = WikiDocs._text(item.find("parameterdescription"))
                lines.append(f"- `{names}` — {desc}".rstrip(" —"))

        return lines

    @staticmethod
    def _simplesect(
        member: ET.Element,
        kind: str,
    ) -> str:
        """Return the text of the first ``simplesect`` of a kind.

        Args:
            member: The ``memberdef`` element.
            kind: The section kind (``return``, ``note`` ...).

        Returns:
            The section text, or an empty string.
        """
        for sect in member.iter("simplesect"):
            if sect.get("kind") == kind:
                return WikiDocs._text(sect)

        return ""

    @staticmethod
    def _doc(node: ET.Element) -> str:
        """Return a node's brief + detailed description as Markdown.

        Args:
            node: A compound or member element.

        Returns:
            The description text (empty when undocumented).
        """
        brief = WikiDocs._text(node.find("briefdescription"))
        detail = WikiDocs._text(node.find("detaileddescription"))

        if detail.startswith(brief):
            detail = detail[len(brief):].strip()

        return "\n\n".join(part for part in (brief, detail) if part)

    @staticmethod
    def _text(node: ET.Element | None) -> str:
        """Flatten a Doxygen description subtree to Markdown text.

        Args:
            node: The element to flatten (``None`` yields an empty string).

        Returns:
            Inline Markdown: code spans, emphasis, lists; whitespace collapsed
            per paragraph.
        """
        if node is None:
            return ""

        text = WikiDocs._inline(node)
        text = re.sub(r"[ \t]+\n", "\n", text)
        text = re.sub(r"\n{3,}", "\n\n", text)

        return text.strip()

    @staticmethod
    def _inline(node: ET.Element) -> str:
        """Recursively render an element's text and children.

        Args:
            node: The element to render.

        Returns:
            The rendered fragment (children and tails included).
        """
        tag = node.tag
        parts = [node.text or ""]

        for child in node:
            parts.append(WikiDocs._child(child))
            parts.append(child.tail or "")

        body = "".join(parts)

        if tag == "para":
            return re.sub(r"[ \t\n]+", " ", body).strip() + "\n\n"

        if tag in ("computeroutput", "verbatim", "preformatted"):
            return f"`{body.strip()}`" if tag == "computeroutput" else (
                f"\n```\n{body.strip()}\n```\n"
            )

        return body

    @staticmethod
    def _child(child: ET.Element) -> str:
        """Render one child element of a description.

        Args:
            child: The child element.

        Returns:
            Its Markdown rendering (skipped constructs yield nothing).
        """
        tag = child.tag

        if tag in ("parameterlist", "simplesect") and (
            tag == "parameterlist" or child.get("kind") == "return"
        ):
            return ""

        if tag == "bold":
            return f"**{WikiDocs._inline(child).strip()}**"

        if tag == "emphasis":
            return f"*{WikiDocs._inline(child).strip()}*"

        if tag == "itemizedlist":
            return "\n" + "".join(
                "- " + WikiDocs._text(item) + "\n"
                for item in child.findall("listitem")
            ) + "\n"

        if tag == "orderedlist":
            return "\n" + "".join(
                f"{i}. " + WikiDocs._text(item) + "\n"
                for i, item in enumerate(child.findall("listitem"), 1)
            ) + "\n"

        if tag == "linebreak":
            return "\n"

        if tag == "sp":
            return " "

        if tag == "ulink":
            return f"[{WikiDocs._inline(child)}]({child.get('url', '')})"

        if tag == "simplesect":
            kind = child.get("kind", "note").capitalize()

            return f"\n\n**{kind}:** {WikiDocs._text(child)}\n\n"

        return WikiDocs._inline(child)

    # --- reference index -----------------------------------------------

    @staticmethod
    def _prune_ref(pages: dict[str, str]) -> None:
        """Delete reference pages whose source file no longer exists.

        Args:
            pages: The page stems produced in this run.
        """
        keep = {f"{name}.md" for name in pages} | set(_HANDWRITTEN_REF)
        stale = [
            os.path.join(_WIKI, name) for name in os.listdir(_WIKI)
            if name.startswith(_REF_PREFIX) and name not in keep
        ]

        WikiDocs._remove(stale)

    @staticmethod
    def _write_ref_index(pages: dict[str, str]) -> None:
        """Write ``Reference.md``: an index grouped by source root and folder.

        Args:
            pages: The page stems produced in this run.
        """
        lines = [
            "# Code reference",
            "",
            "_Generated from the C++ docstrings by `make docs` (Doxygen XML). "
            "Grouped by source root and folder._",
            "",
        ]
        stems = [
            *pages,
            *(name[:-3] for name in _HANDWRITTEN_REF),
        ]

        for group in [s.split("/", 1)[0] for s in _SOURCES] + ["Other"]:
            lines.extend(WikiDocs._group_lines(group, stems))

        WikiDocs._write(
            os.path.join(_WIKI, "Reference.md"),
            "\n".join(lines).rstrip() + "\n"
        )

    @staticmethod
    def _group_lines(
        group: str,
        stems: list[str],
    ) -> list[str]:
        """Render one top-level reference group as a Folder/Pages table.

        Args:
            group: The group (source root label, or ``Other``).
            stems: All page stems.

        Returns:
            The Markdown lines (empty when the group has no page).
        """
        members = [s for s in stems if WikiDocs._group_of(s)[0] == group]

        if not members:
            return []

        folders = sorted({WikiDocs._group_of(s)[1] for s in members})
        rows = [f"## {group}", "", "| Folder | Pages |", "|--------|-------|"]

        for folder in folders:
            links = ", ".join(
                f"[`{WikiDocs._group_of(s)[2]}`]({s})"
                for s in sorted(members) if WikiDocs._group_of(s)[1] == folder
            )
            rows.append(f"| {folder or '(root)'} | {links} |")

        return [*rows, ""]

    @staticmethod
    def _group_of(stem: str) -> tuple[str, str, str]:
        """Classify a page stem into ``(group, folder, leaf label)``.

        Args:
            stem: The page stem (``Reference-...``).

        Returns:
            The source root, its sub-folder (``""`` at root) and the file label.
        """
        parts = stem[len(_REF_PREFIX):].split(".")
        roots = [s.split("/", 1)[0] for s in _SOURCES]

        if parts[0] not in roots or len(parts) < 3:
            return ("Other", "", stem[len(_REF_PREFIX):])

        folder = parts[1] if len(parts) > 3 else ""
        leaf = ".".join(parts[2:] if folder else parts[1:])

        return (parts[0], folder, leaf)

    # --- wiki navigation (what makes it browsable) ---------------------

    @staticmethod
    def _page_title_summary(path: str) -> tuple[str, str]:
        """Read a wiki page's H1 title and its first prose line (a summary).

        Args:
            path: The page file to read.

        Returns:
            The ``(title, summary)``; empty strings on error.
        """
        title = ""
        summary = ""

        try:
            lines = WikiDocs._read(path).splitlines()
        except OSError:
            return title, summary

        for line in lines:
            stripped = line.strip()

            if not title and stripped.startswith("# "):
                title = stripped[2:].strip()
            elif title and stripped and not stripped.startswith("#"):
                summary = stripped
                break

        return title, summary

    @staticmethod
    def _write_design_index() -> None:
        """Write ``Design.md``: the single index of the Design pages."""
        rows: list[str] = []

        for name in sorted(os.listdir(_WIKI)):
            if not (name.startswith("Design-") and name.endswith(".md")):
                continue

            title, summary = WikiDocs._page_title_summary(
                os.path.join(_WIKI, name)
            )
            label = title or name[:-3]
            safe_summary = summary.replace("|", "\\|")
            rows.append(f"| [{label}]({name[:-3]}) | {safe_summary} |")

        lines = [
            "# Design & vision — index",
            "",
            "The single entry point to the project's **direction / design** "
            "axis. One row per page; read the specific page when working on "
            "the matching area.",
            "",
            "| Page | What it holds |",
            "|------|---------------|",
            *rows,
        ]
        WikiDocs._write(
            os.path.join(_WIKI, "Design.md"), "\n".join(lines) + "\n"
        )

    @staticmethod
    def _write_sidebar() -> None:
        """Write ``_Sidebar.md``: the grouped link tree shown on every page."""
        lines = ["## ryu_ldn_nx wiki", "", "[Home](Home)", ""]

        for prefix, title, _desc in _SECTIONS:
            lines.append(f"### {title}")
            if prefix == "Design-":
                lines.append("- [Index](Design)")
                lines.extend(WikiDocs._roadmap_links())
            else:
                lines.extend(WikiDocs._section_links(prefix))
            lines.append("")

        lines.append("### Code reference")
        lines.append("- [Source](Reference)")
        lines.extend(
            f"- [{WikiDocs._title(os.path.join(_WIKI, n), n[:-3])}]({n[:-3]})"
            for n in _HANDWRITTEN_REF
        )

        WikiDocs._write(
            os.path.join(_WIKI, "_Sidebar.md"), "\n".join(lines).rstrip() + "\n"
        )

    @staticmethod
    def _write_footer() -> None:
        """Write ``_Footer.md``: a static generated notice (no timestamp)."""
        text = (
            "_Navigation and code reference generated by `make docs` "
            "(Doxygen XML). Prose is hand-written._\n"
        )
        WikiDocs._write(os.path.join(_WIKI, "_Footer.md"), text)

    @staticmethod
    def _write_home() -> None:
        """Refresh the generated table of contents inside ``Home.md``.

        The prose above the ``toc`` marker is preserved; the table of contents
        between the markers is replaced. A missing ``Home.md`` is seeded.
        """
        lines: list[str] = []

        for prefix, title, desc in _SECTIONS:
            lines.extend([f"### {title}", "", desc, ""])
            if prefix == "Design-":
                lines.append("- [Index](Design) — single entry point")
                lines.extend(WikiDocs._roadmap_links())
            else:
                lines.extend(WikiDocs._section_links(prefix))
            lines.append("")

        lines.extend([
            "### Code reference",
            "",
            "Generated from docstrings — do not edit by hand.",
            "",
            "- [Source code reference](Reference)",
        ])
        lines.extend(
            f"- [{WikiDocs._title(os.path.join(_WIKI, n), n[:-3])}]({n[:-3]})"
            for n in _HANDWRITTEN_REF
        )
        toc = "\n".join(lines).rstrip()

        WikiDocs._replace_region(
            os.path.join(_WIKI, "Home.md"), "toc", toc, _HOME_SEED
        )

    @staticmethod
    def _roadmap_links() -> list[str]:
        """Return the roadmap bullet links shown under the Direction section.

        Returns:
            One ``- [label](page)`` line per ``Roadmap-*`` page, sorted.
        """
        pages = sorted(
            name[:-3] for name in os.listdir(_WIKI)
            if name.startswith("Roadmap-") and name.endswith(".md")
        )

        return [WikiDocs._roadmap_link(stem) for stem in pages]

    @staticmethod
    def _roadmap_link(stem: str) -> str:
        """Return one roadmap bullet: a short label linking to the page.

        Args:
            stem: The roadmap page name (file stem).

        Returns:
            The Markdown bullet line for the page.
        """
        label = _DIRECTION_ROADMAP.get(
            stem, WikiDocs._title(os.path.join(_WIKI, f"{stem}.md"), stem)
        )

        return f"- [{label}]({stem})"

    @staticmethod
    def _section_links(prefix: str) -> list[str]:
        """Return the Markdown bullet links to a prose section's pages.

        Args:
            prefix: The flat-name prefix identifying the section.

        Returns:
            One ``- [Title](Page)`` line per page (a placeholder when empty).
        """
        pages = WikiDocs._pages(prefix)

        if not pages:
            return ["- _(empty)_"]

        return [f"- [{title}]({stem})" for stem, title in pages]

    @staticmethod
    def _pages(prefix: str) -> list[tuple[str, str]]:
        """List a section's pages as ``(page_name, title)``, sorted.

        Args:
            prefix: The flat-name prefix identifying the section.

        Returns:
            One entry per matching top-level Markdown page.
        """
        found = [
            WikiDocs._page_entry(name) for name in os.listdir(_WIKI)
            if name.endswith(".md") and WikiDocs._in_section(name, prefix)
        ]

        return sorted(found)

    @staticmethod
    def _in_section(
        name: str,
        prefix: str,
    ) -> bool:
        """Whether a wiki page belongs to a navigation section.

        Args:
            name: The Markdown file name.
            prefix: The section's flat-name prefix.

        Returns:
            True when the page is listed in the ``prefix`` section.
        """
        if name[:-3] in _TECHNIQUE_DISPLAY:
            return prefix == "Technique-"

        return name.startswith(prefix)

    @staticmethod
    def _page_entry(name: str) -> tuple[str, str]:
        """Build one ``(page_name, title)`` entry for a wiki file.

        Args:
            name: The Markdown file name.

        Returns:
            The page name (file stem) and the page title.
        """
        stem = name[:-3]
        title = WikiDocs._title(os.path.join(_WIKI, name), stem)

        if stem in _TECHNIQUE_DISPLAY:
            title = title.removeprefix("Handoff — ").removeprefix("Handoff ")

        return (stem, title)

    @staticmethod
    def _title(
        path: str,
        stem: str,
    ) -> str:
        """Return a page's title: its first H1, else a prettified name.

        Args:
            path: The Markdown file path.
            stem: The file stem (fallback source).

        Returns:
            The page title.
        """
        for line in WikiDocs._read(path).splitlines():
            if line.startswith("# "):
                return line[2:].strip().strip("`")

        return stem.split("-", 1)[-1].replace("_", " ").replace("-", " ")

    # --- marked-region replacement (prose preserved) -------------------

    @staticmethod
    def _replace_region(
        path: str,
        name: str,
        content: str,
        seed: str,
    ) -> None:
        """Replace a file's ``gen:NAME`` region, preserving prose around it.

        Args:
            path: The Markdown file to update.
            name: The marker name (``<!-- gen:NAME start/end -->``).
            content: The generated content to place between the markers.
            seed: Full file text used when the file does not exist yet.
        """
        start = f"<!-- gen:{name} start -->"
        end = f"<!-- gen:{name} end -->"
        block = f"{start}\n{content}\n{end}"
        original = WikiDocs._read(path) if os.path.exists(path) else seed

        WikiDocs._write(path, WikiDocs._splice(original, start, end, block))

    @staticmethod
    def _splice(
        original: str,
        start: str,
        end: str,
        block: str,
    ) -> str:
        """Set the ``start``..``end`` span of ``original`` to ``block``.

        Args:
            original: The current file text.
            start: The opening marker.
            end: The closing marker.
            block: The full replacement (markers included).

        Returns:
            The updated text; the block is appended when no markers are present.
        """
        head = original.find(start)
        tail = original.find(end)

        if head == -1 or tail == -1:
            return original.rstrip() + "\n\n" + block + "\n"

        return original[:head] + block + original[tail + len(end):]

    # --- io ------------------------------------------------------------

    @staticmethod
    def _remove(paths: list[str]) -> None:
        """Delete each path in ``paths``.

        Args:
            paths: Absolute file paths to remove.
        """
        for path in paths:
            os.remove(path)

    @staticmethod
    def _read(path: str) -> str:
        """Read a file as UTF-8 text.

        Args:
            path: The file path to read.

        Returns:
            The file contents.
        """
        with open(path, encoding="utf-8") as handle:
            return handle.read()

    @staticmethod
    def _write(
        path: str,
        text: str,
    ) -> None:
        """Write ``text`` to ``path`` as UTF-8, creating parent folders.

        Args:
            path: The destination file path.
            text: The content to write.
        """
        os.makedirs(os.path.dirname(path), exist_ok=True)

        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)


# Seed used only when Home.md does not exist yet: prose above, generated ToC
# below. The prose is hand-editable; the ToC region is owned by make docs.
_HOME_SEED = (
    "# ryu_ldn_nx\n\n"
    "Atmosphere sysmodule bridging Nintendo LDN multiplayer to Ryujinx LDN "
    "servers. This wiki is the project's durable documentation; the sections "
    "below are the entry points.\n\n"
    "<!-- gen:toc start -->\n<!-- gen:toc end -->\n"
)

if __name__ == "__main__":
    if "--xml-only" in sys.argv:
        # Inside the compose service: only produce the XML, the host renders.
        shutil.rmtree(_XML_DIR, ignore_errors=True)
        os.makedirs(_XML_DIR)
        with tempfile.TemporaryFile("w+") as cfg:
            cfg.write(WikiDocs._config())
            cfg.seek(0)
            code = subprocess.run(["doxygen", "-"], stdin=cfg, cwd=_ROOT,
                                  check=False).returncode
        sys.exit(code)

    count = WikiDocs().run()
    sys.stdout.write(f"wiki: reference ({count} files) + navigation written\n")
