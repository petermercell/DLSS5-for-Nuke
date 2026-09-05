#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
# Build the Wine prefix the DLSS worker needs: vkd3d-proton (D3D12 -> Vulkan),
# dxvk-nvapi (the NVAPI/NGX bridge), and the driver's NGX core.
#
#   export NUKE_DLSS5_WINE=~/.local/share/dlss5-wine/wine-*/bin/wine
#   ./tools/setup_wine_prefix.sh runtime/
#
# Idempotent: re-run it after changing Wine or driver versions.
# -----------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME="${1:-$REPO/runtime}"

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
: "${WINE:=${NUKE_DLSS5_WINE:-wine}}"

export WINEPREFIX WINE
export WINEDEBUG="${WINEDEBUG:--all}"

WINEDIR="$(dirname "$(command -v "$WINE" || echo "$WINE")")"
# Modern wow64 builds have no separate wine64; point the setup scripts at the
# one binary either way, since they probe for wine64 first.
export WINE64="${WINE64:-$([ -x "$WINEDIR/wine64" ] && echo "$WINEDIR/wine64" || echo "$WINE")}"
export WINESERVER="${WINESERVER:-$WINEDIR/wineserver}"
export WINEBOOT="${WINEBOOT:-$WINEDIR/wineboot}"

# The vendor setup scripts each honour a different subset of WINE/WINE64/
# WINEBOOT/WINESERVER and otherwise fall back to whatever is on PATH. With a
# portable Wine there is nothing on PATH, so put it there too.
export PATH="$WINEDIR:$PATH"

for tool in curl tar; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 1; }
done
# vkd3d-proton ships .tar.zst; GNU tar shells out to the zstd binary for it.
if ! command -v zstd >/dev/null 2>&1; then
    echo "note: 'zstd' is not installed, so the vkd3d-proton archive cannot be"
    echo "      unpacked. Install it first:  sudo dnf install zstd"
    echo
fi

echo "wine    : $("$WINE" --version 2>&1 | head -1)  ($WINE)"
echo "prefix  : $WINEPREFIX"
echo "runtime : $RUNTIME"
echo

mkdir -p "$RUNTIME"

# ---- 1. Prefix --------------------------------------------------------------
# A prefix can have a system.reg and still be unusable if its first wineboot was
# cut short. The vendor setup scripts resolve C:\windows\system32 by running
# winepath.exe through cmd, so use that same call as the liveness test rather
# than trusting the file's existence.
prefix_works() {
    [ -f "$WINEPREFIX/system.reg" ] || return 1
    local p
    p="$("$WINE" cmd /c '%SystemRoot%\system32\winepath.exe -u C:\windows\system32' 2>/dev/null)"
    p="${p%$'\r'}"
    [ -n "$p" ]
}

if [ -f "$WINEPREFIX/system.reg" ] && ! prefix_works; then
    STAMP="$(date +%Y%m%d-%H%M%S)"
    echo "==> the existing prefix does not resolve C:\\windows\\system32"
    echo "    (half-created, usually an interrupted first wineboot)"
    echo "    moving it to $WINEPREFIX.broken-$STAMP and starting fresh"
    mv "$WINEPREFIX" "$WINEPREFIX.broken-$STAMP"
fi

if [ ! -f "$WINEPREFIX/system.reg" ]; then
    echo "==> creating the prefix"
    mkdir -p "$WINEPREFIX"
    BOOTLOG="$(WINEDLLOVERRIDES="mscoree,mshtml=" "$WINE" wineboot -u 2>&1 || true)"

    # wineboot returns before wineserver has finished flushing the registry, so
    # a setup script running straight afterwards can see a prefix with no
    # system.reg and declare it invalid. Wait for the server to settle.
    "$WINESERVER" -w 2>/dev/null || true
    for _ in $(seq 1 30); do
        [ -f "$WINEPREFIX/system.reg" ] && break
        sleep 1
    done

    if [ ! -f "$WINEPREFIX/system.reg" ]; then
        echo "    FAILED: no system.reg after wineboot. Output was:"
        printf '%s\n' "$BOOTLOG" | tail -20 | sed 's/^/      /'
        # By far the most common cause on RHEL-family systems: Wine must map
        # executable memory with text relocations to load PE files, and SELinux
        # denies execmod on a portable Wine living under $HOME.
        if command -v getenforce >/dev/null 2>&1 && [ "$(getenforce)" = "Enforcing" ]; then
            echo
            echo "    SELinux is Enforcing. Check whether it denied Wine:"
            echo "      sudo ausearch -m AVC -c wine-preloader -ts recent"
            echo "    A denial on execmod / ntdll.dll is this failure. Confirm with"
            echo "    'sudo setenforce 0', then fix it properly rather than leaving"
            echo "    enforcement off:"
            echo "      sudo setsebool -P selinuxuser_execmod 1"
            echo "      sudo setsebool -P selinuxuser_execstack 1"
        fi
        echo
        echo "    Try it by hand to see the full error:"
        echo "      WINEPREFIX=$WINEPREFIX $WINE wineboot -u"
        exit 1
    fi
    echo "    ok"
else
    echo "==> prefix already exists and resolves system32"
fi

if ! prefix_works; then
    echo "    FAILED: the prefix still cannot resolve C:\\windows\\system32."
    echo "    Check by hand:"
    echo "      WINEPREFIX=$WINEPREFIX $WINE cmd /c '%SystemRoot%\\system32\\winepath.exe -u C:\\windows\\system32'"
    exit 1
fi
SYS32="$WINEPREFIX/drive_c/windows/system32"

# ---- helper: newest release tag of a GitHub repo ----------------------------
latest_tag() {
    local repo="$1" tag=""
    tag="$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" 2>/dev/null \
           | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)"
    if [ -z "$tag" ]; then
        local loc
        loc="$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
               "https://github.com/$repo/releases/latest" 2>/dev/null || true)"
        tag="${loc##*/tag/}"
        [ "$tag" = "$loc" ] && tag=""
    fi
    printf '%s' "$tag"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

install_from_release() {           # repo  name-stem  extension  dll-list  label
    local repo="$1" stem="$2" ext="$3" dlls="$4" label="$5"
    echo
    echo "==> $label"
    local tag; tag="$(latest_tag "$repo")"
    if [ -z "$tag" ]; then
        echo "    could not resolve the latest release of $repo; skipping."
        echo "    install it by hand from https://github.com/$repo/releases"
        return 1
    fi
    echo "    $tag"

    # Projects disagree about whether the asset keeps the tag's leading "v":
    # vkd3d-proton ships vkd3d-proton-3.0.1.tar.zst from tag v3.0.1, while
    # dxvk-nvapi ships dxvk-nvapi-v0.9.2.tar.gz from tag v0.9.2. Try both.
    local ver_nov="${tag#v}"
    local name="" url=""
    for cand in "${stem}-${ver_nov}${ext}" "${stem}-v${ver_nov}${ext}" "${stem}-${tag}${ext}"; do
        url="https://github.com/$repo/releases/download/$tag/$cand"
        if curl -fL --retry 2 -o "$TMP/$cand" "$url" 2>/dev/null; then
            name="$cand"; break
        fi
    done
    if [ -z "$name" ]; then
        echo "    download failed; tried:"
        for cand in "${stem}-${ver_nov}${ext}" "${stem}-v${ver_nov}${ext}" "${stem}-${tag}${ext}"; do
            echo "      $cand"
        done
        echo "    check the asset name at https://github.com/$repo/releases/tag/$tag"
        return 1
    fi
    echo "    asset: $name"
    mkdir -p "$TMP/x-$label"
    if ! tar -xf "$TMP/$name" -C "$TMP/x-$label" 2>/dev/null; then
        echo "    could not unpack $name (is zstd installed for .tar.zst?)"
        return 1
    fi

    # Install by copying the 64-bit DLLs into the prefix ourselves rather than
    # running the vendor setup script. Those scripts re-resolve the prefix
    # through 'wine cmd', which fails on a half-created prefix and on some
    # portable Wine layouts -- and all they ultimately do is this copy plus DLL
    # overrides, which dlss5-worker.sh already sets via WINEDLLOVERRIDES.
    local src=""
    for d in x64 x86_64 lib/wine/x86_64-windows; do
        if [ -d "$TMP/x-$label/$(ls "$TMP/x-$label" | head -1)/$d" ]; then
            src="$TMP/x-$label/$(ls "$TMP/x-$label" | head -1)/$d"; break
        fi
    done
    [ -z "$src" ] && src="$(find "$TMP/x-$label" -type d -name x64 | head -1)"
    if [ -z "$src" ]; then
        echo "    no x64 directory inside the tarball; contents:"
        find "$TMP/x-$label" -maxdepth 2 | head -10 | sed 's/^/      /'
        return 1
    fi

    local copied=0 f
    for f in $dlls; do
        if [ -f "$src/$f" ]; then
            cp -f "$src/$f" "$SYS32/$f" && { echo "    installed $f"; copied=$((copied+1)); }
        else
            echo "    $f not present in $src"
        fi
    done
    [ "$copied" -gt 0 ] || { echo "    nothing installed."; return 1; }
    return 0
}

install_from_release HansKristian-Work/vkd3d-proton \
    vkd3d-proton .tar.zst "d3d12.dll d3d12core.dll" vkd3d-proton || true

# DXVK is required even though nothing here draws with D3D11: dxvk-nvapi gets
# its Vulkan entry point by querying the DXGI factory, and Wine's builtin dxgi
# cannot serve that. Without DXVK's dxgi.dll, NvAPI_Initialize fails and NGX
# reports a platform error or "out of date" with no driver behind it.
install_from_release doitsujin/dxvk \
    dxvk .tar.gz "dxgi.dll d3d11.dll" dxvk || true

install_from_release jp7677/dxvk-nvapi \
    dxvk-nvapi .tar.gz "nvapi64.dll" dxvk-nvapi || true

# ---- 3. NGX core from the NVIDIA driver -------------------------------------
echo
echo "==> NGX core (_nvngx.dll)"
if [ -f "$RUNTIME/_nvngx.dll" ]; then
    echo "    already present in $RUNTIME"
else
    found=""
    for d in /usr/lib64/nvidia/wine /usr/lib/nvidia/wine \
             /usr/lib/x86_64-linux-gnu/nvidia/wine /usr/lib/nvidia-current/wine \
             /opt/nvidia/wine; do
        if [ -f "$d/_nvngx.dll" ]; then found="$d"; break; fi
    done
    if [ -z "$found" ]; then
        found="$(dirname "$(find /usr /opt -name '_nvngx.dll' 2>/dev/null | head -1)" 2>/dev/null || true)"
        [ "$found" = "." ] && found=""
    fi
    if [ -n "$found" ]; then
        cp -f "$found/_nvngx.dll" "$RUNTIME/_nvngx.dll"
        echo "    copied from $found"
    else
        echo "    NOT FOUND on this system."
        echo "    The NVIDIA driver ships _nvngx.dll and nvngx.dll for Wine/Proton,"
        echo "    but only when its NGX component is installed. Find the package with:"
        echo "        dnf provides '*/wine/_nvngx.dll'"
        echo "    Without it the worker cannot load the NGX core and init will fail."
    fi
fi

# ---- 4. Report --------------------------------------------------------------
echo
echo "==> prefix contents"
# Wine ships its own builtin d3d12.dll/d3d12core.dll, so mere presence proves
# nothing -- what matters is whether these are vkd3d-proton's. Its binaries
# carry the string and are an order of magnitude larger than the builtins.
# vkd3d-proton splits itself the way the Agility SDK does: d3d12core.dll holds
# the implementation (~6 MB) and d3d12.dll is a thin forwarder stub. So size
# alone only means something for d3d12core.dll -- judging d3d12.dll by it
# reports a perfectly good stub as Wine's builtin.
is_vkd3d() { strings -a "$1" 2>/dev/null | grep -qi 'vkd3d'; }

CORE="$SYS32/d3d12core.dll"
CORE_OK=0
if [ -f "$CORE" ]; then
    if is_vkd3d "$CORE" || [ "$(stat -c%s "$CORE")" -gt 1000000 ]; then CORE_OK=1; fi
fi

if [ ! -f "$SYS32/d3d12.dll" ]; then
    echo "    MISSING d3d12.dll"
elif [ "$CORE_OK" = 1 ]; then
    printf '    ok      %-16s %s bytes (forwarder stub)\n' "d3d12.dll" "$(stat -c%s "$SYS32/d3d12.dll")"
elif is_vkd3d "$SYS32/d3d12.dll"; then
    printf '    ok      %-16s vkd3d-proton\n' "d3d12.dll"
else
    printf '    BUILTIN %-16s %s bytes -- Wine'"'"'s own d3d12, NOT vkd3d-proton.\n' \
        "d3d12.dll" "$(stat -c%s "$SYS32/d3d12.dll")"
fi

if [ ! -f "$CORE" ]; then
    echo "    MISSING d3d12core.dll -- vkd3d-proton is not installed; NGX needs it."
elif [ "$CORE_OK" = 1 ]; then
    printf '    ok      %-16s %s bytes (vkd3d-proton)\n' "d3d12core.dll" "$(stat -c%s "$CORE")"
else
    printf '    BUILTIN %-16s %s bytes -- not vkd3d-proton; NGX needs it.\n' \
        "d3d12core.dll" "$(stat -c%s "$CORE")"
fi
if [ -f "$SYS32/nvapi64.dll" ]; then echo "    ok      nvapi64.dll"; else echo "    MISSING nvapi64.dll"; fi

# Wine ships a builtin dxgi too, and dxvk-nvapi specifically needs DXVK's.
# Identify by 8-bit strings, UTF-16 strings (PE version resources are wide, which
# is why a plain `strings` misses DXVK), and finally size: Wine's builtin dxgi is
# a few hundred KB, DXVK's is several MB.
if [ ! -f "$SYS32/dxgi.dll" ]; then
    echo "    MISSING dxgi.dll -- dxvk-nvapi cannot get a Vulkan entry point without DXVK's DXGI."
elif strings -a "$SYS32/dxgi.dll" 2>/dev/null | grep -qi 'dxvk' \
     || strings -a -el "$SYS32/dxgi.dll" 2>/dev/null | grep -qi 'dxvk' \
     || [ "$(stat -c%s "$SYS32/dxgi.dll")" -gt 1000000 ]; then
    printf '    ok      %-16s %s bytes (DXVK)\n' "dxgi.dll" "$(stat -c%s "$SYS32/dxgi.dll")"
else
    printf '    BUILTIN %-16s %s bytes -- Wine'"'"'s own dxgi, not DXVK'"'"'s.\n' \
        "dxgi.dll" "$(stat -c%s "$SYS32/dxgi.dll")"
    echo "            NvAPI_Initialize will fail against it, and NGX will report"
    echo "            a platform error that looks like a DLSS problem but is not."
fi
echo
echo "==> runtime contents ($RUNTIME)"
for f in DLSS_Nuke_Worker.exe nvngx.dll _nvngx.dll nvngx_dlssnr.dll nvngx_dlss.dll; do
    if [ -f "$RUNTIME/$f" ]; then
        printf '    ok      %-22s %s\n' "$f" "$(du -h "$RUNTIME/$f" | cut -f1)"
    else
        printf '    MISSING %s\n' "$f"
    fi
done

cat <<EOF

Next: tests/worker_probe.cpp against this runtime.

  g++ -std=c++17 -O1 -o /tmp/worker_probe tests/worker_probe.cpp \\
      src/WorkerBridge_posix.cpp src/Platform.cpp -Isrc -lpthread -ldl
  WINE=$WINE /tmp/worker_probe $RUNTIME/dlss5-worker.sh
EOF
