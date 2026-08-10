#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/package_macos.sh [--sign] [--notarize] [output_zip_name]

Builds universal Barnaby macOS zip and dmg artifacts in dist/.

Options:
  --sign       Sign Barnaby.app with a Developer ID Application certificate.
  --notarize  Sign, notarize, staple, and package release artifacts.
  -h, --help  Show this help.

Environment:
  BARNABY_MACOS_SIGN_IDENTITY
      Required with --sign or --notarize. Example:
      Developer ID Application: Your Name or Company (TEAMID)

  BARNABY_NOTARY_PROFILE
      xcrun notarytool keychain profile name created with:
      xcrun notarytool store-credentials <profile>

  APPLE_ID, APPLE_TEAM_ID, APPLE_APP_SPECIFIC_PASSWORD
      Used for notarization when BARNABY_NOTARY_PROFILE is not set.
EOF
}

sign_app=0
notarize_app=0

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
default_version="${BARNABY_VERSION:-$(tr -d '[:space:]' < "$repo_root/VERSION" 2>/dev/null || echo 0.4.2)}"
output_name="Barnaby-${default_version}-macos-universal.zip"

notary_args=()

build_notary_args() {
  notary_args=()
  if [[ -n "${BARNABY_NOTARY_PROFILE:-}" ]]; then
    notary_args+=(--keychain-profile "$BARNABY_NOTARY_PROFILE")
  else
    missing=()
    [[ -n "${APPLE_ID:-}" ]] || missing+=(APPLE_ID)
    [[ -n "${APPLE_TEAM_ID:-}" ]] || missing+=(APPLE_TEAM_ID)
    [[ -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ]] || missing+=(APPLE_APP_SPECIFIC_PASSWORD)
    if [[ "${#missing[@]}" -gt 0 ]]; then
      echo "Missing notarization credentials: ${missing[*]}" >&2
      echo "Set BARNABY_NOTARY_PROFILE or APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_SPECIFIC_PASSWORD." >&2
      exit 1
    fi
    notary_args+=(
      --apple-id "$APPLE_ID"
      --team-id "$APPLE_TEAM_ID"
      --password "$APPLE_APP_SPECIFIC_PASSWORD"
    )
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sign)
      sign_app=1
      shift
      ;;
    --notarize)
      sign_app=1
      notarize_app=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      output_name="$1"
      shift
      if [[ $# -gt 0 ]]; then
        echo "Unexpected extra argument: $1" >&2
        usage >&2
        exit 2
      fi
      ;;
  esac
done

cd "$repo_root"

lockfile_was_clean=1
if ! git diff --quiet -- MODULE.bazel.lock ||
   ! git diff --cached --quiet -- MODULE.bazel.lock; then
  lockfile_was_clean=0
fi

bazel build --config=universal_macos //:barnaby_app

if [[ "$lockfile_was_clean" -eq 1 ]] && ! git diff --quiet -- MODULE.bazel.lock; then
  git checkout -- MODULE.bazel.lock
fi

mkdir -p dist
output_path="dist/$output_name"
dmg_name="${output_name%.zip}.dmg"
if [[ "$dmg_name" == "$output_name" ]]; then
  dmg_name="${output_name}.dmg"
fi
dmg_path="dist/$dmg_name"
rm -f "$output_path" "$dmg_path"

if [[ "$sign_app" -eq 0 ]]; then
  cp bazel-bin/src/Barnaby.zip "$output_path"
  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/barnaby-macos-package.XXXXXX")"
  cleanup() {
    rm -rf "$work_dir"
  }
  trap cleanup EXIT

  ditto -x -k bazel-bin/src/Barnaby.zip "$work_dir"
  app_path="$work_dir/Barnaby.app"
  if [[ ! -d "$app_path" ]]; then
    echo "Expected Barnaby.app in bazel-bin/src/Barnaby.zip" >&2
    exit 1
  fi

  dmg_root="$work_dir/dmg-root"
  mkdir -p "$dmg_root"
  cp -R "$app_path" "$dmg_root/Barnaby.app"
  ln -s /Applications "$dmg_root/Applications"
  hdiutil create -volname "Barnaby" \
    -srcfolder "$dmg_root" \
    -ov -format UDZO \
    "$dmg_path"
else
  if [[ -z "${BARNABY_MACOS_SIGN_IDENTITY:-}" ]]; then
    echo "Missing signing identity: set BARNABY_MACOS_SIGN_IDENTITY." >&2
    exit 1
  fi
  sign_identity="$BARNABY_MACOS_SIGN_IDENTITY"
  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/barnaby-macos-package.XXXXXX")"
  cleanup() {
    rm -rf "$work_dir"
  }
  trap cleanup EXIT

  ditto -x -k bazel-bin/src/Barnaby.zip "$work_dir"
  app_path="$work_dir/Barnaby.app"
  if [[ ! -d "$app_path" ]]; then
    echo "Expected Barnaby.app in bazel-bin/src/Barnaby.zip" >&2
    exit 1
  fi

  xattr -dr com.apple.quarantine "$app_path" 2>/dev/null || true

  echo "Signing Barnaby.app with: $sign_identity"
  codesign --force --timestamp --options runtime --all-architectures \
    --sign "$sign_identity" \
    "$app_path/Contents/Resources/gitboard_universal"
  codesign --force --timestamp --options runtime --all-architectures \
    --sign "$sign_identity" \
    "$app_path/Contents/Resources/gitboard_server_universal"
  codesign --force --timestamp --options runtime --all-architectures \
    --sign "$sign_identity" \
    "$app_path/Contents/MacOS/Barnaby"
  codesign --force --timestamp --options runtime --all-architectures \
    --sign "$sign_identity" \
    "$app_path"
  codesign --verify --strict --all-architectures --deep --verbose=2 "$app_path"

  if [[ "$notarize_app" -eq 1 ]]; then
    notary_zip="$work_dir/Barnaby-notary.zip"
    ditto -c -k --sequesterRsrc --keepParent "$app_path" "$notary_zip"

    build_notary_args

    echo "Submitting Barnaby.app for notarization"
    xcrun notarytool submit "$notary_zip" --wait "${notary_args[@]}"
    xcrun stapler staple "$app_path"
    xcrun stapler validate "$app_path"
  fi

  ditto -c -k --sequesterRsrc --keepParent "$app_path" "$output_path"

  dmg_root="$work_dir/dmg-root"
  mkdir -p "$dmg_root"
  cp -R "$app_path" "$dmg_root/Barnaby.app"
  ln -s /Applications "$dmg_root/Applications"
  hdiutil create -volname "Barnaby" \
    -srcfolder "$dmg_root" \
    -ov -format UDZO \
    "$dmg_path"

  codesign --force --timestamp --sign "$sign_identity" "$dmg_path"
  codesign --verify --verbose=2 "$dmg_path"

  if [[ "$notarize_app" -eq 1 ]]; then
    echo "Submitting $dmg_name for notarization"
    xcrun notarytool submit "$dmg_path" --wait "${notary_args[@]}"
    xcrun stapler staple "$dmg_path"
    xcrun stapler validate "$dmg_path"
  fi
fi

shasum -a 256 "$output_path" "$dmg_path" > dist/SHA256SUMS

echo "Wrote dist/$output_name"
echo "Wrote dist/$dmg_name"
echo "Wrote dist/SHA256SUMS"
