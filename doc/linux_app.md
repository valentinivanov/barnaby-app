# Barnaby Linux App Plan

Barnaby should ship on Linux with the same split used by the macOS app:

- `gitboard` remains the task engine and CLI.
- `gitboard-server` remains the local HTTP API and embedded web UI host.
- The Linux desktop app owns the desktop lifecycle and displays the local UI in
  a Chromium Embedded Framework window.

The first Linux implementation should stay intentionally thin. It should launch
a bundled `gitboard-server` helper with `--port 0`, read the loopback URL chosen
by the server, pass the bundled `gitboard` helper path with `--gitboard-path`,
suppress browser launch with `--no-open-browser`, load the server URL in a
CEF browser window, and terminate the helper when the app exits.

## Fedora 44 Development Environment

Install the base build tools, native splash dependencies, and runtime helpers:

```sh
sudo dnf install git gcc-c++ make pkgconf-pkg-config libcurl-devel \
  libsecret-devel libX11-devel cairo-devel gdk-pixbuf2-devel lsof xdg-utils
```

The desktop wrapper uses the Chromium Embedded Framework binary SDK resolved by
Bazel. No GTK/WebKit development packages are required for `//src:BarnabyLinux`.
The launch splash is native Linux UI built with X11, Cairo, and gdk-pixbuf so
it appears before CEF startup work begins.

Install Bazel through Bazelisk or another Bazel distribution available on the
developer machine. Fedora does not need a different Barnaby source layout, but
the build needs a `bazel` command that can build the existing C++ targets.

The Fedora 44 development host has been checked with these dependencies
installed: GCC/G++, Git, Make, `pkg-config`, `lsof`, `xdg-utils`,
`libcurl-devel`, `libsecret-devel`, `libX11-devel`, `cairo-devel`, and
`gdk-pixbuf2-devel`.
`bazel` and `bazelisk` are available under `~/.local/bin`. Flatpak and
`flatpak-builder` are required when beginning the second packaging milestone.

`gitboard-server` links to libcurl for outbound AI HTTP requests. Do not replace
this with the command-line `curl` executable or any shell-mediated HTTP path;
the Bazel build should fail if libcurl development headers or link metadata are
not available.

On Linux, `gitboard-server` stores AI API keys with Secret Service through
libsecret. `config.json` stores only a `linux-secret-service:...` reference; the
secret value must never be written to plain-text config. If Secret Service is
unavailable, saving or loading an API key should fail clearly rather than falling
back to a local file.

Install the Flatpak packaging tools when starting that milestone:

```sh
sudo dnf install flatpak flatpak-builder
```

Verify the basic engine and server targets first:

```sh
bazel build //src:gitboard //src:gitboard-server
bazel test //tests:smoke_test //tests:positive_test
```

Run the current server flow on Linux before adding the desktop wrapper:

```sh
bazel run //src:gitboard-server -- --port 8080
```

The standalone server should open the web UI with `xdg-open`. The desktop app
will instead pass `--no-open-browser` and host the same URL inside CEF.

## Desktop Wrapper

The Linux-specific wrapper is:

```text
src/linux/BarnabyCef.cc
```

The wrapper should:

- show a native splash screen with the Barnaby icon while startup work runs;
- initialize CEF;
- resolve the bundled `gitboard-server` and `gitboard` helper paths;
- launch `gitboard-server --port 0 --gitboard-path PATH --no-open-browser`;
- read the selected loopback URL from the server startup output;
- create a browser window titled `Barnaby`;
- load the reported loopback URL in a CEF browser;
- dismiss the splash screen as soon as the main window is shown;
- print a clear startup error if the helper cannot start or CEF cannot
  initialize;
- terminate the helper process when the window closes or the app exits.

Linux uses the same `tools/cef_config.bzl` module extension as Windows. The
extension selects the platform archive at repository-fetch time. The pinned
Linux archive is:

```text
https://cef-builds.spotifycdn.com/cef_binary_149.0.6%2Bg0d0eeb6%2Bchromium-149.0.7827.201_linux64_minimal.tar.bz2
```

The unpacked Linux SDK layout must contain:

```text
include/cef_app.h
Release/libcef.so
Resources/resources.pak
Resources/locales/*.pak
```

The repo exposes CEF through `@local_cef//:cef` and
`@local_cef//:runtime_files` on Linux and Windows.

## Helper Path Resolution

The wrapper must not depend on the current working directory. It should resolve
paths relative to its own executable and support both development and packaged
layouts.

Development layout:

```text
Barnaby
gitboard
gitboard-server
```

Bazel development output layout:

```text
bazel-bin/src/
  gitboard
  gitboard-server
  linux/
    BarnabyLinux
```

Packaged layout:

```text
bin/Barnaby
lib/barnaby/gitboard
lib/barnaby/gitboard-server
```

The explicit `--gitboard-path` option should always be passed to the server so
the server never has to guess in the desktop app path.

## Bazel Targets

Keep the existing macOS targets in place and add Linux-only targets rather than
trying to make `rules_apple` cross-platform.

Suggested targets:

```text
//src:BarnabyLinux
```

`//src:BarnabyLinux` is a `cc_binary` for the CEF wrapper. It depends on
`@local_cef//:cef` and carries `@local_cef//:runtime_files` as runtime data.

Packaging is the second milestone, after a working CEF desktop wrapper has been
verified on Fedora 44.

## Flatpak Distribution

The Linux release is packaged as a Flatpak that stages the CEF runtime beside
the Barnaby executable under `/app/lib/barnaby/cef`.

Use `dev.gitboard.barnaby` as the canonical Linux application ID. It matches
the GTK application ID and the macOS bundle identifier, and should be used for
the Flatpak manifest, desktop entry, metainfo file, and installed icon names.

Add a manifest at:

```text
packaging/linux/dev.gitboard.barnaby.yml
```

The manifest uses a supported Linux desktop runtime and matching SDK. CEF
runtime files are installed in the layout expected by `src/linux/BarnabyCef.cc`.

The Flatpak install layout should be:

```text
/app/bin/Barnaby
/app/bin/git
/app/libexec/git-core/...
/app/lib/barnaby/gitboard
/app/lib/barnaby/gitboard-server
/app/lib/barnaby/cef/libcef.so
/app/lib/barnaby/cef/resources.pak
/app/lib/barnaby/cef/locales/*.pak
/app/share/applications/dev.gitboard.barnaby.desktop
/app/share/metainfo/dev.gitboard.barnaby.metainfo.xml
/app/share/icons/hicolor/scalable/apps/dev.gitboard.barnaby.svg
/app/share/icons/hicolor/128x128/apps/dev.gitboard.barnaby.png
/app/share/icons/hicolor/256x256/apps/dev.gitboard.barnaby.png
```

This layout matches the wrapper's packaged lookup: executable `/app/bin/Barnaby`
resolves both helpers under `/app/lib/barnaby` and CEF resources under
`/app/lib/barnaby/cef`.

The manifest compiles the Linux wrapper and CEF `libcef_dll` wrapper sources
directly inside `org.gnome.Sdk` using:

```text
packaging/linux/build_barnaby_cef.sh
```

The application runtime does not supply the `git` executable needed by
repository validation and task synchronization. The manifest should therefore
build and install a pinned Git source release into `/app`.

The initial `finish-args` should cover Wayland, fallback X11, shared-memory
use, graphics acceleration, network access for Git remotes, and the user's
home directory for local repositories. It should also allow D-Bus access to
Secret Service so `gitboard-server` can store AI API keys through libsecret:

```yaml
finish-args:
  - --share=ipc
  - --share=network
  - --socket=wayland
  - --socket=fallback-x11
  - --device=dri
  - --talk-name=org.freedesktop.secrets
  - --filesystem=home
```

Barnaby must also read and write user-selected Git repositories. Repository
registration should provide a native GTK/portal-backed folder chooser, matching
the macOS app and a future Windows client rather than requiring users to type
filesystem paths. The server remains responsible for validation and stored
repository mappings.

### Filesystem Access Decision

The Linux Flatpak should grant `--filesystem=home`. Barnaby is a developer
tool whose primary purpose is to discover, read, and update local Git working
trees; broad home-directory access keeps adding and reopening repositories
predictable and allows bundled Git operations to work without portal-document
indirection.

The native repository chooser remains important for convenience, but is not
the Flatpak permission boundary in the current release design. A later release
may revisit this permission and adopt more narrowly granted repository access
if it provides the same reliable Git workflow.

## AppImage Distribution

The CEF-based Linux app can also be staged as an AppDir and optionally bundled
as an AppImage. This path is separate from the Flatpak manifest and is driven by:

```text
scripts/package_linux_appimage.sh
```

The script builds `//src:BarnabyLinux`, stages the executable, helper binaries,
CEF runtime files, desktop metadata, and icons under:

```text
build-appimage/Barnaby.AppDir
```

If `appimagetool` is available on `PATH`, or `APPIMAGETOOL=/path/to/appimagetool`
is set, the script also writes the final bundle to `dist`.

The previous GTK/WebKit AppImage approach was rejected because it depended on
host GTK/WebKit compatibility or a large bundled WebKit runtime. That rejection
does not apply to the CEF-based package layout, which already stages the browser
runtime beside the Barnaby executable.

## Desktop Metadata

The Linux desktop integration source files are:

```text
packaging/linux/dev.gitboard.barnaby.desktop
packaging/linux/dev.gitboard.barnaby.metainfo.xml
packaging/icons/app_icon.svg
packaging/icons/app_icon_256x256.png
packaging/icons/app_icon_128x128.png
```

The `.desktop` file is required for Flatpak desktop export and should be
installed as `/app/share/applications/dev.gitboard.barnaby.desktop`. Its
initial content is:

```ini
[Desktop Entry]
Type=Application
Name=Barnaby
GenericName=Task Board
Comment=Manage project tasks stored in Git repositories
Exec=Barnaby
Icon=dev.gitboard.barnaby
Terminal=false
Categories=Development;ProjectManagement;
StartupNotify=true
```

`Icon` intentionally omits the file extension. `Exec` starts the desktop
wrapper, not either bundled command-line helper. No MIME types or file handlers
are required for the first release.

AppStream metainfo should be installed in the Flatpak so application catalogs
and distribution metadata can describe Barnaby. The current content is:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>dev.gitboard.barnaby</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>

  <name>Barnaby</name>
  <summary>Manage project tasks stored in Git repositories</summary>
  <developer id="dev.gitboard">
    <name>Valentyn Ivanov</name>
  </developer>

  <description>
    <p>
      Barnaby is a desktop task board backed by files in local Git repositories.
      It provides an embedded web interface for viewing and managing project tasks.
    </p>
  </description>

  <launchable type="desktop-id">dev.gitboard.barnaby.desktop</launchable>

  <url type="homepage">https://github.com/valentinivanov/gitboard-cpp</url>
  <url type="bugtracker">https://github.com/valentinivanov/gitboard-cpp/issues</url>

  <content_rating type="oars-1.1"/>
</component>
```

The repository already establishes the application ID, name, `0.4.2` version,
MIT project license, and GitHub project URL. Before publishing a Flatpak,
confirm the summary and description, add screenshot URLs once release-quality
screenshots exist, and confirm the release date in the AppStream metadata.
Release versions are managed with `scripts/set_version.sh`; see
`doc/release_versioning.md`.

## Icon Assets

The dedicated Barnaby icon source and raster exports are:

```text
packaging/icons/app_icon.svg
packaging/icons/app_icon_128x128.png
packaging/icons/app_icon_256x256.png
```

The Flatpak manifest should install these files with the application ID as the
installed basename under `/app/share/icons/hicolor/`. Additional standard sizes
can be added as artwork is finalized.

## Metadata Validation

Before building a release Flatpak, validate the source metadata:

```sh
desktop-file-validate packaging/linux/dev.gitboard.barnaby.desktop
appstreamcli validate --no-net packaging/linux/dev.gitboard.barnaby.metainfo.xml
```

On Fedora 44 these commands are provided by `desktop-file-utils` and
`appstream`, both of which are installed on the current development host.
Use `--no-net` for deterministic local and CI checks; validation without that
flag additionally attempts to verify external project URLs.
After building the Flatpak, verify that its exported desktop entry, metainfo,
and installed icons retain the `dev.gitboard.barnaby` names.

## Port Selection

Port selection is part of the first Linux milestone. Change `gitboard-server`
so `--port 0` binds an OS-selected loopback port and prints the actual listening
URL in a machine-readable startup line. The Linux wrapper should launch the
server using that contract instead of selecting and releasing a candidate port
itself.

The macOS app currently selects an available port and releases it before
launching `gitboard-server`. It can adopt the same server-managed startup
contract in a follow-up change.

## Milestones

### Milestone 1: Fedora Desktop App

1. Confirm the existing CLI, tests, and `gitboard-server` build and run on
   Fedora 44.
2. Add the `gitboard-server --port 0` startup contract and tests.
3. Add `src/linux/BarnabyCef.cc` with CEF lifecycle parity.
4. Add Bazel support for compiling the Linux wrapper with the shared
   platform-aware CEF repository rule.
5. Run a manual desktop smoke test on Fedora 44.
6. Add CI coverage for Linux engine/server tests and, if practical, a wrapper
   build check on Fedora or Ubuntu.

### Milestone 2: Flatpak Distribution

1. [DONE] Confirm the selected desktop runtime can build the bundled CEF
   runtime package layout.
2. [DONE] Update `packaging/linux/dev.gitboard.barnaby.yml` to build and install the
   CEF wrapper, CEF runtime files, both helper binaries, desktop/AppStream
   metadata, and icons.
3. [DONE] Select `--filesystem=home` for local repository access in the
   Flatpak; reconsider narrower access in a future release.
4. Build, install, and run the Flatpak on Fedora 44, including adding
   and modifying tasks in a test Git repository.
5. Test the same Flatpak build on at least one non-Fedora desktop
   distribution: clean Ubuntu 26.04 works after installing Flatpak and the
   selected runtime.
6. Prepare Flathub submission metadata and release information once the
   sandboxed application flow is verified.

## Manual Acceptance Test

For the first desktop milestone, from a Fedora 44 host:

```sh
sudo dnf install git gcc-c++ make pkgconf-pkg-config lsof xdg-utils \
  libcurl-devel libsecret-devel libX11-devel cairo-devel gdk-pixbuf2-devel
bazel build //src:gitboard //src:gitboard-server //src:BarnabyLinux
```

Then run the development desktop binary; it resolves the helper binaries from
the neighboring Bazel output directory:

```sh
./bazel-bin/src/linux/BarnabyLinux
```

Build, install, and run the CEF Flatpak locally:

```sh
scripts/package_linux_flatpack.sh
flatpak install --user ./dist/Barnaby-0.4.2-x86_64.flatpak
flatpak run dev.gitboard.barnaby
```

To use a custom bundle filename:

```sh
scripts/package_linux_flatpack.sh Barnaby-0.4.2-x86_64.flatpak
```

The resulting distributable application bundle is:

```text
dist/Barnaby-0.4.2-x86_64.flatpak
```

The script builds into `build-flatpak`, exports through the local Flatpak
repository at `repo`, writes the bundle into `dist`, and records the bundle
checksum in `dist/SHA256SUMS`. It passes `--disable-rofiles-fuse` to avoid a
`rofiles-fuse` cleanup failure observed on the Fedora 44 development host.

Install and run that bundle with:

```sh
flatpak install --user ./dist/Barnaby-0.4.2-x86_64.flatpak
flatpak run dev.gitboard.barnaby
```

Build the AppDir and, when `appimagetool` is installed, the AppImage:

```sh
scripts/package_linux_appimage.sh
```

To use a custom AppImage filename:

```sh
scripts/package_linux_appimage.sh Barnaby-0.4.2-x86_64.AppImage
```

The script always prepares:

```text
build-appimage/Barnaby.AppDir
```

When `appimagetool` is present it also writes:

```text
dist/Barnaby-0.4.2-x86_64.AppImage
dist/SHA256SUMS.AppImage
```

### Ubuntu 26.04 Bundle Installation

The CEF-based bundle needs fresh Ubuntu validation after the Flatpak package
build is smoke-tested on Fedora 44.

Install Flatpak:

```sh
sudo apt update
sudo apt install flatpak
```

Add Flathub for the current user:

```sh
flatpak --user remote-add --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo
```

Install the runtime required by `packaging/linux/dev.gitboard.barnaby.yml`:

```sh
flatpak install --user flathub org.gnome.Platform//50
```

Install and run Barnaby from the downloaded bundle:

```sh
flatpak install --user ./Barnaby-0.4.2-x86_64.flatpak
flatpak run dev.gitboard.barnaby
```

Expected behavior:

- a `Barnaby` desktop window opens;
- no external browser opens;
- the embedded UI loads from a loopback URL;
- adding an authorized local Git repository works;
- task operations use the bundled `gitboard` helper;
- closing the window terminates the bundled `gitboard-server` helper.

The Fedora 44 Flatpak build currently completes and exports desktop/AppStream
metadata. A graphical runtime smoke test is still required to confirm CEF starts
inside the sandbox and renders the embedded UI.

## Later Native Enhancements

Good follow-up work after the wrapper works:

- AppStream screenshots, release signing, and update metadata;
- single-instance behavior through the existing server lock plus a user-facing
  "already running" dialog;
- native/portal-backed repository chooser for convenient registration;
- optional future evaluation of narrower Flatpak filesystem access;
- application menu actions for refresh and opening repository folders;
- RPM packaging if native Fedora installation becomes useful;
- optional native notifications for task or sync events.

## Packaging References

- GNOME Flatpak distribution guidance:
  <https://developer.gnome.org/documentation/introduction/flatpak.html>
- Flatpak manifest documentation:
  <https://docs.flatpak.org/en/latest/manifests.html>
- Flatpak requirements and desktop export conventions:
  <https://docs.flatpak.org/en/latest/conventions.html>
- Desktop Entry Specification:
  <https://specifications.freedesktop.org/desktop-entry/latest-single/>
- AppStream desktop application metadata:
  <https://www.freedesktop.org/software/appstream/docs/sect-Metadata-Application.html>

## Fedora Package References

As of Fedora 44, the relevant package names are:

- `libcurl-devel`: libcurl headers and link metadata for server-side AI HTTP.
- `libsecret-devel`: libsecret headers and link metadata for Linux Secret Service.
- `pkgconf-pkg-config`: `/usr/bin/pkg-config` provider.
- `gcc-c++`: C++ compiler package.
