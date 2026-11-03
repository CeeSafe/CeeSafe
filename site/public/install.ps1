# Installs ceesafec, the CeeSafe compiler, from a GitHub release (https://github.com/CeeSafe/CeeSafe; docs/release.md).
#
#   irm https://ceesafe.github.io/CeeSafe/install.ps1 | iex
#
# $env:CEESAFE_VERSION = "v0.1.0"   install that release instead of the latest
# $env:CEESAFE_INSTALL = "<dir>"     install into <dir>\bin instead of %USERPROFILE%\.ceesafe\bin
#
# The archive is checked against the release's SHA256SUMS before anything is installed, and the bin directory is
# added to the user's PATH.
$ErrorActionPreference = "Stop"

$repo = "CeeSafe/CeeSafe"
$version = if ($env:CEESAFE_VERSION) { $env:CEESAFE_VERSION } else { "latest" }
$root = if ($env:CEESAFE_INSTALL) { $env:CEESAFE_INSTALL } else { Join-Path $HOME ".ceesafe" }

$arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
    "X64" { "x64" }
    "Arm64" { "arm64" }
    default { throw "ceesafec install: no prebuilt ceesafec for $_" }
}
$asset = "ceesafec-win-$arch.zip"

# CEESAFE_DOWNLOAD_BASE (a URL of a directory holding the assets) is for tests.
$base = if ($env:CEESAFE_DOWNLOAD_BASE) { $env:CEESAFE_DOWNLOAD_BASE }
    elseif ($version -eq "latest") { "https://github.com/$repo/releases/latest/download" }
    else { "https://github.com/$repo/releases/download/$version" }

$work = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Write-Host "ceesafec install: downloading $asset ($version)"
    Invoke-WebRequest -UseBasicParsing "$base/$asset" -OutFile (Join-Path $work $asset)
    Invoke-WebRequest -UseBasicParsing "$base/SHA256SUMS" -OutFile (Join-Path $work "SHA256SUMS")
    $line = Get-Content (Join-Path $work "SHA256SUMS") | Where-Object { $_ -match "  $([regex]::Escape($asset))$" } | Select-Object -First 1
    if (-not $line) { throw "ceesafec install: SHA256SUMS has no line for $asset" }
    $expected = ($line -split "\s+")[0]
    $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $work $asset)).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "ceesafec install: $asset does not match its SHA-256 in SHA256SUMS; nothing was installed" }

    Expand-Archive (Join-Path $work $asset) -DestinationPath (Join-Path $work "unpacked")
    $bin = Join-Path $root "bin"
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    Copy-Item (Join-Path $work "unpacked\ceesafec.exe") (Join-Path $bin "ceesafec.exe") -Force
    Copy-Item (Join-Path $work "unpacked\COPYING"), (Join-Path $work "unpacked\COPYING.RUNTIME") $root -Force

    $installed = & (Join-Path $bin "ceesafec.exe") --version
    Write-Host "ceesafec install: installed $installed in $bin"
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not (($userPath -split ";") -contains $bin)) {
        [Environment]::SetEnvironmentVariable("Path", (($userPath, $bin) | Where-Object { $_ }) -join ";", "User")
        Write-Host "ceesafec install: added $bin to your PATH; open a new terminal to use ceesafec"
    }
    Write-Host "ceesafec install: ceesafec build needs a C compiler (MSVC, clang or gcc); check, run --interp, fmt, lsp and mcp do not."
}
finally {
    Remove-Item -Recurse -Force $work
}
