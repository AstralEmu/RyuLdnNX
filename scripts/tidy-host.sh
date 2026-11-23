#!/bin/bash
# clang-tidy over every host-buildable translation unit: the sysmodule sources
# and the unit tests, headers included (HeaderFilterRegex in .clang-tidy).
# The checks come from the versioned .clang-tidy; any finding fails the run.
# Usage: tidy-host.sh [file.cpp ...]   (default: every source)
set -euo pipefail

cd /workspace
DB=$(mktemp -d)
trap 'rm -rf "$DB"' EXIT

FLAGS="-std=c++20 -DTEST_BUILD -I/workspace/sysmodule/source -I/workspace/tests/stubs -isystem /workspace/tests/stubs/system"
if [ "$#" -gt 0 ]; then
    FILES=("$@")
else
    mapfile -t FILES < <(find sysmodule/source tests -maxdepth 4 -name '*.cpp' -not -path 'tests/stubs/*' | sort)
fi

python3 - "$DB" "$FLAGS" "${FILES[@]}" <<'PY'
import json, os, sys
db, flags, *files = sys.argv[1:]
entries = [{"directory": "/workspace/tests",
            "command": "clang++ " + flags + " -c /workspace/" + os.path.relpath(f, "/workspace"),
            "file": "/workspace/" + os.path.relpath(f, "/workspace")} for f in files]
json.dump(entries, open(os.path.join(db, "compile_commands.json"), "w"))
PY

mkdir -p build-logs
LOG=build-logs/tidy-host.log
read -ra extra <<< "${TIDY_ARGS:-}"
run-clang-tidy "${extra[@]}" -clang-tidy-binary "$(command -v clang-tidy)" -p "$DB" -j"$(nproc)" -quiet "${FILES[@]/#/\/workspace\/}" > "$LOG" 2>&1 || status=$?
# run-clang-tidy colours its output; strip the escapes before counting.
sed -i 's/\x1b\[[0-9;]*m//g' "$LOG"
count=$(grep -c ': \(warning\|error\):' "$LOG" || test $? -eq 1)
echo "clang-tidy findings: $count (see $LOG)"
exit "${status:-0}"
