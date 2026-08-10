# Release Versioning

Barnaby keeps the release version in the repository root `VERSION` file. Package
scripts use that file for their default output names, and each script can be
overridden for one build with `BARNABY_VERSION`.

Update all checked-in version metadata before a release:

```bash
scripts/set_version.sh 0.4.2
```

To set the platform build number at the same time:

```bash
scripts/set_version.sh 0.4.2 7
```

The same values can be supplied through environment variables:

```bash
BARNABY_VERSION=0.4.2 BARNABY_BUILD=7 scripts/set_version.sh
```

The script updates the root `VERSION` file, Bazel module metadata, macOS bundle
metadata, Windows resource metadata, Linux AppStream metadata, package filename
defaults, API documentation, the embedded web UI version label, the public
website version label, and release documentation examples.

For one-off package filenames without changing checked-in metadata, set
`BARNABY_VERSION` when running a package script:

```bash
BARNABY_VERSION=0.4.2 scripts/package_macos.sh
BARNABY_VERSION=0.4.2 scripts/package_linux_flatpack.sh
BARNABY_VERSION=0.4.2 scripts/package_linux_appimage.sh
```

The macOS package script emits both:

```text
dist/Barnaby-0.4.2-macos-universal.zip
dist/Barnaby-0.4.2-macos-universal.dmg
```

On Windows:

```powershell
$env:BARNABY_VERSION = "0.4.2"
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_windows.ps1
```
