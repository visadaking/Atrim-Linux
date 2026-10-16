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
if [ -z "$APPDIR" ] || [ "$APPDIR" = "/" ]; then
  echo "Refusing to rm -rf on APPDIR='$APPDIR' — set APPDIR to a build directory." >&2
  exit 1
fi
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
  --plugin qt

# linuxdeploy-plugin-qt sometimes ships only the platform plugin in use
# (e.g. xcb). A missing offscreen/minimal/wayland plugin aborts the app
# with SIGABRT on systems without X11, so backfill every platform plugin
# (and Wayland integration plugins) from the Qt install, then let
# linuxdeploy pull in their extra dependencies.
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
if [ -n "$QT_PLUGINS" ] && [ -d "$QT_PLUGINS/platforms" ]; then
  echo "==> Backfilling all Qt platform plugins from $QT_PLUGINS"
  mkdir -p "$APPDIR/usr/plugins/platforms"
  cp -n "$QT_PLUGINS"/platforms/*.so "$APPDIR/usr/plugins/platforms/" 2>/dev/null || true
  for plugdir in wayland-shell-integration wayland-graphics-integration-client \
                  wayland-decoration-client xcbglintegrations egldeviceintegrations; do
    if [ -d "$QT_PLUGINS/$plugdir" ]; then
      mkdir -p "$APPDIR/usr/plugins/$plugdir"
      cp -n "$QT_PLUGINS/$plugdir"/*.so "$APPDIR/usr/plugins/$plugdir/" 2>/dev/null || true
    fi
  done
  echo "==> Re-running linuxdeploy for backfilled plugin deps"
  "$TOOLS_DIR/linuxdeploy" \
    --appdir "$APPDIR" \
    --plugin qt
else
  echo "WARNING: could not locate Qt plugins dir; AppImage may miss platform plugins"
fi

echo "==> Building AppImage (appimagetool)"
RUNTIME_FILE="${RUNTIME_FILE:-$TOOLS_DIR/runtime-x86_64}"
if [ ! -f "$RUNTIME_FILE" ]; then
  curl -sL --retry 2 -o "$RUNTIME_FILE" \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
fi
OUT="AtrimLinux-${VERSION}-x86_64.AppImage"
rm -f "$OUT"
"$TOOLS_DIR/appimagetool" \
  --runtime-file "$RUNTIME_FILE" \
  "$APPDIR" "$OUT"

echo "==> Done:"
ls -l "$OUT"
