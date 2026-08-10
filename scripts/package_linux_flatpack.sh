#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
default_version="${BARNABY_VERSION:-$(tr -d '[:space:]' < "$repo_root/VERSION" 2>/dev/null || echo 0.4.2)}"
output_name="${1:-Barnaby-${default_version}-x86_64.flatpak}"

manifest="packaging/linux/dev.gitboard.barnaby.yml"
build_dir="build-flatpak"
repo_dir="repo"
runtime_repo="https://dl.flathub.org/repo/flathub.flatpakrepo"
app_id="dev.gitboard.barnaby"
branch="master"

cd "$repo_root"

for command in flatpak flatpak-builder sha256sum; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Missing required command: $command" >&2
    exit 1
  fi
done

arch="$(flatpak --default-arch)"

echo "Building Flatpak app from $manifest"
flatpak-builder \
  --disable-rofiles-fuse \
  --user \
  --force-clean \
  --repo="$repo_dir" \
  "$build_dir" \
  "$manifest"

mkdir -p dist
rm -f "dist/$output_name" dist/SHA256SUMS

echo "Bundling $app_id/$arch/$branch into dist/$output_name"
flatpak build-bundle \
  --arch="$arch" \
  --runtime-repo="$runtime_repo" \
  "$repo_dir" \
  "dist/$output_name" \
  "$app_id" \
  "$branch"

sha256sum "dist/$output_name" > dist/SHA256SUMS

echo "Wrote dist/$output_name"
echo "Wrote dist/SHA256SUMS"
