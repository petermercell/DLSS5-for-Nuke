#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
# Linux installer for DLSS5Live. Mirrors what install.bat does on Windows:
# creates ~/.nuke/DLSS5Live/, copies the plug-in files, and registers the
# plug-in path from ~/.nuke/init.py.
#
#   ./install/install.sh
# -----------------------------------------------------------------------------
set -euo pipefail

SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$HOME/.nuke/DLSS5Live"
NUKE_INIT="$HOME/.nuke/init.py"
MARKER="# --- DLSS5Live plug-in path (added by install.sh) ---"

echo "installing to $DEST"
mkdir -p "$DEST" "$DEST/runtime"

cp -f "$SRC/install/init.py" "$DEST/init.py"
cp -f "$SRC/install/menu.py" "$DEST/menu.py"
[ -f "$SRC/install/DLSS5.png" ] && cp -f "$SRC/install/DLSS5.png" "$DEST/DLSS5.png"
install -m 0755 "$SRC/install/dlss5-worker.sh" "$DEST/runtime/dlss5-worker.sh"

# Built plug-ins, if any.
if [ -d "$SRC/bin" ]; then
    for d in "$SRC"/bin/Nuke*; do
        [ -d "$d" ] || continue
        mkdir -p "$DEST/bin/$(basename "$d")"
        cp -f "$d"/*.so "$DEST/bin/$(basename "$d")/" 2>/dev/null || true
        echo "  bin/$(basename "$d")"
    done
else
    echo "  (no bin/ yet -- run tools/build_linux.sh first)"
fi

# Worker runtime, if already built.
for f in DLSS_Nuke_Worker.exe nvngx.dll _nvngx.dll nvngx_dlssnr.dll nvngx_dlss.dll; do
    [ -f "$SRC/runtime/$f" ] && cp -f "$SRC/runtime/$f" "$DEST/runtime/$f" && echo "  runtime/$f"
done

mkdir -p "$HOME/.nuke"
touch "$NUKE_INIT"
if ! grep -qF "$MARKER" "$NUKE_INIT"; then
    {
        echo ""
        echo "$MARKER"
        echo "import nuke, os"
        echo "_dlss5 = os.path.expanduser('~/.nuke/DLSS5Live')"
        echo "if os.path.isdir(_dlss5) and _dlss5 not in nuke.pluginPath():"
        echo "    nuke.pluginAddPath(_dlss5)"
    } >> "$NUKE_INIT"
    echo "registered plug-in path in $NUKE_INIT"
else
    echo "plug-in path already registered in $NUKE_INIT"
fi

echo
echo "done. Restart Nuke, press Tab and create DLSS5Live."
echo "Then point the node's worker path at:"
echo "  $DEST/runtime/dlss5-worker.sh"
echo "or export NUKE_DLSS5_WORKER_PATH=$DEST/runtime/dlss5-worker.sh"
