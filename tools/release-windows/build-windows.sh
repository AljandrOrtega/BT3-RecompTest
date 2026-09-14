#!/usr/bin/env bash
# Host driver for the Windows cross-build container.
#
#   tools/release-windows/build-windows.sh [--iso PATH] [--reuse-deps] [--jobs N]
#
# Same contract as tools/release/build.sh: with --iso the container generates
# the recompiled sources natively (setup.py --gen-only) and then cross-compiles
# the runner + launcher for x86_64-pc-windows-msvc (clang-cl + xwin + lld-link).
#
# Produces build/release-windows/out/stage/  (runner, launcher, Qt6 + FFmpeg +
# VC++ runtime DLLs, plugins, wrapper) and runs the PE gate afterwards:
#   check_windows_deps.py                    (imports resolve, layout complete)
# The end-user zip is made separately with tools/release-windows/package.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RELEASE="$ROOT/build/release-windows"
OUT="$RELEASE/out"
RUNNER_BUILD="$RELEASE/runner"
LAUNCH_BUILD="$RELEASE/launcher"
IMG="bt3-release-win:jammy"
ISO_DEFAULT="/home/rexx/Descargas/Roms/PS2/DragonBall Z - Budokai Tenkaichi 3.iso"

# Git Bash/MSYS path bridge for the Docker Desktop CLI: host bind-mount paths
# must look like Windows paths (C:/...) or docker.exe cannot mount them, while
# the container-side targets (/src, /out) stay Linux-form. No-op on Linux/WSL.
winpath() { printf '%s' "$1"; }
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        if command -v cygpath >/dev/null 2>&1; then
            winpath() { cygpath -m "$1"; }
        fi
        ;;
esac

JOBS="${BT3_RELEASE_JOBS:-$(nproc 2>/dev/null || echo 8)}"
REUSE_DEPS=0
ISO=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --reuse-deps) REUSE_DEPS=1; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --jobs=*) JOBS="${1#--jobs=}"; shift ;;
        --iso) ISO="$2"; shift 2 ;;
        --iso=*) ISO="${1#--iso=}"; shift ;;
        --help|-h)
            echo "usage: $0 [--iso PATH|none] [--reuse-deps] [--jobs N]"; exit 0 ;;
        *) echo "ERROR: unknown argument: $1" >&2; echo "usage: $0 [--iso PATH|none] [--reuse-deps] [--jobs N]" >&2; exit 2 ;;
    esac
done

D=""
if ! docker info >/dev/null 2>&1; then
    if sudo -n docker info >/dev/null 2>&1; then D="sudo -n"
    else
        echo "ERROR: docker daemon not reachable. Start it with:" >&2
        echo "  sudo systemctl enable --now docker" >&2
        exit 2
    fi
fi

mkdir -p "$OUT" "$RUNNER_BUILD" "$LAUNCH_BUILD"

# ---- ISO selection (prompt before exec) ---------------------------------------
case "$ISO" in
    none|NONE|None) ISO="" ;;
esac
if [[ "$ISO" == "" ]]; then
    if [[ -t 0 ]]; then
        read -r -p "BT3 ISO path (Enter = ${ISO_DEFAULT}, 'none' = build from generated sources): " ISO
        case "$ISO" in
            "") ISO="$ISO_DEFAULT" ;;
            none|NONE|None) ISO="" ;;
        esac
    fi
fi
ISO_ARG=()
if [[ "$ISO" != "" ]]; then
    if [[ ! -f "$ISO" ]]; then
        echo "ERROR: ISO not found: $ISO" >&2; exit 2
    fi
    echo "== ISO: $ISO"
    ISO_ARG=(-e PS2X_ISO=/srv/bt3.iso -v "$(winpath "$ISO"):/srv/bt3.iso:ro")
else
    echo "== no ISO: building from already-generated sources"
fi

echo "== building image $IMG"
$D docker build -t "$IMG" "$(winpath "$ROOT/tools/release-windows")"

# Fresh _deps for a differently-built tree (cross vs host objects). --reuse-deps
# keeps whatever is there (a rerun after a failed half-build).
if [[ "$REUSE_DEPS" != "1" ]]; then
    rm -rf "$RUNNER_BUILD/_deps"
fi

echo "== building runner + launcher + stage (jobs=$JOBS)"
RUN_ARGS=(docker run --rm --user "$(id -u):$(id -g)" -e HOME=/w/runner -e BT3_RELEASE_JOBS="$JOBS")
[[ ${#ISO_ARG[@]} -gt 0 ]] && RUN_ARGS+=("${ISO_ARG[@]}")
RUN_ARGS+=(-v "$(winpath "$ROOT"):/src" \
    -v "$(winpath "$RUNNER_BUILD"):/w/runner" \
    -v "$(winpath "$LAUNCH_BUILD"):/w/launcher" \
    -v "$(winpath "$OUT"):/out" "$IMG" /src /w/runner /w/launcher /out)
$D "${RUN_ARGS[@]}"

echo "== PE gate"
# pefile lives inside the image (pip); override the entrypoint to run the gate.
$D docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    --entrypoint python3 \
    -v "$(winpath "$ROOT"):/src" -v "$(winpath "$OUT"):/out" \
    "$IMG" /src/tools/release-windows/check_windows_deps.py /out/stage

echo
echo "Stage: $OUT/stage"
echo "Package with: tools/release-windows/package.sh  (BT3-Recomp-x86_64.zip)"