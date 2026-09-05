#!/usr/bin/env bash
# Build and run the upstream reproduction.
#
#   ./build_repro.sh [runtime_dir]
#
# runtime_dir defaults to ../runtime and must contain:
#   _nvngx.dll      from the NVIDIA driver (/usr/lib64/nvidia/wine/)
#   nvngx_dlss.dll  from the public NVIDIA DLSS SDK
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME="${1:-$HERE/../runtime}"
RUNTIME="$(cd "$RUNTIME" && pwd)"

x86_64-w64-mingw32-g++ -std=c++17 -O2 -o "$RUNTIME/ngx_repro.exe" "$HERE/ngx_repro.cpp" \
    -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++
echo "built $RUNTIME/ngx_repro.exe"

for f in _nvngx.dll nvngx_dlss.dll; do
    [ -f "$RUNTIME/$f" ] || echo "WARNING: $RUNTIME/$f missing"
done

: "${WINE:=${NUKE_DLSS5_WINE:-wine}}"
if [ -z "${WINEPREFIX:-}" ]; then
    # Outside ~/.nuke on purpose: Nuke scans its plug-in directories at startup
    # and a Wine prefix parked in one breaks that. An existing prefix at the old
    # location is still honoured.
    if [ -f "$HOME/.nuke/DLSS5Live/wineprefix/system.reg" ]; then
        WINEPREFIX="$HOME/.nuke/DLSS5Live/wineprefix"
    else
        WINEPREFIX="$HOME/.local/share/dlss5-wine/prefix"
    fi
fi

# A missing wine sends "command not found" to the redirected stderr, which makes
# the run look like a binary that died silently at load. Check it up front.
if ! command -v "$WINE" >/dev/null 2>&1 && [ ! -x "$WINE" ]; then
    echo "ERROR: wine not found: '$WINE'" >&2
    echo >&2
    echo "Set NUKE_DLSS5_WINE to your portable Wine, e.g.:" >&2
    for c in "$HOME"/.local/share/dlss5-wine/wine-*/bin/wine; do
        [ -x "$c" ] && echo "  export NUKE_DLSS5_WINE=$c" >&2
    done
    exit 1
fi
echo "wine: $("$WINE" --version 2>&1 | head -1)  ($WINE)"
export WINEPREFIX
export WINEDEBUG="${WINEDEBUG:--all}"
export DXVK_NVAPI_LOG_LEVEL="${DXVK_NVAPI_LOG_LEVEL:-trace}"

# Mandatory. Without it DXVK hides the NVIDIA GPU and reports it with AMD's
# vendor id (0x1002), so any NVIDIA-vendor check rejects the real adapter and
# D3D12CreateDevice is never even reached.
export DXVK_ENABLE_NVAPI=1

# "n,b" silently falls back to Wine's builtin d3d12 when vkd3d-proton fails to
# load, which looks identical to "no GPU". Native-only makes that failure loud.
# Set DLSS5_ALLOW_BUILTIN_D3D12=1 to go back to the permissive form.
if [ "${DLSS5_ALLOW_BUILTIN_D3D12:-0}" = "1" ]; then
    export WINEDLLOVERRIDES='nvapi,nvapi64=n,b;d3d12,d3d12core=n,b;dxgi,d3d11=n,b'
else
    export WINEDLLOVERRIDES='nvapi,nvapi64=n,b;d3d12,d3d12core=n;dxgi,d3d11=n,b'
fi
echo "WINEDLLOVERRIDES=$WINEDLLOVERRIDES"

SYS32="$WINEPREFIX/drive_c/windows/system32"
for f in d3d12.dll d3d12core.dll dxgi.dll nvapi64.dll; do
    if [ -f "$SYS32/$f" ]; then
        printf '  prefix has %-14s %s bytes\n' "$f" "$(stat -c%s "$SYS32/$f")"
    else
        printf '  prefix MISSING %s\n' "$f"
    fi
done

echo
echo "=== running (stderr -> ngx_repro.log, full stdout -> ngx_repro.out) ==="
cd "$RUNTIME"
# NGX writes its own timestamped log lines to stdout, hundreds of them. Keep the
# whole thing in ngx_repro.out and show only the reproducer's own output.
"$WINE" ./ngx_repro.exe 2>"$RUNTIME/ngx_repro.log" \
    | tee "$RUNTIME/ngx_repro.out" \
    | grep -v '^\[[0-9-]\{10\} ' || true

echo
if grep -q "vkd3d-proton" "$RUNTIME/ngx_repro.log"; then
    echo "vkd3d-proton: loaded"
else
    echo "vkd3d-proton: NOT loaded -- d3d12.dll was Wine's builtin, or failed to load."
    echo "              This alone explains a missing D3D12 device."
fi

echo
echo "=== what NGX itself said about the parameters ==="
grep -iE "could not find|EvaluateFeature|ProcessParameters|missing|invalid" \
    "$RUNTIME/ngx_repro.log" | grep -viE "DestroyCubin" | head -25 \
    || echo "  (nothing - set DLSS5_NGX_VERBOSE=1 / check the NGX log path)"

echo
echo "=== NVAPI descriptor-object calls ==="
# Shutdown produces dozens of DestroyCubinComputeShader lines that drown out the
# interesting ones, so exclude them and show the descriptor-object calls only.
grep -iE "GetCudaIndependentDescriptorObject|GetCudaMergedTextureSamplerObject" \
    "$RUNTIME/ngx_repro.log" | grep -v nullptr | head -20 \
    || echo "  (none found - was DXVK_NVAPI_LOG_LEVEL=trace set?)"

echo
echo "=== environment confirmation ==="
grep -iE "NvAPI_Initialize|GetGraphicsCapabilities|CaptureUAVInfo|vkd3d_instance_apply" \
    "$RUNTIME/ngx_repro.log" | head -6 || true
echo "  cubin shaders created: $(grep -c 'CreateCubinComputeShaderExV2 ({' "$RUNTIME/ngx_repro.log" || echo 0)"

echo
echo "full log: $RUNTIME/ngx_repro.log"
