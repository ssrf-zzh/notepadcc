<#
    Build the corresponding-source archive required by GPL-3.0.

    The archive contains the current working tree (tracked plus non-ignored
    untracked files), the fetched third-party source trees, LICENSE, build
    scripts and SOURCE_MANIFEST.txt. It is intentionally generated from the
    working tree rather than `git archive HEAD`, so an explicitly marked dirty
    candidate still carries the source that was actually used for its build.

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\package_source.ps1 -Version 1.0.0
      powershell -ExecutionPolicy Bypass -File scripts\package_source.ps1 -Version 1.0.0 -OutputDir dist
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $root "dist" }
$OutputDir = if ([IO.Path]::IsPathRooted($OutputDir)) {
    [IO.Path]::GetFullPath($OutputDir)
} else {
    [IO.Path]::GetFullPath((Join-Path $root $OutputDir))
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$stageRoot = Join-Path $OutputDir ".source-stage-$Version"
$stage = Join-Path $stageRoot "notepadcc-$Version-source"
$output = Join-Path $OutputDir "notepadcc-$Version-source.zip"

function Remove-SafeDirectory([string]$Path, [string]$AllowedParent) {
    $full = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($AllowedParent).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside the source stage: $full"
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $root "LICENSE"))) {
    throw "Missing LICENSE; the source archive must carry the project license."
}
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) { throw "git.exe is required to enumerate the source tree." }

$relativeFiles = @(& git -C $root -c core.quotepath=false ls-files -co --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw "git ls-files failed (exit $LASTEXITCODE)." }

Remove-SafeDirectory $stageRoot $OutputDir
New-Item -ItemType Directory -Path $stage -Force | Out-Null

foreach ($relative in $relativeFiles) {
    if (-not $relative) { continue }
    $source = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $destination = Join-Path $stage $relative
    $parent = Split-Path -Parent $destination
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

$thirdParty = Join-Path $stage "third_party"
New-Item -ItemType Directory -Path $thirdParty -Force | Out-Null
foreach ($name in @("scintilla", "lexilla", "pcre2")) {
    $source = Join-Path $root "third_party\$name"
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Missing third-party source: $source. Run scripts\fetch_deps.ps1 first."
    }
    Copy-Item -LiteralPath $source -Destination $thirdParty -Recurse -Force
}

$commit = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Unable to read the git commit for the source manifest." }
$dirty = @(& git -C $root status --porcelain --untracked-files=no)
$dirtyFlag = if ($dirty.Count -gt 0) { "true" } else { "false" }
$manifest = @"
notepadcc corresponding source
Version: $Version
License: GPL-3.0-only
Copyright: Copyright (C) 2026 notepadcc contributors
Git commit: $commit
Dirty tracked files: $dirtyFlag
Generated: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))

Build and test instructions are in README.md and the repository handoff document.
The archive includes the project source, build/test/release scripts, LICENSE,
NOTICE, third-party license material, and the fetched Scintilla/Lexilla/PCRE2
source trees used by the release build.
"@
[IO.File]::WriteAllText((Join-Path $stage "SOURCE_MANIFEST.txt"), $manifest,
    [Text.UTF8Encoding]::new($false))

if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$stream = [IO.File]::Open($output, [IO.FileMode]::CreateNew)
try {
    $archive = [IO.Compression.ZipArchive]::new(
        $stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File -Force) {
            $relative = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
            $entry = $archive.CreateEntry(
                "notepadcc-$Version-source/$relative",
                [IO.Compression.CompressionLevel]::Optimal)
            $entryStream = $entry.Open()
            try {
                $sourceStream = [IO.File]::OpenRead($file.FullName)
                try { $sourceStream.CopyTo($entryStream) } finally { $sourceStream.Dispose() }
            } finally {
                $entryStream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    $stream.Dispose()
}
$sizeMb = [math]::Round((Get-Item -LiteralPath $output).Length / 1MB, 2)
Remove-SafeDirectory $stageRoot $OutputDir

Write-Host "[ok] $output ($sizeMb MB, GPL-3.0 corresponding source)"
Write-Output $output
