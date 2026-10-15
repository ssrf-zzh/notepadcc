# Fetch and unpack the vendored third-party sources (Scintilla + Lexilla).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI.
param(
    [string]$ScintillaVersion = '567',
    [string]$LexillaVersion = '554'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$third = Join-Path $root 'third_party'
$dl = Join-Path $third '_dl'

New-Item -ItemType Directory -Force -Path $dl | Out-Null

$jobs = @(
    @{ Name = 'scintilla'; File = "scintilla$ScintillaVersion.zip"; Dir = 'scintilla' },
    @{ Name = 'lexilla';   File = "lexilla$LexillaVersion.zip";   Dir = 'lexilla' }
)

foreach ($job in $jobs) {
    $target = Join-Path $third $job.Dir
    if (Test-Path -LiteralPath (Join-Path $target 'include')) {
        Write-Output "[skip] $($job.Name) already present"
        continue
    }

    $zip = Join-Path $dl $job.File
    if (-not (Test-Path -LiteralPath $zip)) {
        $url = "https://www.scintilla.org/$($job.File)"
        Write-Output "[get ] $url"
        & curl.exe -sL --fail --max-time 600 -o $zip $url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
    }

    Write-Output "[unzip] $($job.File)"
    Expand-Archive -LiteralPath $zip -DestinationPath $third -Force
}

foreach ($job in $jobs) {
    $target = Join-Path $third $job.Dir
    if (-not (Test-Path -LiteralPath (Join-Path $target 'include'))) {
        throw "unpack failed, missing: $target\include"
    }
    Write-Output "[ok  ] $($job.Name) -> $target"
}

Write-Output 'dependencies ready.'
