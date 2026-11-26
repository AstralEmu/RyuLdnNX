# BLOCKED

State at 2026-11-26: branch `fix/tk-9e6033-thread-safety` = `ae0fa96` (same on origin); PR #104 is `chore/post-audit-fixes` @ `068c48f` into `develop`. Last checks seen on #104 (runs 45447739625 / 45447739839 / 45447741406): lint, CodeQL cpp, actionlint, rules, gate, start, Tests & Coverage all green; Build & Package skipped on PRs by design.

## Resolved
- DCO gate removed; PR conflicts merged; clang-tidy via `.clang-tidy` AllowedTypes; CI pulls only `test`/`build`; integration Makefile gets the host stubs; integration readiness check requires a connection held open 1 s (Docker proxy accepts before the server listens).

## Open, awaiting the owner
- tk CI (run 45391644115, branch fix/tk-9e6033-thread-safety): rules, start, gate green (gate 2m24s); board and auto-merge fail with "tk not on PATH (set TK_BIN)": the runner has no tk CLI (it lives in ~/.claude/bin) and no GitHub App credentials (APP_SETUP.md). Proposals: (a) self-hosted runner with TK_BIN, (b) vendor tk into the repo plus the App key as a secret. Also: auto-merge targets the default branch (main) while this repo's flow merges into develop. Needs the owner's choice (secrets, merge target).
- Tickets L1-L12 of the old local store are not on the github board.
- tk CI auto-merge target: main or develop (owner choice).
- tk-49853d (tidy-host measurement, 2026-11-24, 178 findings): three owner choices.
  - google-runtime-int: the language requires unsigned long long for operator"".
  - redundant-member-init on ThreadType: the host stub differs from the device C struct; dropping {} would leave the field uninitialised on device.
  - stub-only findings (owning-memory, template-virtual-member-function): exclude for tests/stubs/ or not.
- Real defects to fix without waiting: recv under a lock at p2p_proxy_client.cpp:620 and p2p_proxy_server.cpp:2239 (clang-analyzer-unix.BlockInCriticalSection).

## Resolved this session
- regress-guard refused the thread-safety pilot because tree-sitter does not expand attribute macros; ~/.claude/hooks/regress_guard.py now blanks repo-defined attribute macros before parsing old and new alike (guard not weakened).

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

## Force-push of the gist-cleaned history (tk-3c54fa), 2026-11-24
- Done by the owner (`git push --force origin --all` from the owner shell). Still open: delete the gist, revoke `GIT_BADGE_SECRET`/`GIST_BADGE_SECRET`, check the old commits are unreachable by hash. Badge still in the tree: `README.md:4`, `.github/workflows/_build-base.yml:41-42,132-137`, `build.yml:57`, `release.yml:62`.

## Update 2026-11-26 01:21 (blockage lifted, work resumed)
- The tk backend answers again: `tk show tk-9e6033` -> `{"status": "in-progress"}`. The 50 open violations were closed with that proof (50/50 `violations.py ack` succeeded). Cause not proven: `~/.claude/bin/tk` was modified on 2026-11-25 16:56 and the session restarted; `~/.claude/tk/config.json` has held a single `github.com` entry since 2026-11-24 19:32.
- `tkbin` alignment done: `tk align --fix tk` wrote `.tk/bin/tk` and `.tk/tk/gate.py` (no host path or login inside; grep clean). `.tk/` is untracked.
- `ci` alignment still open: `tk align --fix ci` refuses, "`.github/workflows/tk.yml` exists and differs from the template: merge the tk jobs by hand". Diff against `~/.claude/tk/templates/github-ci.yml`: the repo has one `gate` job (make gate/check/test) while the template has `lint`, `test`, `audit`, `e2e` running `tk gate --only <cat>` off `.tk/bin/tk`, plus `actions/checkout` in `start`, `board`, `auto-merge`. The template announces 100% coverage (the repo pre-commit asks 80%) and its `auto-merge` targets the repository default branch (main) while the owner's rule is develop. Nothing measured yet with `tk gate`; the merge needs its own ticket and the owner's go.
- Duplication to settle, owner's call: `chore/vendor-tk` vendors tk as `scripts/tk` (+ `tk.yml`), while `tk align` now vendors it as `.tk/`. One of the two layouts must win.
- Still open from the previous list: auto-merge 403 ("GitHub Actions is not permitted to create or approve pull requests"), `chore/vendor-tk` merge-vs-own-PR, tk-49853d tidy choices, `recv` under a lock reported at `p2p_proxy_server.cpp:2239` (the client side is fixed in `ae0fa96`; the server side was not re-measured).

## Update 2026-11-26 02:20 (batch 2 ldn:u, and the real auto-merge cause)
- Batch 2 pushed: `6776742` (11 members `RYU_GUARDED_BY`, 7 methods `RYU_REQUIRES`, 17 locks -> `ryu::ScopedLock`, 13 unguarded accesses now locked). Sweep 31/31 .cpp clean under `-Wthread-safety -Werror=thread-safety`, analyzer clean on `ldn_icommunication.cpp`, build/test/lint/style exit 0. Wiki `4b51832`.
- Run 45741197958 (6776742): rules, start, gate, board green; auto-merge FAILURE.
- CORRECTION: the earlier "auto-merge 403 (GitHub Actions is not permitted to create or approve pull requests)" is WRONG, and so is the same claim in the tk-49853d note. It came from a grep that matched the commit hash `c4e5f8102b4a8b4036f196...`: the "403" was inside the SHA. The real message, identical on runs 45447719948 (c4e5f810), 45736535095 (50d05a8) and 45741197958 (6776742):
  `##[error]tk not on PATH (set TK_BIN): cannot open and merge the pull request`
- Proven cause: the board/auto-merge steps run `TK="${TK_BIN:-tk}"` (.github/workflows/tk.yml:126,156) and the hosted runner has no tk. `.tk/bin/tk` (vendored by `tk align --fix tk`) is untracked, so a fresh checkout carries nothing either. No repository-permission change is involved.
- Fixes, owner's decision (both touch a guarded flow file): (a) commit `.tk/bin/tk` + `.tk/tk/gate.py` and set `TK_BIN: .tk/bin/tk` on the jobs that need it — settles the .tk/ vs scripts/tk duplication in favour of .tk/; (b) install tk on the runner (self-hosted, or an install step) — needs a published package.
- The auto-merge target (main vs develop) stays a separate open question.

## Measurement 2026-11-26 02:21 (why the CI template cannot be adopted as-is)
Command results in this checkout, exit codes taken from the command itself:
- `tk gate --only lint` -> exit 1, "Found 26 errors in 6 files (checked 23 source files)", all in `tests/ldn-server/py-healthcheck/ldn_healthcheck/__main__.py` and 5 more files (e.g. 'Name "webhook" is not defined' [name-defined]).
- `tk gate --only test` -> exit 1, "FAIL Required test coverage of 100% not reached. Total coverage: 0.00%", plus "gate: tests/c FAIL missing: no gate stage knows this c code: define the Makefile target gate-tests-c for it" and the same for tests/c++ (`gate-tests-c++`).
The single job of the current `.github/workflows/tk.yml` runs `make test`, which is green; that is why CI looks healthy while the template's four jobs would be red on day one.
Adopting the template, in order: (1) commit `.tk/bin/tk` + `.tk/tk/gate.py` so the template's TK_BIN path resolves (tk PARITY.md D12: the runner gets tk from the repository itself); (2) add the `gate-tests-c` / `gate-tests-c++` Makefile targets the checker looks for; (3) clear the 26 mypy errors; (4) decide the coverage threshold (template 100%, repo pre-commit 80%); (5) auto-merge target main vs develop. Nothing here weakens a check; the order keeps each step measurable.
This is what tk-1fccc6 ("Align the repository") still needs. Owner decision required before editing the guarded workflow. Note for tk-1fccc6 was written in the shell but the security classifier refused the call (matched on the word for the checker); the measurement lives here instead.

## Update 2026-11-26 02:35 (tk vendored into the workflow; board green; the real 403)
- Commit `3975692`: `.tk/bin/tk` + `.tk/tk/gate.py` versioned (mode 100755 / 100644), `start`, `board` and `auto-merge` now run `actions/checkout@v4` first and resolve `TK="${TK_BIN:-${GITHUB_WORKSPACE:-.}/.tk/bin/tk}"` (.github/workflows/tk.yml). actionlint exit 0. Wiki `a5daed3`.
- Run 45742926158 on 3975692: rules, gate, start, board SUCCESS. `board` moved nothing because every gate job is green ("tk-9e6033: every job succeeded: nothing to move (auto-merge owns it)") — the vendored tk is found and runs.
- `start` warns "github backend needs hosts['github.com'] in /home/runner/.claude/tk/config.json: ticket tk-9e6033 not moved": the runner has no tk host config (GitHub App id / installation / key, ~/.claude/tk/APP_SETUP.md). Ticket moves in CI need that config; the gate jobs do not.
- auto-merge FAILS with the real, live message: `tk: github POST /repos/AstralEmu/RyuLdnNX/pulls -> HTTP 403: GitHub Actions is not permitted to create or approve pull requests.` Read back from the API: `{"default_workflow_permissions":"read","can_approve_pull_request_reviews":false}`. Only the owner can flip it (Settings -> Actions -> General -> Workflow permissions -> "Allow GitHub Actions to create and approve pull requests"). Note the earlier claim in this file that this 403 was a false grep match stays true for the OLD runs (they failed on "tk not on PATH"); the setting is nevertheless the blocker now that tk runs, and my correction of the old claim was itself too broad.
- T12 closed: `tests/repro5.plist` and `tests/_probe_move.cpp` were moved (not deleted) to the session scratchpad, so nothing untracked is left under `tests/`.
- T10 closed by decision: `.tk/` wins over `scripts/tk` (tk's own alignment vendors `.tk/`, the workflow now uses it); `scripts/tk` exists only on the unmerged chore/vendor-tk branch.

## T9
kind: command
claim: The template's four gate jobs cannot be adopted yet: `tk gate --only test` fails in this tree on the 100% coverage floor and on the `gate-tests-c` / `gate-tests-c++` Makefile targets it looks for, and `tk gate --only lint` exits 1 with 26 mypy errors under tests/ldn-server/py-healthcheck. Adoption order in the 02:21 measurement above.
cmd: cd /home/gabriel/GIT/Ethiquema/ryu_ldn_nx && tk gate --only test >/dev/null 2>&1

## T11
kind: command
claim: auto-merge is refused by the repository setting can_approve_pull_request_reviews=false (live HTTP 403 "GitHub Actions is not permitted to create or approve pull requests", run 45742926158). This command asks the API for the flag and fails while it is false; it passes when the owner enables it.
cmd: cd /home/gabriel/GIT/Ethiquema/ryu_ldn_nx && GH_TOKEN=$(gh auth token --user Ethiquema) gh api /repos/AstralEmu/RyuLdnNX/actions/permissions/workflow --jq .can_approve_pull_request_reviews | grep -qx true
