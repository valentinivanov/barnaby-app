#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
default_version="${BARNABY_VERSION:-$(tr -d '[:space:]' < "$repo_root/VERSION" 2>/dev/null || echo 0.4.2)}"
output_name="${1:-Barnaby-${default_version}-x86_64.AppImage}"

app_id="dev.gitboard.barnaby"
appdir="${APPDIR:-build-appimage/Barnaby.AppDir}"
appimagetool="${APPIMAGETOOL:-appimagetool}"

cd "$repo_root"

for command in bazel sha256sum; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Missing required command: $command" >&2
    exit 1
  fi
done

echo "Building Linux Barnaby binaries with Bazel"
bazel build //src:BarnabyLinux --verbose_failures

runfiles="bazel-bin/src/linux/BarnabyLinux.runfiles"
cef_root="$runfiles/+cef_config+local_cef"
if [[ ! -f "$cef_root/Release/libcef.so" ||
      ! -f "$cef_root/Resources/resources.pak" ]]; then
  echo "Could not find CEF runtime files under $cef_root" >&2
  exit 1
fi

echo "Staging AppDir at $appdir"
rm -rf "$appdir"
mkdir -p \
  "$appdir/usr/bin" \
  "$appdir/usr/lib/barnaby/cef" \
  "$appdir/usr/share/applications" \
  "$appdir/usr/share/metainfo" \
  "$appdir/usr/share/icons/hicolor/scalable/apps" \
  "$appdir/usr/share/icons/hicolor/128x128/apps" \
  "$appdir/usr/share/icons/hicolor/256x256/apps"

install -Dm755 bazel-bin/src/linux/BarnabyLinux "$appdir/usr/bin/Barnaby"
install -Dm755 bazel-bin/src/gitboard "$appdir/usr/lib/barnaby/gitboard"
install -Dm755 bazel-bin/src/gitboard-server "$appdir/usr/lib/barnaby/gitboard-server"

cp -aL "$cef_root/Release/." "$appdir/usr/lib/barnaby/cef/"
cp -aL "$cef_root/Resources/." "$appdir/usr/lib/barnaby/cef/"

install -Dm644 packaging/linux/dev.gitboard.barnaby.desktop \
  "$appdir/usr/share/applications/$app_id.desktop"
install -Dm644 packaging/linux/dev.gitboard.barnaby.metainfo.xml \
  "$appdir/usr/share/metainfo/$app_id.metainfo.xml"
install -Dm644 packaging/icons/app_icon.svg \
  "$appdir/usr/share/icons/hicolor/scalable/apps/$app_id.svg"
install -Dm644 packaging/icons/app_icon_128x128.png \
  "$appdir/usr/share/icons/hicolor/128x128/apps/$app_id.png"
install -Dm644 packaging/icons/app_icon_256x256.png \
  "$appdir/usr/share/icons/hicolor/256x256/apps/$app_id.png"
install -Dm644 packaging/icons/app_icon_256x256.png "$appdir/$app_id.png"
install -Dm644 packaging/linux/dev.gitboard.barnaby.desktop \
  "$appdir/$app_id.desktop"

cat > "$appdir/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

appdir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export PATH="$appdir/usr/bin:$PATH"
export LD_LIBRARY_PATH="$appdir/usr/lib/barnaby/cef${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec "$appdir/usr/bin/Barnaby" "$@"
EOF
chmod +x "$appdir/AppRun"

if ! command -v "$appimagetool" >/dev/null 2>&1; then
  echo "Prepared $appdir"
  echo "Install appimagetool or set APPIMAGETOOL=/path/to/appimagetool to build the final AppImage."
  exit 0
fi

mkdir -p dist
rm -f "dist/$output_name" dist/SHA256SUMS.AppImage

arch="$(uname -m)"
echo "Bundling $appdir into dist/$output_name"
ARCH="$arch" "$appimagetool" "$appdir" "dist/$output_name"

sha256sum "dist/$output_name" > dist/SHA256SUMS.AppImage

echo "Wrote dist/$output_name"
echo "Wrote dist/SHA256SUMS.AppImage"
