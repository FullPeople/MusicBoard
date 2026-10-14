# Packaged install/uninstall smoke test.
#
# Runs the *packaged* MusicBoardSetup.exe against a throwaway install root and
# then checks the real system state it produced:
#
#   1. install into a temporary install root
#   2. the install root contains current\MusicBoard.exe
#   3. the app starts, reports its version and enumerates an empty library
#   4. the uninstall registry entry exists and names the right version
#   5. a tampered package is REFUSED (this is the integrity boundary)
#   6. uninstall removes the install root, the registry entry and the shortcut
#   7. the user's data outside the install root is untouched
#
# The registry entry is written under the real product subkey on purpose: the
# point is to verify what a user would actually get. It is deleted again by the
# uninstall step, and the test fails loudly if anything is left behind.
#
# ASCII-only source (see package_release.ps1 for why).

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,

    [string]$InstallRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$PackageDirectory = [System.IO.Path]::GetFullPath($PackageDirectory)
if (-not (Test-Path -LiteralPath $PackageDirectory -PathType Container)) {
    throw "Package directory not found: $PackageDirectory"
}

$setupExe = Join-Path $PackageDirectory 'MusicBoardSetup.exe'
if (-not (Test-Path -LiteralPath $setupExe -PathType Leaf)) {
    throw "Packaged setup executable not found: $setupExe"
}

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("MusicBoard-smoke-" + [guid]::NewGuid().ToString('N'))
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $scratch 'Programs\MusicBoard'
}
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)

$registryKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MusicBoard'
$shortcutName = (-join ([char]0x97F3, [char]0x4E50, [char]0x677F)) + '.lnk'
$shortcut = Join-Path $env:APPDATA ('Microsoft\Windows\Start Menu\Programs\' + $shortcutName)
# The smoke test must not overwrite an existing installation's registration.
if ((Test-Path -LiteralPath $registryKey) -or (Test-Path -LiteralPath $shortcut)) {
    throw 'Existing MusicBoard registration or shortcut found; smoke test requires a clean registration.'
}
$scratchPrefix = [System.IO.Path]::GetFullPath($scratch).TrimEnd('\') + '\'
if (-not $InstallRoot.StartsWith($scratchPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Smoke install root must stay inside its unique scratch directory: $InstallRoot"
}
$packageValues = Get-Content -LiteralPath (Join-Path $PackageDirectory 'VERSION.txt') -Raw -Encoding UTF8 | ConvertFrom-StringData
$expectedVersion = $packageValues['version']
if ([string]::IsNullOrWhiteSpace($expectedVersion)) { throw 'Missing package version.' }
$failures = 0
$checks = 0

function Check {
    param(
        [Parameter(Mandatory)][bool]$Condition,
        [Parameter(Mandatory)][string]$Label,
        [string]$Detail = ''
    )
    $script:checks++
    if ($Condition) {
        Write-Host "PASS $Label"
    } else {
        $script:failures++
        if ([string]::IsNullOrWhiteSpace($Detail)) {
            Write-Host "FAIL $Label"
        } else {
            Write-Host "FAIL $Label  [$Detail]"
        }
    }
}

# Run a native executable and return its exit code plus its stdout/stderr.
#
# Both shipped binaries are **GUI-subsystem** programs, and PowerShell does not
# wait for those: `& $exe --version` returns immediately, captures nothing, and
# leaves $LASTEXITCODE unset (which Set-StrictMode then turns into a hard error).
# That is exactly how packaging broke the day the app stopped being a console
# binary. So every invocation goes through Start-Process with explicit files.
function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @()
    )
    $tag = [guid]::NewGuid().ToString('N')
    $outFile = Join-Path $scratch ($tag + '.out')
    $errFile = Join-Path $scratch ($tag + '.err')
    # -ArgumentList takes a single string here: an array is joined with spaces
    # and would split any path that contains one.
    $quoted = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
    $process = Start-Process -FilePath $FilePath -ArgumentList $quoted -Wait -PassThru `
        -NoNewWindow -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $lines = @()
    foreach ($file in @($outFile, $errFile)) {
        if (Test-Path -LiteralPath $file) {
            $lines += (Get-Content -LiteralPath $file -Encoding UTF8)
            Remove-Item -LiteralPath $file -Force
        }
    }
    return @{ Output = ($lines -join "`n"); Code = $process.ExitCode }
}

function Invoke-Setup {
    param([string[]]$ExtraArguments)
    $arguments = @('--quiet', '--no-launch', '--install-root', $InstallRoot) + $ExtraArguments
    return Invoke-Native -FilePath $setupExe -Arguments $arguments
}

try {
    New-Item -ItemType Directory -Path $scratch | Out-Null
    # ---------------------------------------------------------------- install
    $result = Invoke-Setup -ExtraArguments @('--package-root', $PackageDirectory)
    Check ($result.Code -eq 0) 'packaged setup installs the product' $result.Output

    $installedExe = Join-Path $InstallRoot 'current\MusicBoard.exe'
    Check (Test-Path -LiteralPath $installedExe -PathType Leaf) 'installed executable exists'
    Check (Test-Path -LiteralPath (Join-Path $InstallRoot 'current\LICENSES\THIRD_PARTY_NOTICES.txt') -PathType Leaf) `
        'subdirectories are installed too'

    # ---------------------------------------------------------------- registry
    Check (Test-Path -LiteralPath $registryKey) 'uninstall entry was written'
    Check (Test-Path -LiteralPath $shortcut -PathType Leaf) 'actual product start menu shortcut was written'
    if (Test-Path -LiteralPath $registryKey) {
        $properties = Get-ItemProperty -LiteralPath $registryKey
        # The product name is Chinese; spell it out as code points so this file
        # stays ASCII-only. Note the -join: in PowerShell `+` on two [char]
        # values adds their code points instead of concatenating them, which
        # would compare against a number and always fail.
        $expectedName = -join ([char]0x97F3, [char]0x4E50, [char]0x677F)
        Check ($properties.DisplayName -eq $expectedName) `
            'DisplayName is the product name' "$($properties.DisplayName)"
        Check ($properties.DisplayVersion -eq $expectedVersion) `
            'DisplayVersion matches the package' "$($properties.DisplayVersion)"
        Check ($properties.UninstallString -like '*--uninstall*') `
            'UninstallString points at the packaged setup' "$($properties.UninstallString)"
        Check ($properties.NoModify -eq 1 -and $properties.NoRepair -eq 1) `
            'NoModify and NoRepair are set (no dead buttons in Apps)'
    }

    # ---------------------------------------------------------------- runs
    $versionProbe = Invoke-Native -FilePath $installedExe -Arguments @('--version')
    Check ($versionProbe.Code -eq 0) 'installed executable runs' $versionProbe.Output
    Check ($versionProbe.Output -match [regex]::Escape($expectedVersion)) 'installed executable reports the package version' `
        $versionProbe.Output

    # A settings query must answer without ever creating a window.
    $emptyLibrary = Join-Path $scratch 'empty-library'
    New-Item -ItemType Directory -Path $emptyLibrary | Out-Null
    $settingsProbe = Invoke-Native -FilePath $installedExe `
        -Arguments @('--library', $emptyLibrary, '--settings-path')
    Check ($settingsProbe.Code -eq 0) '--settings-path answers without a window' $settingsProbe.Output
    Check ($settingsProbe.Output -match 'settings\.conf') 'settings path looks like a settings file' `
        $settingsProbe.Output

    # A user file outside the install root must survive uninstall.
    $userData = Join-Path $scratch 'my-library\song.mp3'
    New-Item -ItemType Directory -Path (Split-Path -Parent $userData) | Out-Null
    Set-Content -LiteralPath $userData -Value 'user data' -Encoding ascii

    # ---------------------------------------------------------------- tampering
    #
    # Copy the package, change one byte of the main executable and confirm the
    # installer refuses it. Without this check, verification could be a no-op
    # and everything above would still pass.
    $tampered = Join-Path $scratch 'tampered-package'
    Copy-Item -LiteralPath $PackageDirectory -Destination $tampered -Recurse
    $tamperedExe = Join-Path $tampered 'MusicBoard.exe'
    $bytes = [System.IO.File]::ReadAllBytes($tamperedExe)
    $bytes[0] = $bytes[0] -bxor 0xFF
    [System.IO.File]::WriteAllBytes($tamperedExe, $bytes)

    $tamperedRoot = Join-Path $scratch 'Programs\MusicBoard-tampered'
    $tamperedProbe = Invoke-Native -FilePath $setupExe -Arguments @(
        '--quiet', '--no-launch', '--package-root', $tampered, '--install-root', $tamperedRoot)
    $tamperedOutput = $tamperedProbe.Output
    $tamperedCode = $tamperedProbe.Code
    Check ($tamperedCode -ne 0) 'a tampered package is refused' "exit $tamperedCode"
    Check (-not (Test-Path -LiteralPath $tamperedRoot)) `
        'a refused package installs nothing'
    Check (($tamperedOutput -join ' ') -match 'SHA256|MusicBoard\.exe') `
        'the refusal names the file or the check' ($tamperedOutput -join ' ')

    # A file that is not in the manifest must also be refused.
    $extra = Join-Path $tampered 'sneaked-in.dll'
    Set-Content -LiteralPath $extra -Value 'payload' -Encoding ascii
    $unlistedProbe = Invoke-Native -FilePath $setupExe -Arguments @(
        '--quiet', '--no-launch', '--package-root', $tampered, '--install-root', $tamperedRoot)
    Check ($unlistedProbe.Code -ne 0) 'a package with an unlisted file is refused'

    # ---------------------------------------------------------------- uninstall
    $uninstallProbe = Invoke-Native -FilePath $setupExe -Arguments @(
        '--uninstall', '--quiet', '--install-root', $InstallRoot)
    Check ($uninstallProbe.Code -eq 0) 'uninstall succeeds' $uninstallProbe.Output
    Start-Sleep -Milliseconds 500
    Check (-not (Test-Path -LiteralPath $InstallRoot)) 'install root is gone'
    Check (-not (Test-Path -LiteralPath $registryKey)) 'uninstall entry is gone'
    Check (Test-Path -LiteralPath $userData -PathType Leaf) `
        'user data outside the install root is untouched'

    $shortcut = Join-Path $env:APPDATA ('Microsoft\Windows\Start Menu\Programs\' + $shortcutName)
    Check (-not (Test-Path -LiteralPath $shortcut)) 'start menu shortcut is gone'
}
finally {
    # Never leave the machine changed, even when a check failed above.
    if (Test-Path -LiteralPath $registryKey) {
        Remove-Item -LiteralPath $registryKey -Recurse -Force -ErrorAction SilentlyContinue
    }
    $shortcut = Join-Path $env:APPDATA ('Microsoft\Windows\Start Menu\Programs\' + $shortcutName)
    if (Test-Path -LiteralPath $shortcut) {
        Remove-Item -LiteralPath $shortcut -Force -ErrorAction SilentlyContinue
    }
    foreach ($path in @($InstallRoot, (Join-Path $scratch 'Programs'))) {
        $resolvedTarget = [System.IO.Path]::GetFullPath($path)
        if (-not $resolvedTarget.StartsWith($scratchPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing cleanup outside smoke scratch: $resolvedTarget"
        }
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path -LiteralPath $scratch) {
        $resolvedScratch = [System.IO.Path]::GetFullPath($scratch)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolvedScratch.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            [System.IO.Path]::GetFileName($resolvedScratch) -notmatch '^MusicBoard-smoke-[0-9a-f]{32}$') {
            throw "Refusing unexpected scratch cleanup: $resolvedScratch"
        }
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host "---- packaged setup smoke test: $($checks - $failures)/$checks passed, $failures failed ----"
if ($failures -ne 0) {
    exit 1
}
exit 0
