#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
# Install a portable Wine that does not touch RPM.
#
# Rocky 9.8 dropped mesa-libOSMesa and mesa-libGL now *obsoletes* it, so EPEL's
# wine-core cannot be installed at all: the old package conflicts with your
# current mesa. Rather than fight that, this fetches a Kron4ek portable build --
# self-contained, no installation, no 32-bit libraries, and far newer than
# EPEL's Wine 8.0.
#
#   ./tools/setup_wine_portable.sh                 # latest staging, wow64
#   ./tools/setup_wine_portable.sh --dir /opt/wine
#   ./tools/setup_wine_portable.sh --version 10.6 --variant vanilla
#
# Prints the NUKE_DLSS5_WINE line to use when done.
# -----------------------------------------------------------------------------
set -euo pipefail

DEST="$HOME/.local/share/dlss5-wine"
VARIANT="staging"      # vanilla | staging | staging-tkg
VERSION=""             # empty = latest release

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)     DEST="$2"; shift 2 ;;
        --variant) VARIANT="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

REPO="Kron4ek/Wine-Builds"

if [ -z "$VERSION" ]; then
    echo "==> resolving latest $REPO release"

    # Try the API first, but it is rate-limited per source IP, so fall back to
    # following the /releases/latest redirect, which is not.
    API="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null || true)"
    VERSION="$(printf '%s' "$API" \
               | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)"

    if [ -z "$VERSION" ]; then
        LOC="$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
               "https://github.com/$REPO/releases/latest" 2>/dev/null || true)"
        VERSION="${LOC##*/tag/}"
        [ "$VERSION" = "$LOC" ] && VERSION=""
    fi

    if [ -z "$VERSION" ]; then
        echo "could not resolve the latest release automatically." >&2
        echo "Pick a version from https://github.com/$REPO/releases and pass --version X.Y" >&2
        exit 1
    fi
fi

# wow64 builds run 64-bit Windows binaries with no 32-bit host libraries at all,
# which is exactly what the DLSS worker needs and avoids EL9's multilib gaps.
case "$VARIANT" in
    vanilla) NAME="wine-${VERSION}-amd64-wow64" ;;
    *)       NAME="wine-${VERSION}-${VARIANT}-amd64-wow64" ;;
esac
TARBALL="${NAME}.tar.xz"
URL="https://github.com/$REPO/releases/download/${VERSION}/${TARBALL}"

echo "==> version $VERSION, variant $VARIANT"
echo "    $URL"

mkdir -p "$DEST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! curl -fL --retry 3 -o "$TMP/$TARBALL" "$URL"; then
    echo >&2
    echo "download failed. Check the asset name on the release page:" >&2
    echo "  https://github.com/$REPO/releases/tag/${VERSION}" >&2
    exit 1
fi

echo "==> extracting to $DEST"
tar -xf "$TMP/$TARBALL" -C "$DEST"

WINE_BIN="$DEST/$NAME/bin/wine"
if [ ! -x "$WINE_BIN" ]; then
    # Some builds nest differently; find it rather than guess.
    WINE_BIN="$(find "$DEST" -maxdepth 3 -type f -name wine -perm -u+x | head -1 || true)"
fi
if [ -z "$WINE_BIN" ] || [ ! -x "$WINE_BIN" ]; then
    echo "extracted, but no wine binary found under $DEST" >&2
    exit 1
fi

echo
echo "==> checking shared library dependencies"
MISSING="$(ldd "$WINE_BIN" 2>/dev/null | awk '/not found/{print $1}' | sort -u || true)"
if [ -n "$MISSING" ]; then
    echo "    missing libraries for the wine loader itself:"
    echo "$MISSING" | sed 's/^/      /'
    echo "    install them with dnf and re-run this check."
else
    echo "    loader ok"
fi

# The wineserver and the PE loader pull in more than the wine binary does; a
# version query exercises the real startup path.
echo
echo "==> $("$WINE_BIN" --version 2>&1 | head -1)"

cat <<EOF

Portable Wine is ready.

  export NUKE_DLSS5_WINE=$WINE_BIN

Add that to the environment Nuke starts in, or pass WINE=$WINE_BIN to
dlss5-worker.sh. Then continue with step 4 of BUILD-LINUX.md -- install
vkd3d-proton and dxvk-nvapi into the prefix using this same wine:

  export WINEPREFIX=\$HOME/.local/share/dlss5-wine/prefix
  export WINE=$WINE_BIN
  export WINESERVER=$(dirname "$WINE_BIN")/wineserver
EOF
