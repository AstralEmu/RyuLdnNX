#!/usr/bin/env bash
# Secret scanning gate: gitleaks over every commit of this repository, which
# is exactly the content a push would hand to GitHub. Same purpose as
# GitHub's secret scanning and push protection, measured here so a credential
# is caught before it leaves the machine instead of after.
#
# The history is scanned whole, never a narrowed path list: an exclusion would
# hide the finding rather than the secret. Vendored trees are outside the
# history (the Atmosphere libraries are a submodule, a gitlink in this
# repository), so a hit here is always a credential this project committed.
#
# Usage: secrets.sh [gitleaks args ...]
set -euo pipefail

cd /workspace
mkdir -p build-logs

# --redact keeps the terminal and the CI log free of the secret itself; the
# JSON report holds it so the owner can rotate it.
exec gitleaks git . \
    --no-banner \
    --redact \
    --exit-code 1 \
    --report-format json \
    --report-path build-logs/gitleaks-history.json \
    "$@"
