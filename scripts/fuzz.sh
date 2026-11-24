#!/bin/bash
# libFuzzer targets (tests/fuzz): the wire-protocol stream reassembly and the
# configuration INI parser. Runs inside the lint image, which ships clang 21
# and its libFuzzer runtime (lint.Dockerfile).
#
#
#   docker compose run --rm fuzz              (60 s per target)
#   docker compose run --rm -e FUZZ_SECONDS=600 fuzz
#
# A crash, a sanitizer report or a timeout exits non-zero.

set -euo pipefail

cd "$(dirname "$0")/.."

FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
FUZZ_DIR="build-logs/fuzz"
CXXFLAGS=(
	-std=c++20 -DTEST_BUILD -g -O1
	-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
	-Isysmodule/source -Itests/stubs -isystem tests/stubs/system
)

mkdir -p "$FUZZ_DIR"

# target: extra translation units the target links against
targets=(
	"fuzz_packet_buffer:"
	"fuzz_config_ini:sysmodule/source/config/config.cpp"
)

for entry in "${targets[@]}"; do
	name="${entry%%:*}"
	extra="${entry#*:}"
	clang++ "${CXXFLAGS[@]}" "tests/fuzz/${name}.cpp" ${extra} -o "$FUZZ_DIR/$name"
	echo "fuzz: $name ${FUZZ_SECONDS}s"
	mkdir -p "$FUZZ_DIR/corpus/$name"
	# shellcheck disable=SC2086
	"$FUZZ_DIR/$name" -max_total_time="$FUZZ_SECONDS" -max_len=70000 \
		-timeout=10 -rss_limit_mb=2048 "$FUZZ_DIR/corpus/$name"
done

echo "fuzz: all targets clean"
