# Root Makefile for ryu_ldn_nx
#
# Thin wrapper around the Docker Compose services defined in
# docker-compose.yml. All real work happens inside the dev container
# (ghcr.io/ethiquema/ryu_ldn_nx:monthly-20260701) so the host only needs
# Docker + docker compose — no devkitPro install required.
#
# Usage:
#   make build       # build sysmodule + overlay + dist ZIP
#   make test        # run host unit tests
#   make clean       # wipe build artifacts and output/
#   make dist        # package SD card layout + release ZIP
#   make lint        # run clang-tidy on project sources
#   make style       # formatting + Doxygen-completeness gate (scripts/style.py)
#   make style-fix   # clang-format every source in place
#   make shell       # interactive shell inside the build container
#   make debug IP=... [PID=...]  # interactive GDB session
#   make hooks       # wire tracked git hooks (hooks/) and the wiki hook
#   make hooks-check # fail when git hooks are not wired to this repo
#   make workflow-lint # actionlint (+ shellcheck) on .github/workflows
#
# See docker-compose.yml for the authoritative service definitions.
#
# DEBUG_HEX_DUMP is intentionally NOT defined by default — guards remove
# hex dumps from production builds.

SHELL := /bin/sh

# Containers write into the bind-mounted checkout: run them as the caller so
# no artifact ends up owned by root (consumed by docker-compose.yml).
export DOCS_UID := $(shell id -u)
export DOCS_GID := $(shell id -g)

# docker compose binary (v2). Override if your distro ships a different name.
DOCKER_COMPOSE ?= docker compose

# Optional args passed through to the debugger service.
IP  ?=
PID ?=

# actionlint image used by `make workflow-lint` (bundles shellcheck). Override
# to pin a digest: ACTIONLINT_IMAGE=rhysd/actionlint@sha256:...
ACTIONLINT_IMAGE ?= rhysd/actionlint:1.7.7

.PHONY: build test clean dist lint cppcheck style style-fix shell debug help hooks hooks-check workflow-lint docs

help: ## Show this help
	@awk 'BEGIN {FS = ":.*##"} \
	     /^[a-zA-Z_-]+:.*##/ {printf "  %-12s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

build: ## Build sysmodule + overlay + dist ZIP (docker compose run --rm build)
	$(DOCKER_COMPOSE) run --rm build

test: hooks-check ## Run host unit tests (docker compose run --rm test)
	$(DOCKER_COMPOSE) run --rm test

clean: ## Clean all build artifacts and output/ (docker compose run --rm clean)
	$(DOCKER_COMPOSE) run --rm clean

dist: ## Package SD card layout + release ZIP (runs the dist target in sysmodule)
	$(DOCKER_COMPOSE) run --rm build sh -c 'cd /workspace/sysmodule && make dist'

lint: hooks-check ## Run clang-tidy on project sources (docker compose run --rm lint)
	$(DOCKER_COMPOSE) run --rm lint

cppcheck: hooks-check ## cppcheck gate, any finding fails (docker compose run --rm cppcheck)
	$(DOCKER_COMPOSE) run --rm cppcheck

style: hooks-check ## Formatting + Doxygen-completeness gate (docker compose run --rm style)
	$(DOCKER_COMPOSE) run --rm style

style-fix: hooks-check ## clang-format every C/C++ source in place (docker compose run --rm style format)
	$(DOCKER_COMPOSE) run --rm style format

shell: ## Interactive shell inside the build container (docker compose run --rm shell)
	$(DOCKER_COMPOSE) run --rm shell

debug: ## Interactive GDB session: make debug IP=<SWITCH_IP> [PID=<pid>]
	@if [ -z "$(IP)" ]; then \
		echo "Usage: make debug IP=<SWITCH_IP> [PID=<pid>]" >&2; \
		exit 2; \
	fi
	$(DOCKER_COMPOSE) run --rm debugger $(IP) $(PID)

hooks: ## Wire the tracked git hooks (hooks/) and the wiki's own pre-commit
	git config core.hooksPath hooks
	@# Git hooks call `docker compose` without going through make: compose
	@# reads the untracked .env, so the containers run as the caller there too.
	@printf 'DOCS_UID=%s\nDOCS_GID=%s\n' "$(DOCS_UID)" "$(DOCS_GID)" > .env
	@if [ -e docs/.git ]; then \
		gd=$$(git -C docs rev-parse --absolute-git-dir) && \
		install -m 755 hooks/docs-pre-commit "$$gd/hooks/pre-commit" && \
		echo "wiki pre-commit installed"; \
	else \
		echo "no ./docs wiki symlink: wiki hook not installed"; \
	fi

hooks-check: ## Fail when git hooks are not wired to this repo's hooks/
	@python3 scripts/hooks_path.py

docs: ## Regenerate the wiki code reference + navigation (Doxygen XML -> docs/)
	python3 scripts/gen_docs.py

workflow-lint: ## actionlint + shellcheck on .github/workflows (container)
	docker run --rm -v "$(CURDIR):/repo:ro" -w /repo $(ACTIONLINT_IMAGE) -color
