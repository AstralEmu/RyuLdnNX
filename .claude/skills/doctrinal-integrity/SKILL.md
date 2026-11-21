---
name: doctrinal-integrity
description: Absolute integrity rules. NEVER fake a green check by disabling, excluding or weakening it (lint suppressions, lowered coverage, skipped or deleted tests, `|| true`) — that is a failure disguised as a success. NEVER invent directories outside the project layout. NEVER force-push, rewrite published history or skip a hook. Read and apply on EVERY task; these override convenience and speed.
---

# doctrinal-integrity — no faked victories, no violations, no history rewrite

## 1. Never fake a passing check

A green gate must be earned, never manufactured. It is FORBIDDEN to make a check
pass by weakening or dodging it:

- No path added to a clang-tidy / cppcheck / actionlint exclusion to silence
  findings in code that should pass.
- No `// NOLINT`, `#pragma` warning suppressions, `-Wno-*` flags, lowered coverage
  threshold (`MIN_COVERAGE`), skipped or commented-out test, `|| true`,
  `continue-on-error`, or swallowed error to turn a step green.
- No throwaway code parked where the gate cannot see it.
- No test weakened or special-cased for the test (`#ifdef TEST_BUILD` branches that
  change behaviour only under test).

If the code cannot pass the real gate, the CODE is wrong: fix it, or do not write
it. A check disabled to go green is a failure signal, reverted on sight.

## 2. Never violate the project layout

- Code lives in `sysmodule/source/<area>/`, `overlay/source/`, `tests/`, `scripts/`.
- Documentation and project state live in the wiki (`docs` symlink to the wiki
  clone), flat pages prefixed `Handoff-`, `Technique-`, `User-`, `Design-`,
  `Roadmap-`, `Reference-` — never a new folder of notes in the code repository.

When you need a place for something, use the right existing slot. If none fits,
record the question in the wiki; do not fabricate a directory.

## 3. Never rewrite published history, never skip a hook

Forbidden, in `.claude/settings.json` and `.claude/hooks/block-hook-skip.sh`:

- any force push and any rewrite of a published branch;
- skipping a hook (`--no-verify`, `-n`, `HUSKY=0`, `SKIP_DOCS_GATE`, a
  `core.hooksPath` override): there is no escape hatch;
- committing without `-s` (DCO), or hardcoding an identity: the sign-off comes from
  the repository's git configuration.

Wire the hooks with `make hooks`. See `dev-flow` and the wiki page
`Handoff-Dev-Flow`.

## Mindset

A slower, honest, reviewable result always beats a fast rigged one. When tempted to
exclude, silence or force: stop and do it the honest way.
