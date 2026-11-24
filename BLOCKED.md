# BLOCKED

State at 2026-11-24, PR #104 (head 3154c2a): all checks pass (lint, actionlint/shellcheck, CodeQL x2, Tests & Coverage >= 80%). Build & Package and Create Pre-Release are skipped on PRs by design.

## Resolved
- DCO gate removed; PR conflicts merged; clang-tidy via `.clang-tidy` AllowedTypes; CI pulls only `test`/`build`; integration Makefile gets the host stubs; integration readiness check requires a connection held open 1 s (Docker proxy accepts before the server listens).

## Open, awaiting the owner
- tk ticket flow: `tk detect` returns local (remote host is the SSH alias github-ethiquema); owner AstralEmu absent from ~/.claude/tk/config.json; no .github/workflows/tk.yml; branch name carries no ticket id. Fix not applied without the owner's go.
- L11 (-Wthread-safety): owner decision pending.

## Ticket table (tk list, 2026-11-24)
```
L1	review	ÉTAPE 2 — Transposer chaque check en C++, un par un, jusqu'à zéro alerte. Pour chaque catégorie, mesure d'abord le nombre d'alertes, corrige la cause dans le code, branche ensuite la gate.
L3	review	Autonomie: étape 0 (gates + commits), étapes 1-3 (checks Python -> C++), CodeQL 4 alertes
L4	review	Autonomie: mesures tidy complet + clang analyzer, suite étape 2
L5	review	Autonomie: google-* tidy, relecture agents, gates, commits
L6	review	Corriger régression assert_ll int64_t, gates, commits
L7	review	Aligner le dépôt: RULES.md depuis code-law (ordre tk-align)
L8	review	Autonomie: revalider tests+build après dernier commit, puis suite étape 2 (13 NOLINT, -Wthread-safety, cppcheck)
L9	review	Décisions owner: en-tête test-seam (unity), stub CopyHandle non copiable, fix 3 alertes CodeQL sans casser ryuldn, push puis suivre gates GH
L10	review	Corriger les erreurs avalées des hooks (regress-guard) puis pousser la PR #104
L11	review	Fix -Wthread-safety findings (stub Mutex annotations), see BLOCKED.md
L12	review	Resolve PR #104 conflicts with develop via CLI merge (owner request)
```
Every ticket is in column `review`; moving it to review/done is the owner's slot (and CI's), not the agent's. No ticket is in todo/doing, so no work is left to take.
