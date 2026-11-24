---
name: container-dev
description: How to build, test, lint, debug and generate docs in this repo. Everything toolchain-related runs inside the devkitA64 container defined by the Dockerfile and docker-compose.yml; the host only runs Makefile wrappers, git hooks and python scripts. Invoke before running any compile, test, lint or docs command.
---

# container-dev — the toolchain lives in the container

The devkitPro / devkitA64 toolchain exists only in the build image. Never compile
on the host and never try to make host IDE diagnostics (`<stratosphere.hpp>` not
found) go away: they are expected.

| Goal | Command | Compose service |
|------|---------|-----------------|
| Sysmodule + overlay + dist ZIP | `make build` | `build` |
| Host unit tests (g++, `-DTEST_BUILD`) | `make test` | `test` |
| Coverage (per-file gate, `MIN_COVERAGE`, default 80) | `COVERAGE=1 docker compose run --rm test`, then `check-coverage.py` | `test` |
| clang-tidy | `make lint` | `lint` |
| Package SD layout + release zip | `make dist` | `build` |
| Wiki reference + navigation | `make docs` (host Doxygen if present, else container) | `docs` |
| Workflow lint | `make workflow-lint` | actionlint image |
| GDB against a console | `make debug IP=<switch-ip> [PID=<pid>]` | `debugger` |
| Shell in the image | `make shell` | `shell` |

Rules:

- Use compose services, never `make` of the sysmodule directly on the host.
- A single suite: `cd tests && make test-<suite>` (host g++, no container needed).
- The `docs` service runs as `DOCS_UID:DOCS_GID` so generated files are not root-owned.
- Memory budget: the sysmodule shares a 384 KB expanded heap; do not grow buffers
  or stacks without a measured need (see the wiki `Design-Vision-And-Invariants`).
- Logs of an on-console run land in `tmp/logs/ryu_ldn_nx.log`; read the delta per
  run, not the whole file.
