---
name: dev-flow
description: The dev flow of this repo from branch to pull request. Invoke before branching, committing or pushing, when a hook or guard blocks you, when CI is red, and BEFORE editing the Makefile, hooks/, .github/workflows/, scripts/ci/, docker-compose*.yml or .claude/hooks/ (guard-flow-docs.py and the pre-commit hook refuse the commit unless this skill or the wiki page Handoff-Dev-Flow moves with it). A short map to the authoritative wiki page.
---

# dev-flow — from branch to `develop`

The authoritative page is the wiki page **`Handoff-Dev-Flow`** (`docs/Handoff-Dev-Flow.md`).
This skill is the reflex and the map; fix BOTH when the flow changes. Never duplicate
the page here.

## Reflexes

- Branch `feature|bugfix|hotfix|chore|docs/<slug>`; never commit on `main`/`develop`.
- `git commit -s` (DCO). Identity comes from the repository's git config: never set
  or print it. No `--no-verify`, no force push, no history rewrite.
- `make hooks` once per clone (sets `core.hooksPath=hooks`); `make hooks-check`
  proves it.
- Builds and tests run in the devkitA64 container: `make build`, `make test`,
  `make lint`, `make dist`. Never invoke the cross toolchain on the host.
- Workflows: `make workflow-lint` (actionlint + shellcheck, containerised). Actions
  pinned by commit SHA, least-privilege `permissions`, untrusted input only via
  `env:`, no forced-success fallback after a command.
- The ticket CI (`tk.yml`) runs the tk CLI vendored in `scripts/tk` through `TK_BIN`, never a
  tk installed on the runner; re-sync that copy by hand when CI needs a newer tk. Its `gate` job checks out with
  `submodules: recursive` (the test image builds from `sysmodule/Atmosphere-libs`).

## Wiki before code

Edit the pages, `make docs` if docstrings moved, commit INSIDE `docs/` with explicit
pathspecs, then commit the code repository (`git commit -s`). The pre-commit hook
refuses a code commit unless `Handoff-Changes`, `Technique-Changelog`,
`User-Changelog` and `Handoff-Current-State` were committed in the wiki after the
previous code commit, and the wiki tree is clean. See `doc-sync`.

## Guards (`.claude/settings.json`)

- `block-hook-skip.sh`: `--no-verify`, `-n`, `HUSKY=0`, `SKIP_DOCS_GATE`, any
  unquoted `core.hooksPath` (false positive: that literal in an unquoted command —
  quote it or use `make hooks`).
- `guard-no-test-deletion.py`: `rm`/`git rm` near `tests/`, edits or writes that drop
  test cases or assertions from `tests/*.cpp`.
- `guard-flow-docs.py`: refuses a commit staging flow files unless this skill is
  staged or `Handoff-Dev-Flow.md` moved in the wiki.
- A denial by the auto-mode classifier is final for that outcome: take another
  legitimate route, do not reword and retry.
