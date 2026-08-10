#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/set_version.sh VERSION [BUILD]

Updates Barnaby version strings across build metadata, packaging defaults,
docs, and user-visible UI text.

VERSION must be numeric semver: MAJOR.MINOR.PATCH
BUILD defaults to the current CFBundleVersion when present, otherwise 1.

Examples:
  scripts/set_version.sh 0.4.2
  scripts/set_version.sh 0.4.2 7
  BARNABY_VERSION=0.4.2 scripts/set_version.sh
EOF
}

version="${1:-${BARNABY_VERSION:-}}"
build="${2:-${BARNABY_BUILD:-}}"

if [[ -z "$version" || "$version" == "-h" || "$version" == "--help" ]]; then
  usage
  [[ -z "$version" ]] && exit 2
  exit 0
fi

if [[ ! "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "VERSION must match MAJOR.MINOR.PATCH, got: $version" >&2
  exit 2
fi

major="${BASH_REMATCH[1]}"
minor="${BASH_REMATCH[2]}"
patch="${BASH_REMATCH[3]}"

if [[ -z "$build" ]]; then
  build="$(perl -0ne 'print $1 if /<key>CFBundleVersion<\/key>\s*<string>([^<]+)<\/string>/' src/macos/Info.plist)"
  build="${build:-1}"
fi

if [[ ! "$build" =~ ^[0-9]+$ ]]; then
  echo "BUILD must be a positive integer, got: $build" >&2
  exit 2
fi

version_commas="$major,$minor,$patch,$build"

printf '%s\n' "$version" > VERSION

perl -0pi -e 's/module\(name = "gitboard_cpp", version = "[^"]+"\)/module(name = "gitboard_cpp", version = "'$version'")/' MODULE.bazel

perl -0pi -e 's/(build_version = ")[^"]+(")/${1}'$build'$2/; s/(short_version_string = ")[^"]+(")/${1}'$version'$2/' src/BUILD.bazel

perl -0pi -e 's/(<key>CFBundleShortVersionString<\/key>\s*<string>)[^<]+(<\/string>)/${1}'$version'$2/; s/(<key>CFBundleVersion<\/key>\s*<string>)[^<]+(<\/string>)/${1}'$build'$2/' src/macos/Info.plist

perl -0pi -e 's/FILEVERSION\s+[0-9]+,[0-9]+,[0-9]+,[0-9]+/FILEVERSION '$version_commas'/; s/PRODUCTVERSION\s+[0-9]+,[0-9]+,[0-9]+,[0-9]+/PRODUCTVERSION '$version_commas'/; s/VALUE "FileVersion", "[^"]+"/VALUE "FileVersion", "'$version'"/; s/VALUE "ProductVersion", "[^"]+"/VALUE "ProductVersion", "'$version'"/' src/windows/BarnabyWin.rc

perl -0pi -e 's/echo [0-9]+\.[0-9]+\.[0-9]+\)/echo '$version')/g' scripts/package_macos.sh scripts/package_linux_appimage.sh scripts/package_linux_flatpack.sh
perl -0pi -e 's/"version-string": "[^"]+"/"version-string": "'$version'"/' tools/curl_config.bzl

perl -0pi -e 's/(info:\s*\n\s*title: GitBoard Server API\s*\n\s*version:\s*)[0-9]+\.[0-9]+\.[0-9]+/${1}'$version'/' doc/server_openapi.yaml
perl -0pi -e 's/GitBoard Server API [0-9]+\.[0-9]+\.[0-9]+/GitBoard Server API '$version'/g' src/server/assets/app.js

perl -0pi -e 's/VERSION [0-9]+\.[0-9]+\.[0-9]+/VERSION '$version'/g' website/index.html
perl -0pi -e 's/<release version="[0-9]+\.[0-9]+\.[0-9]+"/<release version="'$version'"/g' packaging/linux/dev.gitboard.barnaby.metainfo.xml

perl -0pi -e 's/[0-9]+\.[0-9]+\.[0-9]+/'$version'/g' doc/release_versioning.md
perl -0pi -e 's/Barnaby-[0-9]+\.[0-9]+\.[0-9]+-macos-universal\.zip/Barnaby-'$version'-macos-universal.zip/g; s/Barnaby-[0-9]+\.[0-9]+\.[0-9]+-macos-universal\.dmg/Barnaby-'$version'-macos-universal.dmg/g' doc/macos_app.md doc/audit_report_07082026.md doc/release_versioning.md
perl -0pi -e 's/Barnaby-[0-9]+\.[0-9]+\.[0-9]+-windows-portable\.zip/Barnaby-'$version'-windows-portable.zip/g' doc/windows_app.md
perl -0pi -e 's/Barnaby-[0-9]+\.[0-9]+\.[0-9]+-x86_64\.flatpak/Barnaby-'$version'-x86_64.flatpak/g; s/Barnaby-[0-9]+\.[0-9]+\.[0-9]+-x86_64\.AppImage/Barnaby-'$version'-x86_64.AppImage/g; s/`[0-9]+\.[0-9]+\.[0-9]+` version/`'$version'` version/g; s/<release version="[0-9]+\.[0-9]+\.[0-9]+"/<release version="'$version'"/g' doc/linux_app.md

echo "Updated Barnaby version to $version (build $build)."
