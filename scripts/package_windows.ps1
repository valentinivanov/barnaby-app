param(
  [string]$OutputName = ""
)

$ErrorActionPreference = "Stop"

function Find-VsWhere {
  $command = Get-Command vswhere.exe -ErrorAction SilentlyContinue
  if ($command) {
    return $command.Source
  }

  $programFilesX86 = ${env:ProgramFiles(x86)}
  if ($programFilesX86) {
    $candidate = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $candidate) {
      return $candidate
    }
  }

  return $null
}

function Import-BatchEnvironment {
  param(
    [Parameter(Mandatory = $true)][string]$BatchFile,
    [string[]]$Arguments = @()
  )

  $argumentLine = $Arguments -join " "
  $commandLine = "`"$BatchFile`" $argumentLine >NUL && set"
  & $env:ComSpec /s /c $commandLine | ForEach-Object {
    $separator = $_.IndexOf("=")
    if ($separator -le 0) {
      return
    }
    $name = $_.Substring(0, $separator)
    $value = $_.Substring($separator + 1)
    [Environment]::SetEnvironmentVariable($name, $value, "Process")
  }
}

function Initialize-VisualStudioEnvironment {
  $vswhere = Find-VsWhere
  if (-not $vswhere) {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
      return $null
    }
    throw "Could not find vswhere.exe. Install Visual Studio Build Tools or add vswhere.exe to PATH."
  }

  $installationPath = & $vswhere `
      -latest `
      -products * `
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
      -property installationPath
  if (-not $installationPath) {
    throw "Could not find Visual Studio Build Tools with the VC toolchain."
  }

  $vsDevCmd = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
  if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "Could not find VsDevCmd.bat under '$installationPath'."
  }

  Import-BatchEnvironment `
      -BatchFile $vsDevCmd `
      -Arguments @("-no_logo", "-arch=x64", "-host_arch=x64")

  return $installationPath
}

function Add-BazelRepoEnv {
  param(
    [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Args,
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Value
  )

  if ($Value) {
    $Args.Add("--repo_env=$Name=$Value")
  }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
  $versionFile = Join-Path $repoRoot "VERSION"
  if ($env:BARNABY_VERSION) {
    $packageVersion = $env:BARNABY_VERSION.Trim()
  } elseif (Test-Path -LiteralPath $versionFile) {
    $packageVersion = (Get-Content -Raw -LiteralPath $versionFile).Trim()
  } else {
    $packageVersion = "0.4.2"
  }
  if (-not $OutputName) {
    $OutputName = "Barnaby-$packageVersion-windows-portable.zip"
  }

  $visualStudioRoot = Initialize-VisualStudioEnvironment

  if (-not $env:VCPKG_ROOT) {
    $vcpkg = Get-Command vcpkg.exe -ErrorAction SilentlyContinue
    if ($vcpkg) {
      $env:VCPKG_ROOT = Split-Path -Parent $vcpkg.Source
    }
  }

  $bazelStartupArgs = [System.Collections.Generic.List[string]]::new()
  $bazelBuildArgs = [System.Collections.Generic.List[string]]::new()
  if ($env:BAZEL_SH) {
    $bazelStartupArgs.Add("--shell_executable=$env:BAZEL_SH")
  }

  $bazelVs = if ($env:BAZEL_VS) { $env:BAZEL_VS } else { $visualStudioRoot }
  $bazelVc = if ($env:BAZEL_VC) {
    $env:BAZEL_VC
  } elseif ($env:VCINSTALLDIR) {
    $env:VCINSTALLDIR.TrimEnd("\")
  } elseif ($visualStudioRoot) {
    Join-Path $visualStudioRoot "VC"
  } else {
    $null
  }
  $bazelVcFullVersion = if ($env:BAZEL_VC_FULL_VERSION) {
    $env:BAZEL_VC_FULL_VERSION
  } else {
    $env:VCToolsVersion
  }
  $bazelWinSdkFullVersion = if ($env:BAZEL_WINSDK_FULL_VERSION) {
    $env:BAZEL_WINSDK_FULL_VERSION
  } elseif ($env:WindowsSDKVersion) {
    $env:WindowsSDKVersion.TrimEnd("\")
  } else {
    $null
  }

  Add-BazelRepoEnv -Args $bazelBuildArgs -Name "VCPKG_ROOT" -Value $env:VCPKG_ROOT
  Add-BazelRepoEnv -Args $bazelBuildArgs -Name "BAZEL_VS" -Value $bazelVs
  Add-BazelRepoEnv -Args $bazelBuildArgs -Name "BAZEL_VC" -Value $bazelVc
  Add-BazelRepoEnv -Args $bazelBuildArgs -Name "BAZEL_VC_FULL_VERSION" -Value $bazelVcFullVersion
  Add-BazelRepoEnv -Args $bazelBuildArgs -Name "BAZEL_WINSDK_FULL_VERSION" -Value $bazelWinSdkFullVersion

  & bazel @bazelStartupArgs build @bazelBuildArgs //:barnaby_windows_portable
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }

  $dist = Join-Path $repoRoot "dist"
  New-Item -ItemType Directory -Force -Path $dist | Out-Null
  Copy-Item -LiteralPath (Join-Path $repoRoot "bazel-bin\dist\barnaby_windows_portable.zip") `
            -Destination (Join-Path $dist $OutputName) `
            -Force
  Write-Host "Wrote $(Join-Path $dist $OutputName)"
} finally {
  Pop-Location
}
