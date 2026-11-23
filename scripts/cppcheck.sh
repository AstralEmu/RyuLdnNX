#!/bin/bash
# cppcheck gate: warning, style, performance and portability checks over every
# project source. Any finding fails the run (--error-exitcode), nothing is
# suppressed: the code is fixed instead. Extra arguments reach cppcheck.
set -euo pipefail

cd /workspace

exec cppcheck \
    --enable=warning,style,performance,portability \
    --inconclusive \
    --std=c++20 \
    -DTEST_BUILD \
    -Isysmodule/source \
    -Itests/stubs \
    --suppress=missingIncludeSystem \
    --error-exitcode=1 \
    --quiet \
    -j"$(nproc)" \
    --template='{file}:{line}: {severity}: {message} [{id}]' \
    "$@" \
    sysmodule/source overlay/source
