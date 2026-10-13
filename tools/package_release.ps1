# MusicBoard release packaging.
#
# Produces, under dist\:
#   MusicBoard-<version>-windows-x64\        the package directory
#   MusicBoard-<version>-windows-x64.zip     the distributable archive
#
# The package directory contains a SHA256SUMS.txt manifest that the installer
# verifies before touching the machine. This script is the only thing that
# generates that manifest, so it must list every file except itself.
#
# Deliberately ASCII-only source: Windows PowerShell 5.1 reads .ps1 files using
# the ANSI code page unless they carry a BOM, so non-ASCII characters in this
# file would be mangled depending on the machine. Everything user-visible that
# needs Chinese lives in packaging\*.txt (UTF-8), which is copied verbatim.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version 0.2.0

[CmdletBinding()]
param(
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z.-]*$')]
    [string]$Version = '0.3.2',

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$BuildDirectory = '',

    # Skip the install/uninstall smoke test. Only for a quick local iteration;
    # a release candidate must not be packaged with this switch.
    [switch]$SkipSmokeTest,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Windows PowerShell 5.1 has no utf8NoBOM encoding name, and its UTF8 encoding
# WRITES a BOM. A BOM on SHA256SUMS.txt would break the installer's parser (the
# first digest would start with the BOM bytes), so the files below are written
# through .NET with an explicit no-BOM encoder.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Read external program output as UTF-8 so the tool's Chinese text is not
# mangled by the console code page.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory)][string]$LiteralPath,
        [Parameter(Mandatory)][string]$Content
    )
    [System.IO.File]::WriteAllText($LiteralPath, $Content, $utf8NoBom)
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $buildDirectory = Join-Path $repositoryRoot "build\windows-x64\$Configuration"
} else {
    $buildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
}

$distributionRoot = Join-Path $repositoryRoot 'dist'
$packageName = "MusicBoard-$Version-windows-x64"
$packageDirectory = Join-Path $distributionRoot $packageName
$archivePath = Join-Path $distributionRoot "$packageName.zip"

function Assert-FileExists {
    param([Parameter(Mandatory)][string]$LiteralPath)
    if (-not (Test-Path -LiteralPath $LiteralPath -PathType Leaf)) {
        throw "Required file is missing: $LiteralPath"
    }
}

function Get-Sha256Hex {
    param([Parameter(Mandatory)][string]$LiteralPath)
    return (Get-FileHash -LiteralPath $LiteralPath -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Relative path with forward slashes, which is what the manifest format uses.
#
# Not [System.IO.Path]::GetRelativePath: that is .NET Core 2.0+, and Windows
# PowerShell 5.1 still runs on .NET Framework. Every file we hash lives under
# the package root, so trimming the prefix is exact.
function Get-RelativePosixPath {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$File
    )
    $prefix = $Root.TrimEnd('\') + '\'
    if (-not $File.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "File is outside the package root: $File"
    }
    return $File.Substring($prefix.Length).Replace('\', '/')
}

# ---------------------------------------------------------------- sanity checks

Assert-FileExists (Join-Path $buildDirectory 'musicboard.exe')
Assert-FileExists (Join-Path $buildDirectory 'MusicBoardSetup.exe')
Assert-FileExists (Join-Path $buildDirectory 'FullToolsUpdater.exe')

$mainExe = Join-Path $buildDirectory 'musicboard.exe'
$setupExe = Join-Path $buildDirectory 'MusicBoardSetup.exe'

# The application must at least report its version: this catches the case where
# the binary was built with a broken command line or the wrong configuration.
#
# Start-Process -Wait, not `& $mainExe`: the app is a **GUI-subsystem** binary,
# and PowerShell does not wait for those. With `&` this line returned
# immediately, $LASTEXITCODE never got set, and Set-StrictMode turned that into
# "variable not set" -- i.e. packaging broke the moment the app stopped being a
# console binary. Redirecting stdout also only works through Start-Process.
$versionProbeFile = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('musicboard-version-' + [Guid]::NewGuid().ToString('N') + '.txt')
$versionProbe = Start-Process -FilePath $mainExe -ArgumentList '--version' `
    -Wait -PassThru -NoNewWindow -RedirectStandardOutput $versionProbeFile
$reportedVersion = ''
if (Test-Path -LiteralPath $versionProbeFile) {
    $reportedVersion = Get-Content -LiteralPath $versionProbeFile -Raw -Encoding UTF8
    Remove-Item -LiteralPath $versionProbeFile -Force
}
if ($versionProbe.ExitCode -ne 0) {
    throw "musicboard.exe --version failed with exit code $($versionProbe.ExitCode) : $reportedVersion"
}
# Only echo the ASCII version number: the full string is Chinese and would be
# turned into mojibake by whichever console code page this happens to run under.
$versionMatch = [regex]::Match($reportedVersion, '\d+\.\d+(\.\d+)?')
if (-not $versionMatch.Success) {
    throw "musicboard.exe --version did not report a version: $reportedVersion"
}
Write-Host "main executable reports version $($versionMatch.Value)"
if ($versionMatch.Value -ne $Version) {
    throw "Binary version $($versionMatch.Value) does not match requested package version $Version"
}

# ---------------------------------------------------------------- layout

if (Test-Path -LiteralPath $packageDirectory) {
    if (-not $Force) {
        throw "Package directory already exists: $packageDirectory (use -Force to replace it)"
    }
    $resolvedPackage = [System.IO.Path]::GetFullPath($packageDirectory)
    $distPrefix = [System.IO.Path]::GetFullPath($distributionRoot).TrimEnd('\') + '\'
    if (-not $resolvedPackage.StartsWith($distPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing package cleanup outside dist: $resolvedPackage"
    }
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDirectory | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDirectory 'LICENSES') | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination (Join-Path $packageDirectory 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'packaging/README.en.txt') -Destination (Join-Path $packageDirectory 'README.en.txt')

Copy-Item -LiteralPath $mainExe -Destination (Join-Path $packageDirectory 'MusicBoard.exe')
Copy-Item -LiteralPath $setupExe -Destination (Join-Path $packageDirectory 'MusicBoardSetup.exe')
Copy-Item -LiteralPath (Join-Path $buildDirectory 'FullToolsUpdater.exe') -Destination (Join-Path $packageDirectory 'FullToolsUpdater.exe')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'packaging\README.zh-CN.txt') `
    -Destination (Join-Path $packageDirectory 'README.zh-CN.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'packaging\THIRD_PARTY_NOTICES.txt') `
    -Destination (Join-Path $packageDirectory 'THIRD_PARTY_NOTICES.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'packaging\LICENSES\THIRD_PARTY_NOTICES.txt') `
    -Destination (Join-Path $packageDirectory 'LICENSES\THIRD_PARTY_NOTICES.txt')

$mediaTools = Join-Path $repositoryRoot '.cache\media-tools'
Assert-FileExists (Join-Path $mediaTools 'yt-dlp.exe')
Assert-FileExists (Join-Path $mediaTools 'ffmpeg.exe')
Assert-FileExists (Join-Path $mediaTools 'ffprobe.exe')
Assert-FileExists (Join-Path $mediaTools 'licenses\yt-dlp-UNLICENSE.txt')
Assert-FileExists (Join-Path $mediaTools 'licenses\yt-dlp-THIRD_PARTY_LICENSES.txt')
Assert-FileExists (Join-Path $mediaTools 'licenses\FFmpeg-LGPL-2.1.txt')
Assert-FileExists (Join-Path $mediaTools 'sources\MusicBoard-media-corresponding-source.zip')
& python (Join-Path $repositoryRoot 'tools\verify_media_runtime.py') $mediaTools
if ($LASTEXITCODE -ne 0) { throw 'Media runtime provenance verification failed; package not released' }
Copy-Item -LiteralPath $mediaTools -Destination (Join-Path $packageDirectory 'media-tools') -Recurse
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'media-plugins') `
    -Destination (Join-Path $packageDirectory 'media-tools\plugins') -Recurse

# ---------------------------------------------------------------- version contract

# key=value, the same line format the settings file uses. The installer parses
# this and refuses to install a package whose architecture or product differs.
$versionText = @(
    'product=MusicBoard',
    "version=$Version",
    "package_version=$Version",
    'architecture=windows-x64',
    'executable=MusicBoard.exe',
    'complete=1'
) -join "`n"
Write-Utf8NoBom -LiteralPath (Join-Path $packageDirectory 'VERSION.txt') -Content $versionText

# ---------------------------------------------------------------- manifest

$files = Get-ChildItem -LiteralPath $packageDirectory -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName

if ($files.Count -eq 0) {
    throw 'Nothing to package.'
}

$manifestLines = @()
foreach ($file in $files) {
    $relative = Get-RelativePosixPath -Root $packageDirectory -File $file.FullName
    $hash = Get-Sha256Hex -LiteralPath $file.FullName
    $manifestLines += "$hash  $relative"
}

# The manifest itself is deliberately absent: it cannot contain its own hash.
# That is why the installer treats it as the package's root of trust and why
# release notes should carry the package hash out of band.
Write-Utf8NoBom -LiteralPath (Join-Path $packageDirectory 'SHA256SUMS.txt') `
    -Content ($manifestLines -join "`n")

Write-Host "Packaged $($files.Count) file(s) into $packageName"

# ---------------------------------------------------------------- smoke test

if (-not $SkipSmokeTest) {
    $smoke = Join-Path $repositoryRoot 'tools\smoke_packaged_setup.ps1'
    Assert-FileExists $smoke
    Write-Host 'Running the packaged install/uninstall smoke test...'
    & powershell -ExecutionPolicy Bypass -NoProfile -File $smoke -PackageDirectory $packageDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "The packaged setup smoke test failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Host 'WARNING: smoke test skipped. Do not publish this package as a release.'
}

# ---------------------------------------------------------------- archive

if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
Compress-Archive -Path $packageDirectory -DestinationPath $archivePath -CompressionLevel Optimal

$archiveHash = Get-Sha256Hex -LiteralPath $archivePath
$archiveSize = (Get-Item -LiteralPath $archivePath).Length

Write-Host ''
Write-Host 'Package ready.'
Write-Host "  directory : $packageDirectory"
Write-Host "  archive   : $archivePath"
Write-Host ("  size      : {0:N1} KiB" -f ($archiveSize / 1KB))
Write-Host "  sha256    : $archiveHash"
Write-Host ''
Write-Host 'Publish that sha256 alongside the archive: the manifest inside the package'
Write-Host 'cannot verify itself, so the hash has to come from outside.'
