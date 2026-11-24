# BLOCKED

State at 2026-11-24, PR #104 (head 3154c2a): all checks pass (lint, actionlint/shellcheck, CodeQL x2, Tests & Coverage >= 80%). Build & Package and Create Pre-Release are skipped on PRs by design.

## Resolved
- DCO gate removed; PR conflicts merged; clang-tidy via `.clang-tidy` AllowedTypes; CI pulls only `test`/`build`; integration Makefile gets the host stubs; integration readiness check requires a connection held open 1 s (Docker proxy accepts before the server listens).

## Open, awaiting the owner
- tk CI (run 45391644115, branch fix/tk-9e6033-thread-safety): rules, start, gate green (gate 2m24s); board and auto-merge fail with "tk not on PATH (set TK_BIN)": the runner has no tk CLI (it lives in ~/.claude/bin) and no GitHub App credentials (APP_SETUP.md). Proposals: (a) self-hosted runner with TK_BIN, (b) vendor tk into the repo plus the App key as a secret. Also: auto-merge targets the default branch (main) while this repo's flow merges into develop. Needs the owner's choice (secrets, merge target).
- Tickets L1-L12 of the old local store are not on the github board.

## Ticket table (tk list, 2026-11-24)
```
tk-9e6033	in-progress	Fix -Wthread-safety findings: annotate the host stub Mutex (stub done; GUARDED_BY needs an annotated RAII guard first)
tk-7113bf	todo	Step 2: port each remaining Python check to C++, down to zero alerts
tk-49853d	todo	Measure full clang-tidy and clang analyzer findings
tk-351f0e	todo	Review google-* tidy, agents' review, gates
tk-1fccc6	in-progress	Align the repository: RULES.md from code-law (already aligned, rules job green)
tk-fc4772	in-progress	NOLINT/cppcheck revalidation (done: no NOLINT in own code, cppcheck exit 0)
```
Column moves (review/done) are decided by the tk.yml CI on the ticket branch, never by the agent.
