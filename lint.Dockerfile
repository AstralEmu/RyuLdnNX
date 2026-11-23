# Lint image for ryu_ldn_nx.
#
# Extends the published dev environment image with clang-tidy-21 from
# apt.llvm.org. Debian bookworm's stock clang-tidy 14 cannot parse the
# GCC 15 libstdc++ headers shipped by current devkitA64 (C++23 constexpr
# math builtins, <format>, ranges) and segfaults on them — see the header
# comment in scripts/lint.sh. The binary is symlinked as `clang-tidy` so
# scripts can invoke it without a version suffix. clang-format-21 is shipped
# alongside so the formatting gate (scripts/style.py) is deterministic: the
# host's clang-format version never decides what is "correctly formatted".
#
# Usage:
#   docker compose build lint   (or: docker compose run --rm lint)
#
# Keep the base image reference in sync with docker-compose.yml.

ARG BASE_IMAGE=ghcr.io/ethiquema/ryu_ldn_nx:latest
FROM ${BASE_IMAGE}

RUN apt-get update && apt-get install -y --no-install-recommends gnupg && \
    rm -rf /var/lib/apt/lists/* && \
    curl -fsSL -o /tmp/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key && \
    gpg --yes --batch --dearmor -o /usr/share/keyrings/llvm.gpg /tmp/llvm-snapshot.gpg.key && \
    rm -f /tmp/llvm-snapshot.gpg.key && \
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-21 main" \
        > /etc/apt/sources.list.d/llvm21.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends clang-21 libclang-rt-21-dev clang-tidy-21 clang-format-21 && \
    ln -sf /usr/bin/clang++-21 /usr/local/bin/clang++ && \
    ln -sf /usr/bin/clang-tidy-21 /usr/local/bin/clang-tidy && \
    ln -sf /usr/bin/clang-format-21 /usr/local/bin/clang-format && \
    rm -rf /var/lib/apt/lists/* && \
    clang-tidy --version && \
    clang-format --version

# Security and workflow analysers (maintained, standard tools). They live in a
# dedicated virtualenv so pip never touches the system interpreter packages.
RUN apt-get update && \
    apt-get install -y --no-install-recommends python3-venv valgrind && \
    rm -rf /var/lib/apt/lists/* && \
    python3 -m venv /opt/analysers && \
    /opt/analysers/bin/pip install --no-cache-dir flawfinder semgrep zizmor && \
    for tool in flawfinder semgrep zizmor; do \
        ln -sf /opt/analysers/bin/$tool /usr/local/bin/$tool; \
    done && \
    flawfinder --version && semgrep --version && zizmor --version && valgrind --version
