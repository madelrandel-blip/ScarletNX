#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Makes dist/ScarletNX-linux-<arch>.AppImage, a runtime that runs on any Linux, with pkgforge's quick-sharun
# (https://github.com/pkgforge-dev/Anylinux-AppImages): it bundles the runtime with every library it loads, SDL3, the
# Vulkan loader and Mesa's Vulkan drivers included. CI runs it in pkgforge's Arch container after `cmake --install`
# into /usr (.github/workflows/build.yml), and quick-sharun has to be on PATH (pkgforge-dev/anylinux-setup-action puts
# it there).
#
# A copy named after a game works like the bare runtime: games/Catherine.AppImage (or games/Catherine) loads
# games/Catherine_code.so, since the runtime takes its name and folder from $APPIMAGE.
set -euo pipefail

ARCH=$(uname -m)
REPOSITORY=${GITHUB_REPOSITORY:-scarletnx/ScarletNX}
VERSION=${VERSION:-$(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)}
export ARCH VERSION
export OUTPATH=./dist
export OUTNAME=ScarletNX-linux-$ARCH.AppImage
# .DirIcon, what file managers and integrators show, is this PNG (docs.appimage.org/reference/appdir.html)
export ICON=/usr/share/icons/hicolor/256x256/apps/wimsy.png
export DESKTOP=/usr/share/applications/scarletnx.desktop
# AppImageUpdate finds new builds through the .zsync file next to the AppImage in the latest release
# (scripts/publish-builds.py): a version's in this repository's, a nightly's in the owner's nightly repository
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
    export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|${REPOSITORY#*/}|latest|*$ARCH.AppImage.zsync"
else
    export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|nightly|latest|*$ARCH.AppImage.zsync"
fi
# The runtime exits at once without a game module, so tracing it for dlopened libraries finds nothing: name them
export STRACE_MODE=0 DEPLOY_SDL=1 DEPLOY_VULKAN=1 DEPLOY_OPENGL=1

# The runtime stays the AppImage's program (the desktop entry's Exec), so a copy named after a game runs that game.
# Started bare it opens its launcher, which runs Switch games with it and the other drivers from beside it
# (scarletnx-3ds, scarletnx-gnuabi)
quick-sharun /usr/bin/scarletnx /usr/bin/scarletnx-switch /usr/bin/scarletnx-3ds /usr/bin/scarletnx-gnuabi

# The licenses travel with the AppImage: the runtime's and those of the libraries built into it (`cmake --install` put
# them in /usr/share/licenses/scarletnx), and those of the Arch packages quick-sharun took the bundled libraries
# from, with a list of those packages, their versions and licenses
appdir=${APPDIR:-$PWD/AppDir}
if [ ! -d "$appdir/lib" ]; then
    echo "quick-sharun left no $appdir/lib: where it puts the bundled libraries has changed" >&2
    exit 1
fi
if [ "$(head -c 8 "$appdir/.DirIcon" | od -An -tx1 | tr -d ' \n')" != 89504e470d0a1a0a ]; then
    echo "$appdir/.DirIcon is not a PNG: quick-sharun no longer takes ICON for it" >&2
    exit 1
fi
# and the vector next to it, the best the desktop entry's Icon= can name
cp /usr/share/icons/hicolor/scalable/apps/wimsy.svg "$appdir/"
mkdir -p "$appdir/share/licenses"
cp -r /usr/share/licenses/scarletnx "$appdir/share/licenses/"
mapfile -t packages < <(cd "$appdir/lib" && find . -name '*.so*' -type f | while read -r lib; do
    pacman -Qqo "/usr/lib/${lib#./}" 2>/dev/null || true
done | sort -u)
for pkg in "${packages[@]}"; do
    pacman -Qi "$pkg" | grep -E '^(Name|Version|Licenses) '
    echo
    if [ -d "/usr/share/licenses/$pkg" ]; then
        cp -r "/usr/share/licenses/$pkg" "$appdir/share/licenses/"
    fi
done >"$appdir/share/licenses/bundled-packages.txt"

quick-sharun --make-appimage

# Without a game the runtime says which module it looked for. Under another name, it must look next to the AppImage
# for that name's module, which shows the libraries load and the name and folder come from $APPIMAGE. With --game it
# must look for the module that names
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
cp "$OUTPATH/$OUTNAME" "$probe/Probe.AppImage"
check() { # expected module, then the AppImage's arguments
    local want=$1 out
    shift
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 ./Probe.AppImage "$@" 2>&1 || true)
    if ! grep -qF "$want" <<<"$out"; then
        echo "The AppImage did not look for $want. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
}
# -H: started bare, it would open the launcher instead
check "$probe/Probe_code.so" -H
check "$probe/games/Other_code.so" --game="$probe/games/Other"
# Started bare it opens the launcher, which needs everything it runs beside it (the launcher itself can't be run
# here: without a display SDL may fall back to an offscreen one and keep it open)
for program in scarletnx-3ds scarletnx-gnuabi; do
    if [ ! -e "$appdir/bin/$program" ]; then
        echo "The AppImage has no bin/$program, which the launcher runs" >&2
        exit 1
    fi
done
echo "AppImage checked: it looks for its game module next to itself or where --game says, and has the launcher"
