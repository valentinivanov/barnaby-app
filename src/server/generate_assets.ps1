param(
    [Parameter(Mandatory = $true)]
    [string]$Out,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Sources
)

$ErrorActionPreference = "Stop"

function Get-ContentType {
    param([string]$Path)
    switch -Regex ($Path) {
        '\.(html|htm)$' { return "text/html; charset=utf-8" }
        '\.css$' { return "text/css; charset=utf-8" }
        '\.js$' { return "application/javascript; charset=utf-8" }
        '\.json$' { return "application/json; charset=utf-8" }
        '\.svg$' { return "image/svg+xml" }
        '\.png$' { return "image/png" }
        '\.(jpg|jpeg)$' { return "image/jpeg" }
        '\.gif$' { return "image/gif" }
        '\.webp$' { return "image/webp" }
        '\.ico$' { return "image/x-icon" }
        '\.txt$' { return "text/plain; charset=utf-8" }
        default { return "application/octet-stream" }
    }
}

function Get-UrlPath {
    param([string]$Path)
    $normalized = $Path -replace '\\', '/'
    $marker = "src/server/assets/"
    $index = $normalized.IndexOf($marker, [StringComparison]::Ordinal)
    if ($index -ge 0) {
        $relative = $normalized.Substring($index + $marker.Length)
    } else {
        $relative = [IO.Path]::GetFileName($Path)
    }

    if ($relative -eq "index.html" -or $relative -eq "index.htm") {
        return "/"
    }
    if ($relative -match '\.(html|htm)$') {
        return "/" + $relative
    }
    return "/static/" + $relative
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($Out, $false, $utf8NoBom)
$entries = New-Object System.Collections.Generic.List[string]

try {
    $writer.WriteLine('#include "src/server/assets.h"')
    $writer.WriteLine()
    $writer.WriteLine("#include <array>")
    $writer.WriteLine()
    $writer.WriteLine("namespace gitboard::server {")
    $writer.WriteLine("namespace {")
    $writer.WriteLine()

    $assetIndex = 0
    foreach ($src in $Sources) {
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
            continue
        }

        $symbol = "kAsset$assetIndex"
        $path = Get-UrlPath $src
        $type = Get-ContentType $src
        $bytes = [IO.File]::ReadAllBytes($src)

        $writer.WriteLine("constexpr unsigned char $symbol[] = {")
        for ($i = 0; $i -lt $bytes.Length; $i += 16) {
            $end = [Math]::Min($i + 15, $bytes.Length - 1)
            $line = ($bytes[$i..$end] | ForEach-Object { "$_," }) -join ""
            $writer.WriteLine("  $line")
        }
        if ($bytes.Length -eq 0) {
            $writer.WriteLine()
        }
        $writer.WriteLine("};")
        $writer.WriteLine()

        $entries.Add("    {`"$path`", `"$type`", $symbol, sizeof($symbol)},")
        if ($path -eq "/") {
            $entries.Add("    {`"/index.html`", `"$type`", $symbol, sizeof($symbol)},")
        }

        $assetIndex++
    }

    $writer.WriteLine("constexpr std::array<asset, $($entries.Count)> kAssets = {{")
    foreach ($entry in $entries) {
        $writer.WriteLine($entry)
    }
    $writer.WriteLine("}};")
    $writer.WriteLine()
    $writer.WriteLine("}  // namespace")
    $writer.WriteLine()
    $writer.WriteLine("const asset* find_asset(std::string_view path) {")
    $writer.WriteLine("  for (const asset& item : kAssets) {")
    $writer.WriteLine("    if (item.path == path) return &item;")
    $writer.WriteLine("  }")
    $writer.WriteLine("  return nullptr;")
    $writer.WriteLine("}")
    $writer.WriteLine()
    $writer.WriteLine("}  // namespace gitboard::server")
} finally {
    $writer.Dispose()
}
