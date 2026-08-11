# Barnaby

![Barnaby and Pip](https://github.com/valentinivanov/barnaby-app/blob/main/doc/agent_pip_and_barnaby_the_task_engine_forest.png)

Barnaby is a local-first desktop task board for projects that already live in
Git. It stores tasks as Markdown files with structured front matter, keeps task
history reviewable through normal Git workflows, and provides a small local web
UI wrapped in native desktop apps for macOS, Linux, and Windows.

The project is built around three pieces:

- `gitboard`: the task engine and command-line interface.
- `gitboard-server`: a loopback HTTP API and embedded web UI host.
- `Barnaby`: the desktop wrapper that launches the local server and displays the
  UI in a native app window.

## Goals

- Keep project tasks portable, inspectable, and version-controlled.
- Avoid hosted task-tracker lock-in for small teams and local development work.
- Provide a fast desktop workflow while preserving scriptable CLI behavior.
- Let users manage tasks directly inside local Git repositories.

## Features

- Markdown task files under each repository's `tasks/` directory.
- YAML front matter for status, priority, assignee, story points, branches, PRs,
  tags, and timestamps.
- Configurable workflow statuses through `tasks/statuses.json`.
- Local repository registration and validation through `gitboard-server`.
- Embedded web UI for browsing repositories and managing task boards.
- Native desktop wrappers:
  - macOS uses `WKWebView`.
  - Linux and Windows use Chromium Embedded Framework.
- AI provider integration support in the local server, with platform-aware
  secret storage.

## Why Barnaby

- **Git-native:** task changes can be reviewed, branched, synced, and recovered
  with the same tools as source code.
- **Local-first:** project data stays in the working tree instead of a remote
  service by default.
- **Simple data model:** tasks remain human-readable Markdown files.
- **Cross-platform:** the same engine and web UI are packaged for the major
  desktop platforms.
- **Scriptable core:** automation can use the CLI or the server API without
  depending on the desktop shell.
- **Agent Pip:** a bring-your-own-key AI agent can manage tasks, review code,
  and help draft project documentation while working against the same local
  task data.

## Running
Barnaby assumes there is Git CLI available in the PATH. Otherwise it has no additional dependencies.

## Building

Barnaby uses Bazel for C++ builds. The main development targets are:

```sh
bazel build //src:gitboard //src:gitboard-server
bazel test //tests:smoke_test //tests:positive_test
```

Build the platform-selected desktop app with:

```sh
bazel build //:barnaby_app
```

### macOS

Package a universal macOS zip and DMG:

```sh
scripts/package_macos.sh
```

Use `--sign` or `--notarize` for release builds. See
[`doc/macos_app.md`](doc/macos_app.md) for signing, notarization, and app bundle
details.

### Linux

Build the CEF desktop target:

```sh
bazel build //src:BarnabyLinux
```

Build a Flatpak bundle:

```sh
scripts/package_linux_flatpack.sh
```

Build an AppDir and, when `appimagetool` is available, an AppImage:

```sh
scripts/package_linux_appimage.sh
```

See [`doc/linux_app.md`](doc/linux_app.md) for Fedora setup, Flatpak/AppImage
packaging, CEF runtime layout, and Linux-specific dependencies.

### Windows

From PowerShell, build the portable Windows package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_windows.ps1
```

The script initializes the Visual Studio build environment and builds
`//:barnaby_windows_portable`. See [`doc/windows_app.md`](doc/windows_app.md)
for required tools, CEF/vcpkg setup, and direct Bazel build commands.

## Documentation

- [`doc/spec.md`](doc/spec.md): task engine behavior and file format.
- [`doc/server_spec.md`](doc/server_spec.md): local HTTP API and server model.
- [`doc/web_ui.md`](doc/web_ui.md): embedded web UI design.
- [`doc/statuses_config.md`](doc/statuses_config.md): workflow status
  configuration.
- [`doc/release_versioning.md`](doc/release_versioning.md): version and release
  management.

## License

Barnaby is licensed under the MIT License. See [`LICENSE`](LICENSE).
