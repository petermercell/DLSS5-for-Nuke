#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
# Build DLSS5Live.so against one or more local Nuke installations.
#
#   ./tools/build_linux.sh /usr/local/Nuke17.0v6 /usr/local/Nuke17.1v2
#
# Output: bin/Nuke<major>.<minor>/DLSS5Live.so  (init.py prefers this over
#         bin/Nuke<major>, because Nuke's plug-in ABI moves between minors).
# -----------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

if [ $# -eq 0 ]; then
    echo "usage: $0 <NukeInstallDir> [NukeInstallDir ...]" >&2
    echo "example: $0 /usr/local/Nuke17.0v6 /usr/local/Nuke17.1v2" >&2
    exit 2
fi

# Nuke 13+ uses the C++11 std::string ABI; override with NUKE_CXX11_ABI=0 if a
# link produces a wall of undefined std::__cxx11 references.
CXX11_ABI="${NUKE_CXX11_ABI:-1}"

for NUKE_DIR in "$@"; do
    if [ ! -f "$NUKE_DIR/include/DDImage/Iop.h" ]; then
        echo "!! $NUKE_DIR does not look like a Nuke install (no include/DDImage/Iop.h)" >&2
        exit 1
    fi

    # /usr/local/Nuke17.1v2 -> 17.1
    BASE="$(basename "$NUKE_DIR")"
    VER="$(echo "$BASE" | sed -E 's/^Nuke([0-9]+\.[0-9]+).*/\1/')"
    if [ "$VER" = "$BASE" ]; then
        echo "!! could not parse a Nuke version out of '$BASE'" >&2
        exit 1
    fi

    BUILD="build/linux-Nuke$VER"
    OUT="bin/Nuke$VER"

    echo "==> Nuke $VER   ($NUKE_DIR)"
    cmake -S . -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNUKE_INSTALL_DIR="$NUKE_DIR" \
        -DNUKE_CXX11_ABI="$CXX11_ABI" \
        > "$BUILD.log" 2>&1 || { tail -30 "$BUILD.log"; exit 1; }

    cmake --build "$BUILD" -j"$(nproc)" >> "$BUILD.log" 2>&1 || { tail -40 "$BUILD.log"; exit 1; }

    mkdir -p "$OUT"
    cp -f "$BUILD/DLSS5Live.so" "$OUT/DLSS5Live.so"
    echo "    -> $OUT/DLSS5Live.so"

    # Undefined symbols other than Nuke's own are a load-time failure waiting to
    # happen, so surface them now rather than inside Nuke.
    #
    # Collect the output once instead of piping into head: under `set -o
    # pipefail`, head closing the pipe early makes grep die of SIGPIPE, the
    # pipeline reports failure, and `set -e` aborts the whole script after the
    # first Nuke version -- silently, because nothing printed an error.
    LDD_OUT="$(ldd -r "$OUT/DLSS5Live.so" 2>&1 || true)"
    if printf '%s\n' "$LDD_OUT" | grep -q "undefined symbol"; then
        echo "    note: unresolved symbols below are expected to come from Nuke itself:"
        printf '%s\n' "$LDD_OUT" | grep "undefined symbol" | head -5 || true
    fi
done

echo
echo "done."
