#!/bin/bash
# Build a universal Atrim Linux AppImage (x86_64).
# For maximum distro compatibility, run this on the OLDEST system you want to
# support (e.g. Ubuntu 22.04, glibc 2.35) — an AppImage built on a newer
# glibc will not start on older distros. See .github/workflows/appimage.yml.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/opencode/atrim-build}"
APPDIR="${APPDIR:-/tmp/opencode/appimage/AppDir}"
TOOLS_DIR="${TOOLS_DIR:-/tmp/opencode/tools}"
VERSION="$(grep -m1 'project(AtrimLauncher VERSION' "$REPO_DIR/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"

for tool in linuxdeploy linuxdeploy-plugin-qt appimagetool; do
  if [ ! -x "$TOOLS_DIR/$tool" ]; then
    echo "Missing $TOOLS_DIR/$tool — download it first:"
    echo "  linuxdeploy:            https://github.com/linuxdeploy/linuxdeploy/releases"
    echo "  linuxdeploy-plugin-qt:  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases"
    echo "  appimagetool:           https://github.com/AppImage/appimagetool/releases"
    exit 1
  fi
done

echo "==> Configuring ($REPO_DIR, version $VERSION)"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "==> Building"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> Installing into AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$REPO_DIR/packaging/atrim-launcher.png" \
   "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/atrim-launcher.png" \
   "$APPDIR/atrim-launcher.png"
cp "$APPDIR/usr/share/applications/atrim-launcher.desktop" "$APPDIR/"

echo "==> Bundling Qt + deps (linuxdeploy)"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$(command -v qmake6 || command -v qmake || true)"
"$TOOLS_DIR/linuxdeploy" \
  --appdir "$APPDIR" \
  --plugin qt \
  --output appimage

echo "==> Done:"
ls -l ./*.AppImage "$APPDIR"/../*.AppImage 2>/dev/null || ls -l ./*.AppImage
