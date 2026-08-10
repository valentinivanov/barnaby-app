def _has_constraint(ctx, attr_name):
    return ctx.target_platform_has_constraint(
        getattr(ctx.attr, attr_name)[platform_common.ConstraintValueInfo],
    )

def _dist_name(ctx):
    if _has_constraint(ctx, "_macos"):
        os_name = "darwin"
    elif _has_constraint(ctx, "_linux"):
        os_name = "linux"
    elif _has_constraint(ctx, "_windows"):
        os_name = "windows"
    else:
        os_name = "unknown"

    if _has_constraint(ctx, "_x86_64"):
        cpu_name = "x86_64"
    elif _has_constraint(ctx, "_arm64") or _has_constraint(ctx, "_aarch64"):
        cpu_name = "arm64"
    else:
        cpu_name = ctx.var.get("TARGET_CPU", "unknown")

    return "{}_{}_{}".format(
        os_name,
        cpu_name,
        ctx.var.get("COMPILATION_MODE", "fastbuild"),
    )

def _gitboard_dist_impl(ctx):
    config_name = _dist_name(ctx)
    out = ctx.actions.declare_directory("bin/{}".format(config_name))
    gitboard = ctx.executable.gitboard
    server = ctx.executable.server
    statuses = ctx.file.statuses
    restart_script = ctx.file.restart_script

    if _has_constraint(ctx, "_windows"):
        command = """
@echo off
setlocal
set "out=%~1"
set "gitboard=%~2"
set "server=%~3"
set "statuses=%~4"
set "restart_script=%~5"
if exist "%out%" rmdir /S /Q "%out%"
mkdir "%out%\\config"
copy /Y "%gitboard%" "%out%\\gitboard.exe" >NUL
copy /Y "%server%" "%out%\\gitboard-server.exe" >NUL
copy /Y "%statuses%" "%out%\\config\\statuses.json" >NUL
copy /Y "%restart_script%" "%out%\\restart_barnaby.sh" >NUL
"""
    else:
        command = """
set -euo pipefail
out="$1"
gitboard="$2"
server="$3"
statuses="$4"
restart_script="$5"
rm -rf "$out"
mkdir -p "$out/config"
cp "$gitboard" "$out/gitboard"
cp "$server" "$out/gitboard-server"
cp "$statuses" "$out/config/statuses.json"
cp "$restart_script" "$out/restart_barnaby.sh"
chmod +x "$out/gitboard" "$out/gitboard-server" "$out/restart_barnaby.sh"
"""

    ctx.actions.run_shell(
        inputs = [gitboard, server, statuses, restart_script],
        outputs = [out],
        command = command,
        arguments = [
            out.path,
            gitboard.path,
            server.path,
            statuses.path,
            restart_script.path,
        ],
        mnemonic = "GitBoardDist",
        progress_message = "Packaging GitBoard distribution {}".format(config_name),
    )

    return [DefaultInfo(files = depset([out]))]

gitboard_dist = rule(
    implementation = _gitboard_dist_impl,
    attrs = {
        "gitboard": attr.label(
            executable = True,
            cfg = "target",
            allow_files = True,
            mandatory = True,
        ),
        "server": attr.label(
            executable = True,
            cfg = "target",
            allow_files = True,
            mandatory = True,
        ),
        "statuses": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "restart_script": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "_aarch64": attr.label(
            default = Label("@platforms//cpu:aarch64"),
            providers = [platform_common.ConstraintValueInfo],
        ),
        "_arm64": attr.label(
            default = Label("@platforms//cpu:arm64"),
            providers = [platform_common.ConstraintValueInfo],
        ),
        "_linux": attr.label(
            default = Label("@platforms//os:linux"),
            providers = [platform_common.ConstraintValueInfo],
        ),
        "_macos": attr.label(
            default = Label("@platforms//os:macos"),
            providers = [platform_common.ConstraintValueInfo],
        ),
        "_windows": attr.label(
            default = Label("@platforms//os:windows"),
            providers = [platform_common.ConstraintValueInfo],
        ),
        "_x86_64": attr.label(
            default = Label("@platforms//cpu:x86_64"),
            providers = [platform_common.ConstraintValueInfo],
        ),
    },
)

def _barnaby_windows_portable_zip_impl(ctx):
    out = ctx.actions.declare_file("dist/{}.zip".format(ctx.label.name))
    script = ctx.actions.declare_file("{}_package.ps1".format(ctx.label.name))
    app = ctx.executable.app
    gitboard = ctx.executable.gitboard
    server = ctx.executable.server
    statuses = ctx.file.statuses
    cef_runtime = ctx.files.cef_runtime
    curl_runtime = ctx.files.curl_runtime
    runtime_files = cef_runtime + curl_runtime

    ctx.actions.write(
        output = script,
        content = """
param(
  [Parameter(Mandatory = $true)][string]$outArg,
  [Parameter(Mandatory = $true)][string]$appArg,
  [Parameter(Mandatory = $true)][string]$gitboardArg,
  [Parameter(Mandatory = $true)][string]$serverArg,
  [Parameter(Mandatory = $true)][string]$statusesArg,
  [Parameter(ValueFromRemainingArguments = $true)][string[]]$runtimeArgs
)
$ErrorActionPreference = "Stop"
$out = [IO.Path]::GetFullPath($outArg)
$app = [IO.Path]::GetFullPath($appArg)
$gitboard = [IO.Path]::GetFullPath($gitboardArg)
$server = [IO.Path]::GetFullPath($serverArg)
$statuses = [IO.Path]::GetFullPath($statusesArg)
$runtimeFiles = $runtimeArgs | ForEach-Object { [IO.Path]::GetFullPath($_) }
$staging = Join-Path ([IO.Path]::GetDirectoryName($out)) ([IO.Path]::GetFileNameWithoutExtension($out))
if (Test-Path -LiteralPath $staging) {
  Remove-Item -LiteralPath $staging -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $staging "config") | Out-Null
Copy-Item -LiteralPath $app -Destination (Join-Path $staging "Barnaby.exe") -Force
Copy-Item -LiteralPath $gitboard -Destination (Join-Path $staging "gitboard.exe") -Force
Copy-Item -LiteralPath $server -Destination (Join-Path $staging "gitboard-server.exe") -Force
Copy-Item -LiteralPath $statuses -Destination (Join-Path $staging "config/statuses.json") -Force
foreach ($file in $runtimeFiles) {
  if ($file -like "*\\locales\\*" -or $file -like "*/locales/*") {
    $destDir = Join-Path $staging "locales"
  } else {
    $destDir = $staging
  }
  New-Item -ItemType Directory -Force -Path $destDir | Out-Null
  Copy-Item -LiteralPath $file -Destination (Join-Path $destDir ([IO.Path]::GetFileName($file))) -Force
}
if (Test-Path -LiteralPath $out) {
  Remove-Item -LiteralPath $out -Force
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $staging,
  $out,
  [System.IO.Compression.CompressionLevel]::Fastest,
  $false)
""",
    )

    ctx.actions.run(
        inputs = [script, app, gitboard, server, statuses] + runtime_files,
        outputs = [out],
        executable = "powershell.exe",
        arguments = [
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            script.path,
            out.path,
            app.path,
            gitboard.path,
            server.path,
            statuses.path,
        ] + [file.path for file in runtime_files],
        mnemonic = "BarnabyWindowsPortableZip",
        progress_message = "Packaging Barnaby portable Windows zip",
    )

    return [DefaultInfo(files = depset([out]))]

barnaby_windows_portable_zip = rule(
    implementation = _barnaby_windows_portable_zip_impl,
    attrs = {
        "app": attr.label(
            executable = True,
            cfg = "target",
            allow_files = True,
            mandatory = True,
        ),
        "gitboard": attr.label(
            executable = True,
            cfg = "target",
            allow_files = True,
            mandatory = True,
        ),
        "server": attr.label(
            executable = True,
            cfg = "target",
            allow_files = True,
            mandatory = True,
        ),
        "statuses": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "cef_runtime": attr.label(
            allow_files = True,
            default = Label("@local_cef//:runtime_files"),
        ),
        "curl_runtime": attr.label(
            allow_files = True,
            default = Label("@local_libcurl//:runtime_dlls"),
        ),
    },
)
