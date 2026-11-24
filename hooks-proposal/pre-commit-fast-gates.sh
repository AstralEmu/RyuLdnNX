#!/bin/sh
# PROPOSAL, not active: blocks to insert in hooks/pre-commit after the style
# gate (step 2) and before the workflow lint. The owner applies it.
#
# Fast gates (staged files only, well under a minute): static analysis of what
# this commit changes, and a secret scan of the exact content being committed.
# The whole-tree versions run in pre-push (hooks-proposal/pre-push-heavy-gates.sh).
# No override, no skip: a failing line means the code or the commit is fixed.

# 2b. clang-tidy on the staged C/C++ files --------------------------------
# The check set comes from .clang-tidy (any finding is an error). Only the
# files this commit touches are analysed: the whole tree is pre-push's job.
tidy_staged=$(git diff --cached --name-only --diff-filter=ACM -- '*.c' '*.cpp' '*.h' '*.hpp')
if [ -n "$tidy_staged" ]; then
	echo "pre-commit: clang-tidy on staged files"
	# shellcheck disable=SC2086
	docker compose run --rm tidy-host $tidy_staged
fi

# 2c. Secrets in the content being committed ------------------------------
# gitleaks reads the staged diff exactly as GitHub's push protection would,
# before the commit exists. The full history is scanned in pre-push.
if [ -n "$(staged)" ]; then
	echo "pre-commit: secret scan on staged changes"
	docker compose run --rm --entrypoint gitleaks secrets git . --pre-commit --staged \
		--no-banner --redact --exit-code 1
fi
