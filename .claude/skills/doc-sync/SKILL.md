---
name: doc-sync
description: MANDATORY documentation workflow. Invoke before committing ANY change under sysmodule/source, overlay/source or scripts, and whenever editing the wiki (docs/). Encodes the "docs move with code" reflex the pre-commit hook enforces — handoff state + changelogs, regenerated reference, wiki committed first, then the code.
---

# doc-sync — keep the docs in lockstep with the code

## Why

`docs/` (a symlink to the separate wiki clone) is the durable memory. No change to
`sysmodule/source/`, `overlay/source/` or `scripts/` is complete until the docs move
with it. Editing the docs at the same time forces a reread against the live code,
which is how drift is caught the moment it appears. The changelogs are a checklist
of what changed, NOT the documentation: the documentation is the synthesis pages
that explain what the project is and how it works.

## The five axes and the frontier rule

One source per fact: LINK across axes, never repeat.

| Axis | Question | Pages | Driven by |
|------|----------|-------|-----------|
| Reference | what (mechanical) | `Reference-*` (generated) | `make docs`, never by hand |
| Technique | how it is built, how to contribute | `Technique-*` | the code on disk |
| Direction | why, invariants, where we go | `Design-*`, `Roadmap-*` | a dated decision |
| User | how to use the tool | `User-*` | the overlay / config surface a player sees |
| Handoff | state, resume, impact map | `Handoff-*` | every code change |

Do not pad the User axis with internals: a protocol or proxy change is Technique +
Handoff, a User page moves only when what a player sees or configures moves.

## What the pre-commit hook blocks

A commit touching `sysmodule/source/`, `overlay/source/` or `scripts/` is refused
unless the wiki is clean (the hook regenerates the reference first, so generator
drift shows as a dirty wiki) and `Handoff-Changes`, `Technique-Changelog`,
`User-Changelog` and `Handoff-Current-State` were each committed in the wiki after
the previous code commit. For a genuine no-op write "no user-visible change"
explicitly. When a capability is added or removed also move a Technique page; when
a decision moved update `Design-Decisions` (not diff-triggered: a discipline).

## Layout

The wiki is FLAT (GitHub wikis have no subfolders): `Handoff-*`, `Technique-*`,
`User-*`, `Design-*`, `Roadmap-*`, `Reference-*`, `Home`, `_Sidebar`, `_Footer`.
Handoff pages are one terse dsl block (Dev-Flow, Changes, Decisions are prose).
Generated regions sit between `<!-- gen:NAME start -->` / `<!-- gen:NAME end -->`:
prose outside is preserved, content inside is overwritten. To change a generated
block edit `scripts/gen_docs.py`, not the wiki.

`Handoff-Current-State` is the resume anchor: state, plan with status, open TODO,
invariants, impact map. Update it in the same commit as any change that moves them.

## Workflow

1. Make the code change with its tests.
2. `make docs` when docstrings moved.
3. Edit the pages (at least the four above) and any page the reread shows wrong.
4. Commit the wiki with explicit pathspecs: `git -C docs add <pages>` then
   `git -C docs commit -m "docs: <what>"` (the wiki's own hook refuses a commit off
   its default branch).
5. Commit the code: `git commit -s` (the repository's configured identity).
6. Push only with the owner's go-ahead; `pre-push` publishes the wiki first.

## Do not

- Hand-edit `Reference-*.md`, `Reference.md`, `Design.md`, `_Sidebar.md`, `_Footer.md`
  or a generated region.
- Create wiki subdirectories.
- Commit with `--no-verify` to dodge the gate.
- Put personal hostnames, IPs or owner names in the public wiki.
