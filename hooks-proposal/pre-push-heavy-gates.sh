#!/bin/sh
# PROPOSAL, not active: block to insert in hooks/pre-push after the branch
# checks and before the wiki publication. The owner applies it.
#
# Heavy gates: anything GitHub (CodeQL, Dependabot, secret scanning) or the CI
# would raise must fail here, before the push. Everything runs through
# `docker compose` (see the wiki page Handoff-Dev-Flow). No override, no skip.
#
# Gates that are not wired yet (CodeQL without a local bundle, -Wthread-safety)
# are listed in BLOCKED.md and in the wiki page Handoff-Gate-Placement; add
# each line below when its gate exists.

run_gate() {
	label=$1
	shift
	echo "pre-push: $label"
	if ! "$@"; then
		echo "✗ pre-push gate failed: $label" >&2
		exit 1
	fi
}

run_gate "style (whole tree)" docker compose run -T --rm style check
run_gate "cppcheck" docker compose run -T --rm cppcheck
run_gate "clang-tidy (whole tree)" docker compose run -T --rm tidy-host
run_gate "host suites with coverage" docker compose run -T --rm -e COVERAGE=1 test
run_gate "AddressSanitizer + UBSan" docker compose run -T --rm -e SANITIZE=address,undefined test
run_gate "ThreadSanitizer" docker compose run -T --rm -e SANITIZE=thread test
run_gate "devkitA64 build" docker compose run -T --rm build
run_gate "libFuzzer (protocol, INI)" docker compose run -T --rm fuzz
run_gate "secrets (gitleaks, whole history)" docker compose run -T --rm secrets
