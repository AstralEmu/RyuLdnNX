#!/usr/bin/env bash
# Legacy entry point kept for old commands. The local CodeQL gate is
# scripts/codeql.sh: it traces the host test build and the cross-compiled
# sysmodule, applies the workflow's query set and paths, and fails on any
# result. This script only forwards to it, so it can never report a
# success the gate did not measure.
set -euo pipefail

exec "$(dirname "$0")/codeql.sh" "$@"
