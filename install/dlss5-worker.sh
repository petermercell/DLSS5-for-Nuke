#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
# Wine launcher for DLSS_Nuke_Worker.exe on Linux.
#
# The DLSS-NR runtime (nvngx_dlssnr.dll, NGX feature 18) is a Windows PE with no
# Linux build, so the worker runs under Wine with vkd3d-proton translating its
# D3D12 calls to Vulkan. The Nuke node talks to it over stdin/stdout exactly as
# it does on Windows.
#
# This script is exec'd by the plug-in with:  dlss5-worker.sh <worker.exe> --video
# stdin and stdout MUST stay untouched: every byte on them is protocol payload.
# All setup chatter therefore goes to $DLSS5_LOG (default: the runtime folder).
# -----------------------------------------------------------------------------
set -u

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Two calling conventions have to work:
#   plug-in:  dlss5-worker.sh --video                      (script IS the worker path)
#   by hand:  dlss5-worker.sh ./DLSS_Nuke_Worker.exe --video
# Only treat the first argument as the executable if it actually looks like one,
# otherwise every argument belongs to the worker.
WORKER_EXE="$HERE/DLSS_Nuke_Worker.exe"
case "${1:-}" in
    *.exe|*.EXE)
        if [ -f "$1" ]; then WORKER_EXE="$1"; shift; fi
        ;;
esac

# Nuke exports its own LD_LIBRARY_PATH (and sometimes LD_PRELOAD) into every
# child process. Wine loading Nuke's bundled libstdc++/libGL instead of the
# system ones is a classic, hard-to-diagnose crash, so scrub them unless the
# caller explicitly asks to keep them.
if [ "${DLSS5_KEEP_LD_ENV:-0}" != "1" ]; then
    unset LD_LIBRARY_PATH
    unset LD_PRELOAD
fi

# The prefix deliberately lives OUTSIDE ~/.nuke. Nuke walks its plug-in path
# directories at startup, and a Wine prefix is tens of thousands of files with
# DLLs among them -- parking one inside ~/.nuke/DLSS5Live stalls or breaks Nuke
# startup, and it also means uninstalling the plug-in destroys a 500 MB Wine
# environment that takes minutes to rebuild. A prefix already at the old
# location is still honoured so existing installs keep working.
if [ -z "${WINEPREFIX:-}" ]; then
    if [ -f "$HOME/.nuke/DLSS5Live/wineprefix/system.reg" ]; then
        WINEPREFIX="$HOME/.nuke/DLSS5Live/wineprefix"
    else
        WINEPREFIX="$HOME/.local/share/dlss5-wine/prefix"
    fi
fi
: "${DLSS5_LOG:=$HERE/dlss5-worker.log}"
# NUKE_DLSS5_WINE is what the plug-in and the docs use; honour it here too, so
# one exported variable works whether the bridge launches the .exe directly or
# goes through this script.
: "${WINE:=${NUKE_DLSS5_WINE:-}}"

# Nothing named a Wine, or the name given is not runnable: look for a portable
# Wine installed by tools/setup_wine_portable.sh before giving up. Requiring
# NUKE_DLSS5_WINE in the environment means the node only works when Nuke was
# started from a shell that exported it -- launching Nuke from the desktop, or
# from a shell that has since been replaced, produces a bare "wine: command not
# found" buried in this log and a node that silently does nothing.
if [ -z "$WINE" ] || { ! command -v "$WINE" >/dev/null 2>&1 && [ ! -x "$WINE" ]; }; then
    _found=""
    # Newest version first, so an upgraded Wine is preferred automatically.
    for _c in $(ls -d "$HOME"/.local/share/dlss5-wine/wine-*/bin/wine 2>/dev/null | sort -rV); do
        [ -x "$_c" ] && { _found="$_c"; break; }
    done
    if [ -n "$_found" ]; then
        WINE="$_found"
    elif command -v wine >/dev/null 2>&1; then
        WINE="wine"
    fi
fi

export WINEPREFIX
export WINEDEBUG="${WINEDEBUG:--all}"

# Prefer native NVAPI and vkd3d-proton D3D12, but fall back to Wine's builtins.
# "=n" alone means native-ONLY: with vkd3d-proton not yet installed there is no
# d3d12.dll in the prefix, so the worker's static import cannot resolve and Wine
# fails to start the process at all -- silently, with exit code 53. "n,b" keeps
# the same preference order while staying startable.
export DXVK_ENABLE_NVAPI="${DXVK_ENABLE_NVAPI:-1}"
# These three overrides are mandatory, not defaults:
#   dxgi    - dxvk-nvapi gets its Vulkan entry point by querying the DXGI
#             factory, which Wine's builtin dxgi cannot answer. Without it
#             NvAPI_Initialize fails and NGX has no driver to interrogate.
#   d3d12   - must be vkd3d-proton, not Wine's builtin.
#   nvapi64 - dxvk-nvapi.
# Merge them into any inherited WINEDLLOVERRIDES instead of deferring to it
# wholesale: a value left exported in the caller's shell used to silently
# replace this list, and a missing dxgi entry looks exactly like a DLSS failure.
# A key can appear as "dxgi=..." or grouped as "dxgi,d3d11=...", so match the
# name followed by either '=' or ',' -- not just '='.
_dlss5_need_override() {
    case ";${WINEDLLOVERRIDES:-};" in
        *";$1="*|*",$1="*|*";$1,"*|*",$1,"*) return 1 ;;   # already mentioned
        *) return 0 ;;
    esac
}
for _ovr in "nvapi,nvapi64=n,b" "d3d12,d3d12core=n,b" "dxgi,d3d11=n,b"; do
    _key="${_ovr%%,*}"
    if _dlss5_need_override "$_key"; then
        WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}$_ovr"
    fi
done
export WINEDLLOVERRIDES

# Everything below writes to the log, never to stdout.
exec 3>&1                      # keep the protocol stdout on fd 3
exec 1>>"$DLSS5_LOG" 2>&1
echo "=== $(date -Is) launching $WORKER_EXE ==="

# ---- One-time prefix creation ------------------------------------------------
# Only ever bootstrap a prefix that does not exist yet. "wineboot -u" restores
# Wine's builtin DLLs over system32, which silently destroys the native
# vkd3d-proton / DXVK / dxvk-nvapi installs the worker depends on -- and the
# only symptom is D3D12CreateDevice failing much later. Test for drive_c rather
# than system.reg alone: a prefix whose first wineboot was interrupted has the
# directory but no registry, and re-running wineboot there is safe, while an
# established prefix must never be touched.
if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "creating Wine prefix at $WINEPREFIX"
    mkdir -p "$WINEPREFIX"
    WINEDLLOVERRIDES="mscoree,mshtml=" "$WINE" wineboot -u || true
elif [ ! -f "$WINEPREFIX/system.reg" ]; then
    echo "prefix at $WINEPREFIX has drive_c but no system.reg (interrupted setup)."
    echo "completing it with wineboot -u"
    WINEDLLOVERRIDES="mscoree,mshtml=" "$WINE" wineboot -u || true
fi

SYS32="$WINEPREFIX/drive_c/windows/system32"

# ---- Locate the driver's NGX core -------------------------------------------
# The worker's LoadCoreNGX() checks <runtime>/_nvngx.dll first, so dropping the
# driver's copy beside the worker avoids touching system32 -- and avoids a
# basename collision with this project's own nvngx.dll caller shim.
if [ ! -f "$HERE/_nvngx.dll" ]; then
    for d in /usr/lib/nvidia/wine /usr/lib64/nvidia/wine /usr/lib/x86_64-linux-gnu/nvidia/wine \
             /usr/lib/nvidia-current/wine /opt/nvidia/wine; do
        if [ -f "$d/_nvngx.dll" ]; then
            echo "copying _nvngx.dll from $d"
            cp -f "$d/_nvngx.dll" "$HERE/_nvngx.dll" && break
        fi
    done
fi
if [ ! -f "$HERE/_nvngx.dll" ]; then
    echo "WARNING: _nvngx.dll not found. Install the NVIDIA driver's Wine/NGX"
    echo "         component (it ships _nvngx.dll and nvngx.dll for Proton), or"
    echo "         copy _nvngx.dll into $HERE manually."
fi

# ---- Sanity notes on the rest of the runtime --------------------------------
for f in nvngx_dlssnr.dll nvngx.dll; do
    [ -f "$HERE/$f" ] || echo "NOTE: $HERE/$f is missing (see the project's runtime policy)."
done
# A prefix missing any of these cannot possibly work: the d3d12,d3d12core=n,b
# override falls back to Wine's builtin d3d12, the worker gets no device, and
# the only message anyone sees is "D3D12CreateDevice failed on the NVIDIA
# adapter" -- which reads like a driver or GPU problem rather than a missing
# DLL. Refuse to launch instead, and say exactly what to run.
_missing=""
for f in d3d12.dll d3d12core.dll dxgi.dll nvapi64.dll; do
    [ -f "$SYS32/$f" ] || _missing="$_missing $f"
done
if [ -n "$_missing" ]; then
    echo "ERROR: the Wine prefix is missing:$_missing"
    echo "       prefix: $WINEPREFIX"
    echo
    echo "       vkd3d-proton (d3d12/d3d12core), DXVK (dxgi) and dxvk-nvapi"
    echo "       (nvapi64) are all mandatory. Reinstall them with:"
    echo
    echo "         export NUKE_DLSS5_WINE=$WINE"
    echo "         ./tools/setup_wine_prefix.sh runtime/"
    echo
    echo "       Set DLSS5_ALLOW_INCOMPLETE_PREFIX=1 to launch anyway (it will fail)."
    [ "${DLSS5_ALLOW_INCOMPLETE_PREFIX:-0}" = "1" ] || exit 127
fi

echo "wine:    $("$WINE" --version 2>&1)"
echo "dlls:    $WINEDLLOVERRIDES"
echo "prefix:  $WINEPREFIX"
echo "worker:  $WORKER_EXE"
echo "args:    $*"

cd "$HERE" || exit 127

# Never exec into a broken command line. stdout is the protocol stream, and a
# Wine usage message printed there would be read by the plug-in as frame data --
# which is exactly how a missing worker turns into an unexplainable protocol
# error instead of a clean failure. Exiting here gives the bridge an honest EOF.
if [ ! -f "$WORKER_EXE" ]; then
    echo "ERROR: worker executable not found: $WORKER_EXE"
    echo "       args were: $*"
    exit 127
fi
if ! command -v "$WINE" >/dev/null 2>&1 && [ ! -x "$WINE" ]; then
    echo "ERROR: no usable Wine found."
    echo "       tried: '${WINE:-<empty>}', \$NUKE_DLSS5_WINE, \$HOME/.local/share/dlss5-wine/wine-*/bin/wine, PATH"
    echo "       Install one with tools/setup_wine_portable.sh, or set"
    echo "       NUKE_DLSS5_WINE=/path/to/wine before starting Nuke."
    exit 127
fi

echo "exec:    $WINE $WORKER_EXE $*"

# Restore the protocol stdout for the worker itself; its own logs stay on stderr,
# which the plug-in already routes to /dev/null.
exec 1>&3 3>&-
exec "$WINE" "$WORKER_EXE" "$@"
