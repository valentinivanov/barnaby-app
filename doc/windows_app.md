# Windows Barnaby App

This document records the current Windows 11 porting state for Barnaby, the
native app wrapper, and the Bazel environment needed to build and test it.

## Current Shape

The Windows app target is:

```powershell
bazel build //src/windows:BarnabyWindows
```

The cross-platform app alias also selects it on Windows:

```powershell
bazel build //:barnaby_app
```

`BarnabyWindows` is a native Win32 launcher. It:

- locates `gitboard.exe` and `gitboard-server.exe` beside the app or in the
  adjacent Bazel output layout;
- starts `gitboard-server --port 0 --gitboard-path ... --no-open-browser`;
- starts the server without showing a separate console window;
- reads the machine-readable `GITBOARD_SERVER_URL=...` line from server stdout;
- shows a native splash screen with the embedded Barnaby icon while startup
  tasks run;
- creates an embedded Chromium Embedded Framework browser in the main window;
- navigates the embedded web view to the server URL;
- stops the server process when the Barnaby window closes.

The runtime behavior now matches the macOS `WKWebView` and Linux CEF wrappers:
Barnaby is a standalone app window hosting the same local web UI, backed by the
same local server process.

The splash screen is created before Chromium Embedded Framework initialization
and before the blocking helper startup path begins. `libcef.dll` is linked with
MSVC delay-loading so Windows can enter the Barnaby launcher and paint the
splash before the first CEF API call loads Chromium. The splash uses the app
icon embedded by `src/windows/BarnabyWin.rc`, paints the `Barnaby` title in a
small borderless Win32 window, and is dismissed immediately after the main app
window is shown. CEF subprocess launches are detected from their `--type=...`
argument and skip the splash path.

## Required Tools

The Windows build currently uses:

- Bazel 9.1.1 or compatible.
- Visual Studio Build Tools with the VC toolchain.
- Git for Windows Bash, because Bazel's Windows test machinery needs a Unix
  shell toolchain.
- Chromium Embedded Framework binary SDK.
- vcpkg-provided libcurl, as configured by `tools/curl_config.bzl`.
- `vswhere.exe`, normally installed with Visual Studio, for locating the active
  Visual Studio Build Tools installation.

## Environment Setup

In a plain PowerShell session, `cl.exe` may not be on `PATH`. That does not mean
the compiler is missing. Visual Studio's developer scripts and Bazel both build a
toolchain environment by layering Visual Studio, VC tools, Windows SDK, .NET SDK,
MSBuild, and the inherited process environment.

For packaging, prefer the checked-in helper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_windows.ps1
```

`scripts/package_windows.ps1` finds Visual Studio with `vswhere.exe`, imports
`VsDevCmd.bat -arch=x64 -host_arch=x64`, discovers `vcpkg.exe` from the imported
developer environment, and passes the resulting `VCPKG_ROOT`, `BAZEL_VS`,
`BAZEL_VC`, `BAZEL_VC_FULL_VERSION`, and `BAZEL_WINSDK_FULL_VERSION` values to
Bazel as repository environment. This keeps local install paths out of the
script while still giving Bazel's C++ toolchain detection the explicit values it
expects on Windows.

`BAZEL_SH` is still honored when already set. It is useful for test targets that
need a Unix shell through Bazel's Windows test machinery, but the Windows asset
generation path and portable packaging path do not require Bash.

For ad hoc direct `bazel build` or `bazel test` commands, run them from a
Visual Studio developer environment or pass the same repository environment that
the package script derives. The package script is the canonical example of that
setup.

CEF is resolved by the platform-aware Bazel module extension in
`tools/cef_config.bzl`. The lookup order is:

- `CEF_ROOT`, when explicitly set;
- `dependencies\cef`, when it already contains an unpacked SDK;
- the pinned Spotify CDN CEF archive, downloaded and extracted into Bazel's
  external repository cache.

The pinned Windows archive is:

```text
https://cef-builds.spotifycdn.com/cef_binary_149.0.5%2Bg6770623%2Bchromium-149.0.7827.197_windows64_minimal.tar.bz2
```

The unpacked SDK layout must contain:

```text
include\cef_app.h
Release\libcef.dll
Release\libcef.lib
Resources\*.pak
Resources\locales\*.pak
```

The repo exposes CEF through `tools/cef_config.bzl` as `@local_cef//:cef` and
`@local_cef//:runtime_files` on Windows and Linux. The extension compiles CEF's
`libcef_dll_wrapper` sources directly, so a prebuilt
`libcef_dll_wrapper.lib` is not required.

`dependencies\cef` is a transitional local cache and is ignored by Git. If it is
missing or empty, Bazel downloads the pinned SDK automatically when network
access is available. That automatic download is extracted into Bazel's external
repository cache, for example under `_bazel_<user>\...\external\+cef_config+local_cef`;
it does not create `dependencies\cef` in the workspace.

libcurl is resolved by `tools/curl_config.bzl`. On Windows, `VCPKG_ROOT` must
point at a vcpkg checkout or bundled Visual Studio vcpkg directory. The package
script derives this from `VsDevCmd.bat` when possible. The Bazel repository rule
then checks for `curl:x64-windows` under:

```text
installed\x64-windows
packages\curl_x64-windows
```

If those layouts are missing but `vcpkg.exe` exists, the rule tries to install
curl. It first attempts classic mode:

```powershell
vcpkg install curl:x64-windows
```

If the Visual Studio bundled vcpkg requires manifest mode, the rule writes a
small generated `vcpkg.json` inside Bazel's external repository directory, runs
manifest install with the `x64-windows` triplet, and adds an initial baseline
when that vcpkg instance requires one.

If using the Visual Studio scripts manually, the relevant entrypoints are:

```text
<Visual Studio installation>\Common7\Tools\VsDevCmd.bat
<Visual Studio installation>\VC\Auxiliary\Build\vcvarsall.bat
```

`vcvarsall.bat x64` calls `VsDevCmd.bat -arch=x64 -host_arch=x64`. `VsDevCmd.bat`
then calls component scripts under `Common7\Tools\VsDevCmd\core` and
`Common7\Tools\VsDevCmd\ext`, such as `winsdk.bat`, `msbuild.bat`, `vcvars.bat`,
`netfxsdk.bat`, `cmake.bat`, `roslyn.bat`, and `vcpkg.bat`. Those scripts prepend
their directories to `PATH`, `INCLUDE`, `LIB`, and `LIBPATH`.

## Build Commands

Build the portable zip and copy it into the workspace `dist` folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_windows.ps1
```

That script builds `//:barnaby_windows_portable` and copies:

```text
bazel-bin\dist\barnaby_windows_portable.zip
```

to:

```text
dist\Barnaby-0.4.2-windows-portable.zip
```

Release versions are managed with `scripts/set_version.sh`; see
`doc/release_versioning.md`. To override only the package output name for one
build, pass `-OutputName` or set `BARNABY_VERSION`.

Build individual targets directly when your shell has already been initialized
with Visual Studio developer environment values, or when passing equivalent
`--repo_env` values:

```powershell
bazel build //src:gitboard
bazel build //src:gitboard-server
bazel build //src/windows:BarnabyWindows
bazel build //:barnaby_app
```

Set `CEF_ROOT` only when you want to force a specific unpacked CEF SDK instead
of using `dependencies\cef` or the pinned download.

Build the distribution target:

```powershell
bazel build //:gitboard_dist
```

`//:gitboard_dist` has a Windows branch in `tools/package.bzl` that uses
`cmd.exe` commands and emits `gitboard.exe` and `gitboard-server.exe`.

Build the portable zip directly with Bazel:

```powershell
bazel build //:barnaby_windows_portable
```

The Bazel target emits its tracked output under:

```text
bazel-bin/dist/barnaby_windows_portable.zip
```

The zip contains:

```text
Barnaby.exe
gitboard.exe
gitboard-server.exe
config/statuses.json
libcef.dll
CEF support DLLs
CEF .pak/.dat resources
locales/*.pak
```

The launcher executable has Windows version metadata and the Barnaby app icon
embedded via `src/windows/BarnabyWin.rc`.

## Tests

Run the full test suite from a Visual Studio developer environment. If Git Bash
is not on `PATH`, set `BAZEL_SH` to `bash.exe` before invoking Bazel:

```powershell
bazel test //tests:all --verbose_failures
```

As of the current porting pass, this passes on Windows:

```text
7 tests pass
```

The test fixes made for Windows include:

- selecting MSVC copts in `tests/BUILD.bazel`;
- using `_popen` and `_pclose` in `positive_test.cc`;
- using Windows command quoting in `positive_test.cc`;
- replacing POSIX process/socket code in `server_startup_test.cc` with Win32 and
  Winsock equivalents;
- resolving Bazel runfiles manifest paths for `$(location)` executable args;
- stopping the child server before deleting its temp config directory, because
  Windows does not allow deleting open locked files.

The tests do not build `//src/windows:BarnabyWindows`; that target additionally
requires a usable CEF SDK via `CEF_ROOT`, `dependencies\cef`, or the pinned
download into Bazel's external repository cache.

## Agent Pip Secrets

Windows uses Credential Manager for AI backend API keys. The server reports:

```json
{
  "secretStorage": "windows-credential-manager",
  "secretStorageWarning": null
}
```

API keys are stored as generic credentials with target names prefixed by:

```text
dev.gitboard.barnaby.ai:
```

The persisted `config.json` stores only `apiKeyRef`, never the key value. This
matches the macOS Keychain and Linux Secret Service behavior expected by Agent
Pip's AI backend configuration UI.

## Generated Assets

The server embeds web assets through `//src:generated_assets`.

Unix-like builds use:

```text
src/server/generate_assets.sh
```

Windows builds use:

```text
src/server/generate_assets.ps1
```

`src/BUILD.bazel` wires the PowerShell script through `cmd_bat`, so Windows
asset generation does not require Bash.

## Known Warnings

The current Visual Studio Build Tools install emits:

```text
LINK : warning LNK4315: /DEBUG:FASTLINK is no longer supported.
Using /DEBUG:FULL instead.
```

This warning does not currently block builds or tests.

## Remaining Work

- Runtime smoke-test the portable zip, including CEF subprocess launch and
  resource lookup.
- Re-run positive tests with `--config=run_tests` when validating full CLI
  behavior against the built binary.
