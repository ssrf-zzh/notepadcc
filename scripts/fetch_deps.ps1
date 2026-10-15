# Fetch and unpack the vendored third-party sources.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1
#
# Dependencies:
#   scintilla / lexilla  - www.scintilla.org release zips
#   pcre2                - codeload.github.com tag zip (note: github.com itself may be
#                          unreachable while codeload/api/raw are fine)
#   sljit                - PCRE2's JIT backend. It is a git submodule and a tag zip does
#                          not contain submodules, so the exact commit PCRE2 pins is
#                          fetched separately and placed at pcre2/deps/sljit.
#
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI.
param(
    [string]$ScintillaVersion = '567',
    [string]$LexillaVersion = '554',
    [string]$Pcre2Tag = 'pcre2-10.49',
    [string]$SljitCommit = 'de0259c7aaf36aa40cba8014f3fad3edde9307f9'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$third = Join-Path $root 'third_party'
$dl = Join-Path $third '_dl'
$stage = Join-Path $third '_stage'

New-Item -ItemType Directory -Force -Path $dl | Out-Null

function Get-Archive {
    param([string]$Url, [string]$FileName)
    $zip = Join-Path $dl $FileName
    if (-not (Test-Path -LiteralPath $zip)) {
        # 必须用 Write-Host：Write-Output 会混进函数的返回值，
        # 调用方拿到的就不只是路径了（踩过一次）。
        Write-Host "[get  ] $Url"
        & curl.exe -sL --fail --max-time 900 -o $zip $Url | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
    }
    return $zip
}

function Expand-Staged {
    param([string]$Zip)
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Expand-Archive -LiteralPath $Zip -DestinationPath $stage -Force
    $entries = @(Get-ChildItem -LiteralPath $stage)
    if ($entries.Count -ne 1 -or -not $entries[0].PSIsContainer) {
        throw "unexpected archive layout in $Zip (expected one top-level directory)"
    }
    return $entries[0].FullName
}

# ---------------------------------------------------------------- scintilla / lexilla
$simple = @(
    @{ Name = 'scintilla'; Url = "https://www.scintilla.org/scintilla$ScintillaVersion.zip";
       File = "scintilla$ScintillaVersion.zip"; Marker = 'include/Scintilla.h' },
    @{ Name = 'lexilla';   Url = "https://www.scintilla.org/lexilla$LexillaVersion.zip";
       File = "lexilla$LexillaVersion.zip";     Marker = 'include/Lexilla.h' }
)

foreach ($job in $simple) {
    $target = Join-Path $third $job.Name
    if (Test-Path -LiteralPath (Join-Path $target $job.Marker)) {
        Write-Output "[skip ] $($job.Name) already present"
        continue
    }
    $zip = Get-Archive -Url $job.Url -FileName $job.File
    Write-Output "[unzip] $($job.File)"
    Expand-Archive -LiteralPath $zip -DestinationPath $third -Force
    if (-not (Test-Path -LiteralPath (Join-Path $target $job.Marker))) {
        throw "unpack failed, missing: $target\$($job.Marker)"
    }
}

# ---------------------------------------------------------------- pcre2
$pcre2 = Join-Path $third 'pcre2'
if (Test-Path -LiteralPath (Join-Path $pcre2 'CMakeLists.txt')) {
    Write-Output "[skip ] pcre2 already present"
}
else {
    $url = "https://codeload.github.com/PCRE2Project/pcre2/zip/refs/tags/$Pcre2Tag"
    $zip = Get-Archive -Url $url -FileName "$Pcre2Tag.zip"
    Write-Output "[unzip] $Pcre2Tag"
    $inner = Expand-Staged -Zip $zip
    if (Test-Path -LiteralPath $pcre2) { Remove-Item -LiteralPath $pcre2 -Recurse -Force }
    Move-Item -LiteralPath $inner -Destination $pcre2 -Force
    Write-Output "[ok   ] pcre2 -> $pcre2"
}

# ---------------------------------------------------------------- sljit (PCRE2 JIT)
$sljit = Join-Path $pcre2 'deps\sljit'
if (Test-Path -LiteralPath (Join-Path $sljit 'sljit_src\sljitLir.c')) {
    Write-Output "[skip ] sljit already present"
}
else {
    $short = $SljitCommit.Substring(0, 12)
    $url = "https://codeload.github.com/zherczeg/sljit/zip/$SljitCommit"
    $zip = Get-Archive -Url $url -FileName "sljit-$short.zip"
    Write-Output "[unzip] sljit @ $short"
    $inner = Expand-Staged -Zip $zip
    New-Item -ItemType Directory -Force -Path $sljit | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $inner) {
        Move-Item -LiteralPath $item.FullName -Destination $sljit -Force
    }
    Write-Output "[ok   ] sljit -> $sljit"
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }

Write-Output 'dependencies ready.'
