# Barnaby macOS App

Barnaby should ship as a native macOS application bundle while preserving the
existing GitBoard architecture:

- `gitboard` remains the task engine and CLI.
- `gitboard-server` remains the local HTTP API and embedded web UI host.
- `Barnaby.app` owns the desktop lifecycle and displays the local UI in a
  native `WKWebView`.

## First Implementation

The first app bundle is intentionally thin:

- launch a bundled `gitboard-server` helper on an available loopback port,
- pass the bundled `gitboard` helper path with `--gitboard-path`,
- suppress browser launch with `--no-open-browser`,
- load the server URL in a native WebKit window,
- terminate the helper when the app exits.

This keeps task behavior, repository configuration, API behavior, and UI assets
shared with the existing server target.

## Repository Selection Decision

Barnaby desktop apps should let the user choose a local Git repository with the
platform's native folder picker rather than requiring manual path entry. On
macOS, `Barnaby.app` should provide an `NSOpenPanel` directory-selection flow
from the Add Repository UI.

The wrapper should return the selected absolute path to the embedded web UI.
The UI should then register it through `POST /api/repos`, leaving validation
and persistent repository configuration with `gitboard-server`. Linux should
offer the equivalent native/portal-backed flow, and a future Windows wrapper
should use its native folder chooser.

## Build Output

For local development builds, the Bazel target:

```text
//:barnaby_app
```

uses `rules_apple`'s `macos_application` rule and emits:

```text
bazel-bin/src/Barnaby.zip
```

The zip contains `Barnaby.app`.

## Distribution Build

Use the packaging script from the repository root:

```bash
scripts/package_macos.sh
```

The script runs the checked-in universal macOS Bazel build:

```text
bazel build --config=universal_macos //:barnaby_app
```

The app executable and bundled helper executables are built as universal
`x86_64`/`arm64` Mach-O binaries. The helper resources are named
`gitboard_universal` and `gitboard_server_universal` inside
`Contents/Resources`.

After the build, the script writes:

```text
dist/Barnaby-0.4.2-macos-universal.zip
dist/Barnaby-0.4.2-macos-universal.dmg
```

It also regenerates:

```text
dist/SHA256SUMS
```

To use a different output zip name, pass it as the first argument. The DMG name
is derived from that zip name:

```bash
scripts/package_macos.sh Barnaby-0.4.2-macos-universal.zip
```

By default, the script still produces an unsigned local-development zip. For a
Developer ID signed build, pass `--sign`:

```bash
scripts/package_macos.sh --sign
```

For a signed and notarized release build, pass `--notarize`:

```bash
scripts/package_macos.sh --notarize
```

`--notarize` implies `--sign`. The script signs the bundled helper executables,
the app executable, and then `Barnaby.app` with hardened runtime enabled. It
submits an app zip to Apple with `xcrun notarytool`, staples the app
notarization ticket with `xcrun stapler`, validates the staple, then writes both
the final stapled zip and a signed, notarized, stapled DMG to the distribution
folder.

```text
dist/Barnaby-0.4.2-macos-universal.zip
dist/Barnaby-0.4.2-macos-universal.dmg
```

To use a different output zip name with notarization, pass it after the option:

```bash
scripts/package_macos.sh --notarize Barnaby-0.4.2-macos-universal.zip
```

Release versions are managed with `scripts/set_version.sh`; see
`doc/release_versioning.md`.

## Later Native Enhancements

Good next steps after the wrapper works:

- app icon and document-quality bundle metadata,
- menu commands for refresh, settings, and opening repository folders,
- native folder picker wired into repository registration as specified above,
- single-instance behavior through `NSRunningApplication` or a bundle-level
  lock,
- optional native notifications for task or sync events.

## Signing And Notarization

The release signing identity is:

```text
CERTIFICATE_SHA1 "Developer ID Application: Your Name or Company (TEAMID)"
```

The packaging script requires an explicit signing identity:

```text
Developer ID Application: Your Name or Company (TEAMID)
```

To provide the identity, set:

```bash
export BARNABY_MACOS_SIGN_IDENTITY="Developer ID Application: Your Name or Company (TEAMID)"
```

For notarization credentials, prefer a local notarytool keychain profile:

```bash
xcrun notarytool store-credentials barnaby-notary \
  --apple-id "APPLE_ID_EMAIL" \
  --team-id "TEAMID" \
  --password "APP_SPECIFIC_PASSWORD"

export BARNABY_NOTARY_PROFILE="barnaby-notary"
scripts/package_macos.sh --notarize
```

Alternatively, pass credentials through environment variables:

```bash
export APPLE_ID="APPLE_ID_EMAIL"
export APPLE_TEAM_ID="TEAMID"
export APPLE_APP_SPECIFIC_PASSWORD="APP_SPECIFIC_PASSWORD"
scripts/package_macos.sh --notarize
```

If macOS shows a first-launch dialog mentioning Keka, Safari, Chrome, or another
app, that name comes from the `com.apple.quarantine` metadata attached by the
tool that created, downloaded, or unpacked the local file. It is not the code
signing identity. A valid notarized Barnaby build should pass:

```bash
xcrun stapler validate Barnaby.app
spctl --assess --type execute --verbose Barnaby.app
```

`spctl` should report `accepted` with `source=Notarized Developer ID`.

You need a paid Apple Developer Program membership. To create a Developer ID
Application certificate in the Apple Developer portal:

Steps:

1. Sign in at Apple Developer: https://developer.apple.com/account/
2. Go to Certificates, Identifiers & Profiles.
3. Open Certificates, then click +.
4. Under Software, choose Developer ID Application. Use Developer ID Installer only if you plan to sign a .pkg installer.
5. Create a Certificate Signing Request on your Mac:
 - Open Keychain Access
 - Menu: Keychain Access > Certificate Assistant > Request a Certificate From a Certificate Authority
 - Enter your email and name
 - Select Saved to disk
 - Save the .certSigningRequest file
6. Upload that CSR to Apple when creating the certificate.
7. Download the generated .cer file.
8. Double-click the .cer file to add it to your login keychain.
9. Verify it exists:

```bash
security find-identity -v -p codesigning
```
You should see something like:

```text
Developer ID Application: Your Name or Company (TEAMID)
```

Apple’s official docs:
https://developer.apple.com/help/account/certificates/create-developer-id-certificates

Once that identity exists locally, we can add a signing/notarization script for Barnaby.
