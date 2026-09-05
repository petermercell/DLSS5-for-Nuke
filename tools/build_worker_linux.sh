#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
# Cross-compile the Windows worker (DLSS_Nuke_Worker.exe + the nvngx.dll caller
# shim) on Linux with mingw-w64. No Visual Studio, no Windows machine.
#
#   ./tools/build_worker_linux.sh [output_dir]      # default: runtime/
#
# Requires: mingw-w64  (dnf install mingw64-gcc-c++ / apt install mingw-w64)
# -----------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

OUT="${1:-$REPO/runtime}"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "x86_64-w64-mingw32-g++ not found." >&2
    echo "  Rocky/RHEL/Fedora: sudo dnf install mingw64-gcc-c++" >&2
    echo "  Debian/Ubuntu:     sudo apt install mingw-w64" >&2
    exit 1
fi

BUILD="build/worker-mingw"
cmake -S worker -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"

mkdir -p "$OUT"
cp -f "$BUILD/DLSS_Nuke_Worker.exe" "$OUT/"
cp -f "$BUILD/nvngx.dll"            "$OUT/"
install -m 0755 install/dlss5-worker.sh "$OUT/dlss5-worker.sh"

echo
echo "worker written to $OUT:"
ls -la "$OUT/DLSS_Nuke_Worker.exe" "$OUT/nvngx.dll" "$OUT/dlss5-worker.sh"

# The caller shim only works if the compiler left a real CALL/RET in it: the NR
# runtime validates that the return address belongs to nvngx.dll, so a
# tail-call-optimised JMP would trip 0xBAD00002.
#
# The linked DLL carries no symbol names, so inspect the object file, where each
# DLSSNR_Call* function is still labelled. Each must contain an indirect CALL
# and end in RET; an indirect JMP means the call was tail-call optimised away
# and the shim is silently useless.
echo
echo "exported entry points:"
x86_64-w64-mingw32-objdump -p "$OUT/nvngx.dll" | grep -E "DLSSNR_Call" | sed 's/^/  /'

SHIM_OBJ="$(find "$BUILD" -name 'caller_shim.cpp.obj' -o -name 'caller_shim.cpp.o' | head -1)"
echo
echo "verifying the shim kept a real CALL/RET in each thunk:"
if [ -z "$SHIM_OBJ" ]; then
    echo "  ?? caller_shim object not found under $BUILD -- cannot verify"
else
    DIS="$(x86_64-w64-mingw32-objdump -d "$SHIM_OBJ")"
    shim_bad=0
    for sym in DLSSNR_CallInit DLSSNR_CallCreate DLSSNR_CallEvaluate \
               DLSSNR_CallRelease DLSSNR_CallShutdown; do
        body="$(printf '%s\n' "$DIS" | awk -v s="<$sym>:" '
            index($0, s) {inside=1; next}
            inside && /^$/ {exit}
            inside {print}')"
        if [ -z "$body" ]; then
            echo "  ?? $sym: not found in the object"; shim_bad=1; continue
        fi
        has_call=$(printf '%s\n' "$body" | grep -cE '\bcall\s+\*' || true)
        has_ret=$(printf '%s\n'  "$body" | grep -cE '\bret\b'      || true)
        has_tail=$(printf '%s\n' "$body" | grep -cE '\bjmp\s+\*'   || true)
        if [ "$has_call" -ge 1 ] && [ "$has_ret" -ge 1 ] && [ "$has_tail" -eq 0 ]; then
            echo "  ok $sym (indirect call + ret, no tail-call)"
        else
            echo "  !! $sym: call=$has_call ret=$has_ret tailjmp=$has_tail"
            shim_bad=1
        fi
    done
    if [ "$shim_bad" -ne 0 ]; then
        echo
        echo "  WARNING: the shim was optimised in a way that breaks its purpose."
        echo "  Expect NGX to reject the caller with 0xBAD00002. Rebuild without LTO."
        exit 1
    fi
fi
