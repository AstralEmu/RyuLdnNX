#!/usr/bin/env bash
################################################################################
# entrypoint-build.sh — Build sysmodule + overlay + dist ZIP
################################################################################
#
# PURPOSE:
#   Replaces the inline `command:` of the `build` service in docker-compose.yml.
#   Handles the libstratosphere.a resolution so the sysmodule Makefile always
#   finds a valid library to link against, regardless of whether the local
#   Atmosphere-libs override (docker-compose.override.yml) is active.
#
# RESOLUTION ORDER (libstratosphere.a):
#   1. LOCAL_A exists  → use as-is (locally patched build already present, or
#      a previously created symlink to the image's pre-built .a).
#   2. IMAGE_A exists  → the override is NOT active, so the image's pre-built
#      libstratosphere.a is available. Symlink it into the workspace checkout
#      path so the sysmodule Makefile (which resolves via
#      ATMOSPHERE_LIBRARIES_DIR = ./sysmodule/Atmosphere-libs) finds it.
#   3. Neither exists → the override IS active (IMAGE_A is shadowed by the
#      local mount) AND the local .a has been cleaned. Build libstratosphere
#      from source at the workspace path (always the host submodule checkout
#      with full source + Makefile, available via the .:/workspace mount).
#
# PATHS:
#   LOCAL_A  /workspace/sysmodule/Atmosphere-libs/libstratosphere/lib/
#            nintendo_nx_arm64_armv8a/release/libstratosphere.a
#   IMAGE_A  /opt/ryu_ldn_nx/libstratosphere/lib/
#            nintendo_nx_arm64_armv8a/release/libstratosphere.a
#   SRC_DIR  /workspace/sysmodule/Atmosphere-libs/libstratosphere
#            (with override active, /opt/ryu_ldn_nx/libstratosphere is the
#            same directory; without override, /opt/ryu_ldn_nx/libstratosphere
#            has NO Makefile, so we always build from the workspace path)
#
################################################################################

set -euo pipefail

# ATMOSPHERE_LIBS_PATH: when set by docker-compose.override.yml, points to the
# local Atmosphere-libs checkout to use instead of the image's pre-built libs.
# Defaults to the standard workspace checkout path so the script works
# identically with or without the override.
ATMOSPHERE_DIR="${ATMOSPHERE_LIBS_PATH:-/workspace/sysmodule/Atmosphere-libs}"

LOCAL_A="$ATMOSPHERE_DIR/libstratosphere/lib/nintendo_nx_arm64_armv8a/release/libstratosphere.a"
IMAGE_A="/opt/ryu_ldn_nx/libstratosphere/lib/nintendo_nx_arm64_armv8a/release/libstratosphere.a"
SRC_DIR="$ATMOSPHERE_DIR/libstratosphere"

# ── Step 0: apply Atmosphere PR #2821 patch if needed ─────────────────────────
PATCH_FILE="/workspace/patches/atmosphere-pr2821-forwardrequest.patch"
TARGET_CPP="$ATMOSPHERE_DIR/libstratosphere/source/sf/hipc/sf_hipc_server_session_manager.cpp"

if [ -f "$PATCH_FILE" ] && [ -f "$TARGET_CPP" ]; then
    # Detection: grep for the exact fix line. If upstream reformulates the fix
    # (different variable name, different formatting), this will produce a false
    # negative and git apply --check will fail gracefully with a WARNING.
    # This is an acceptable trade-off: the build continues with the pre-built
    # library, and the warning in logs signals that the patch needs updating.
    if grep -q "saved_request.meta.num_copy_handles" "$TARGET_CPP"; then
        echo "[build] ✅ Atmosphere PR #2821 fix already present — patch skipped."
    else
        echo "[build] 🔧 Atmosphere PR #2821 fix not detected — applying patch..."
        if git -C "$ATMOSPHERE_DIR" apply --check "$PATCH_FILE" 2>/dev/null; then
            git -C "$ATMOSPHERE_DIR" apply "$PATCH_FILE"
            echo "[build] ✅ Patch applied successfully."
            # Force rebuild: remove stale .a so Step 1 will rebuild from patched source.
            # Without this, a pre-existing libstratosphere.a (from a previous build
            # or symlinked from the Docker image) would be linked without the fix.
            if [ -f "$LOCAL_A" ]; then
                rm -f "$LOCAL_A"
                echo "[build] 🔧 Removed stale libstratosphere.a to force rebuild with patch"
            fi
        else
            echo "[build] ⚠️  WARNING: Patch could not be applied (upstream may have integrated the fix differently)."
            echo "[build] ⚠️  Build will continue — verify libstratosphere includes the fix manually."
        fi
    fi
fi

# NOTE: The patch above modifies the source tree but does NOT force a rebuild
# of libstratosphere.a. If a pre-existing .a is found (LOCAL_A or IMAGE_A),
# it will be used as-is — potentially without the PR #2821 fix. To guarantee
# a patched build, use `docker compose run --rm build-local` (which mounts
# the local submodule and rebuilds from source) or `clean-local && build-local`.

# ── Step 1: resolve libstratosphere.a ────────────────────────────────────────
if [ -f "$LOCAL_A" ]; then
    echo "[build] ✅ libstratosphere.a found at workspace path — using as-is"
elif [ -f "$IMAGE_A" ]; then
    echo "[build] 🔗 no local .a — symlinking pre-built libstratosphere.a from image"
    echo "[build]    (override not active; using image's pre-built library)"
    mkdir -p "$(dirname "$LOCAL_A")"
    ln -sf "$IMAGE_A" "$LOCAL_A"
else
    echo "[build] 🔧 no libstratosphere.a found — building from source..."
    echo "[build]    (override active and local .a cleaned: recompiling patched libs)"
    if [ ! -f "$SRC_DIR/Makefile" ]; then
        echo "[build] ❌ FATAL: $SRC_DIR/Makefile missing — cannot build libstratosphere" >&2
        exit 1
    fi
    make -C "$SRC_DIR" -j"$(nproc)" nx_release
    if [ ! -f "$LOCAL_A" ]; then
        echo "[build] ❌ FATAL: libstratosphere.a still missing after build" >&2
        exit 1
    fi
    echo "[build] ✅ libstratosphere.a built from source"
fi

# ── Step 2: build sysmodule ──────────────────────────────────────────────────
echo "[build] 🔨 Building sysmodule..."
cd /workspace/sysmodule && make -j4 all

# ── Step 3: build overlay ────────────────────────────────────────────────────
echo "[build] 🔨 Building overlay..."
cd /workspace/overlay && make -j4 all

# ── Step 4: package SD card structure + ZIP ───────────────────────────────────
echo "[build] 📦 Packaging SD card structure..."
cd /workspace/sysmodule && make dist

echo "[build] ✅ Done — output/ and ryu_ldn_nx-release.zip ready"