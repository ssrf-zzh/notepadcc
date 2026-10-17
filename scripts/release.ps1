<#
    Build the complete notepadcc release bundle (B13).

    Artifacts produced in dist\release-<version>:
      notepadcc.exe, portable zip, per-user / per-machine MSI,
      notepadcc-<version>-source.zip (GPL-3.0 corresponding source),
      SHA256SUMS, release-manifest.json, RELEASE_NOTES.txt,
      LICENSE, NOTICE, sbom.cdx.json, licenses\,
      index.html (static download page)

    Production usage with a certificate in CurrentUser\My:
      powershell -ExecutionPolicy Bypass -File scripts\release.ps1 `
        -Version 1.0.0 -BuildDir build-msvc-final `
        -SigningThumbprint <sha1-thumbprint>

    Local acceptance without a certificate must be explicit:
      powershell -ExecutionPolicy Bypass -File scripts\release.ps1 `
        -Version 1.0.0 -BuildDir build-msvc-final -AllowUnsigned

    The unsigned result is marked as a candidate in the manifest and download
    page. It must not be published as the official release.
#>
param(
    [string]$Version = "",
    [string]$BuildDir = "build-msvc-final",
    [ValidateSet("stable", "beta")][string]$Channel = "stable",
    [string]$ReleaseNotes = "",
    [string]$PageTemplate = "",
    [string]$SigningThumbprint = "",
    [string]$PfxPath = "",
    [string]$PfxPassword = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$AllowUnsigned,
    [switch]$RequireSignature,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Get-CMakeVersion {
    $match = Select-String -Path (Join-Path $root "CMakeLists.txt") `
                           -Pattern "VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)" |
             Select-Object -First 1
    if (-not $match) { throw "Cannot read the project version from CMakeLists.txt" }
    return $match.Matches[0].Groups[1].Value
}

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $kitRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitRoot) {
        $candidate = Get-ChildItem -LiteralPath $kitRoot -Directory |
                     Sort-Object Name -Descending |
                     ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
                     Where-Object { Test-Path -LiteralPath $_ } |
                     Select-Object -First 1
        if ($candidate) { return $candidate }
    }
    throw "signtool.exe was not found. Install the Windows SDK or put signtool on PATH."
}

function Invoke-SignTool([string]$SignTool, [string]$Path) {
    $arguments = @("sign", "/fd", "SHA256", "/tr", $TimestampUrl, "/td", "SHA256")
    if ($SigningThumbprint) {
        $arguments += @("/sha1", $SigningThumbprint)
    } else {
        $arguments += @("/f", $PfxPath, "/a")
        if ($PfxPassword) { $arguments += @("/p", $PfxPassword) }
    }
    $arguments += $Path
    Write-Host "[sign] $Path"
    & $SignTool @arguments
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed for $Path (exit $LASTEXITCODE)" }
}

function Test-SignTool([string]$SignTool, [string]$Path) {
    & $SignTool verify /pa /all $Path | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $Path (exit $LASTEXITCODE)" }
}

function Remove-SafeDirectory([string]$Path, [string]$AllowedParent) {
    $full = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($AllowedParent).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside dist: $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}

function Copy-Checked([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { throw "Missing required file: $Source" }
    $parent = Split-Path -Parent $Destination
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Invoke-Python([string[]]$Arguments) {
    $oldEncoding = $env:PYTHONIOENCODING
    $env:PYTHONIOENCODING = "utf-8"
    try {
        & python @Arguments
        if ($LASTEXITCODE -ne 0) { throw "python failed (exit $LASTEXITCODE)" }
    } finally {
        $env:PYTHONIOENCODING = $oldEncoding
    }
}

if (-not $Version) { $Version = Get-CMakeVersion }
$cmakeVersion = Get-CMakeVersion
if ($Version -ne $cmakeVersion) {
    throw "Version $Version does not match CMakeLists.txt ($cmakeVersion)"
}
$headerVersion = Select-String -Path (Join-Path $root "src\core\Version.h") `
                               -Pattern 'kAppVersionAscii\s*=\s*"([^"]+)"' |
                 Select-Object -First 1
if (-not $headerVersion -or $headerVersion.Matches[0].Groups[1].Value -ne $Version) {
    throw "src\core\Version.h does not match version $Version"
}

$signingRequested = [bool]($SigningThumbprint -or $PfxPath)
if ($SigningThumbprint -and $PfxPath) {
    throw "Use either -SigningThumbprint or -PfxPath, not both."
}
if (-not $signingRequested -and ($RequireSignature -or -not $AllowUnsigned)) {
    throw "No signing certificate supplied. Use -AllowUnsigned only for a candidate that will not be published."
}
if ($signingRequested -and $SkipPackage) {
    throw "Signing requires packaging after signing so the zip contains the signed executable."
}

$buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$exePath = Join-Path $buildPath "bin\notepadcc.exe"
if (-not (Test-Path -LiteralPath $exePath)) { throw "Missing $exePath; build the MSVC release first." }

$signTool = $null
if ($signingRequested) {
    $signTool = Find-SignTool
    Invoke-SignTool $signTool $exePath
    Test-SignTool $signTool $exePath
}

if (-not $SkipPackage) {
    $packageScript = Join-Path $PSScriptRoot "package_msi.ps1"
    & $packageScript -Version $Version -BuildDir $BuildDir -NoBuild
    if ($LASTEXITCODE -ne 0) { throw "Package generation failed." }
}

$dist = Join-Path $root "dist"
$releaseDir = Join-Path $dist "release-$Version"
Remove-SafeDirectory $releaseDir $dist
New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null

$portableZip = Join-Path $dist "notepadcc-$Version-portable-x64.zip"
$perUserMsi = Join-Path $dist "notepadcc-$Version-x64-peruser.msi"
$perMachineMsi = Join-Path $dist "notepadcc-$Version-x64-permachine.msi"

if ($signTool) {
    Invoke-SignTool $signTool $perUserMsi
    Invoke-SignTool $signTool $perMachineMsi
    Test-SignTool $signTool $perUserMsi
    Test-SignTool $signTool $perMachineMsi
}

Copy-Checked $exePath (Join-Path $releaseDir "notepadcc.exe")
Copy-Checked $portableZip (Join-Path $releaseDir (Split-Path -Leaf $portableZip))
Copy-Checked $perUserMsi (Join-Path $releaseDir (Split-Path -Leaf $perUserMsi))
Copy-Checked $perMachineMsi (Join-Path $releaseDir (Split-Path -Leaf $perMachineMsi))
Copy-Checked (Join-Path $root "LICENSE") (Join-Path $releaseDir "LICENSE")
Copy-Checked (Join-Path $root "NOTICE") (Join-Path $releaseDir "NOTICE")

$sourceScript = Join-Path $PSScriptRoot "package_source.ps1"
$sourceZip = Join-Path $dist "notepadcc-$Version-source.zip"
& $sourceScript -Version $Version -OutputDir $dist
if ($LASTEXITCODE -ne 0) { throw "Corresponding source archive generation failed." }
Copy-Checked $sourceZip (Join-Path $releaseDir (Split-Path -Leaf $sourceZip))

$licenses = Join-Path $releaseDir "licenses"
Copy-Checked (Join-Path $root "third_party\scintilla\License.txt") (Join-Path $licenses "Scintilla-License.txt")
Copy-Checked (Join-Path $root "third_party\lexilla\License.txt") (Join-Path $licenses "Lexilla-License.txt")
Copy-Checked (Join-Path $root "third_party\pcre2\LICENCE.md") (Join-Path $licenses "PCRE2-LICENCE.md")
Copy-Checked (Join-Path $root "third_party\pcre2\deps\sljit\LICENSE") (Join-Path $licenses "SLJIT-LICENSE.txt")
Copy-Checked (Join-Path $root "licenses\MSVC-RUNTIME-NOTICE.txt") (Join-Path $licenses "MSVC-RUNTIME-NOTICE.txt")

if (-not $ReleaseNotes) { $ReleaseNotes = Join-Path $root "docs\release-notes\$Version.txt" }
if (-not $PageTemplate) { $PageTemplate = Join-Path $root "packaging\download-page-template.html" }
if (-not (Test-Path -LiteralPath $ReleaseNotes)) { throw "Missing release notes: $ReleaseNotes" }
if (-not (Test-Path -LiteralPath $PageTemplate)) { throw "Missing page template: $PageTemplate" }
Copy-Checked $ReleaseNotes (Join-Path $releaseDir "RELEASE_NOTES.txt")

$commit = "unknown"
$dirtyTracked = $false
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git) {
    $commit = (& git -C $root rev-parse HEAD).Trim()
    $dirty = @(& git -C $root status --porcelain --untracked-files=no)
    $dirtyTracked = $dirty.Count -gt 0
    if ($dirtyTracked) {
        Write-Warning "Tracked files are modified. The manifest will record dirtyTracked=true."
    }
}

$compilerVersion = "unknown"
$compilerFile = Get-ChildItem -LiteralPath (Join-Path $buildPath "CMakeFiles") `
                              -Recurse -Filter "CMakeCXXCompiler.cmake" -ErrorAction SilentlyContinue |
                Select-Object -First 1
if ($compilerFile) {
    $match = Select-String -Path $compilerFile.FullName -Pattern 'CMAKE_CXX_COMPILER_VERSION\s+"([^"]+)"' |
             Select-Object -First 1
    if ($match) { $compilerVersion = $match.Matches[0].Groups[1].Value }
}

$artifacts = @(
    (Join-Path $releaseDir "notepadcc.exe"),
    (Join-Path $releaseDir (Split-Path -Leaf $portableZip)),
    (Join-Path $releaseDir (Split-Path -Leaf $perUserMsi)),
    (Join-Path $releaseDir (Split-Path -Leaf $perMachineMsi)),
    (Join-Path $releaseDir (Split-Path -Leaf $sourceZip))
)
$sbomArguments = @(
    (Join-Path $PSScriptRoot "generate_sbom.py"),
    "--version", $Version,
    "--output", (Join-Path $releaseDir "sbom.cdx.json"),
    "--commit", $commit,
    "--channel", $Channel,
    "--toolchain-version", $compilerVersion,
    "--repo-root", $root
)
foreach ($artifact in $artifacts) { $sbomArguments += @("--artifact", $artifact) }
Invoke-Python $sbomArguments

$signingStatus = if ($signTool) { "signed" } else { "unsigned" }
$signingMethod = if ($SigningThumbprint) { "signtool-certificate-store" } elseif ($PfxPath) { "signtool-pfx" } else { "none" }
$releaseDate = Get-Date -Format "yyyy-MM-dd"
$renderArguments = @(
    (Join-Path $PSScriptRoot "render_release.py"),
    "--release-dir", $releaseDir,
    "--version", $Version,
    "--channel", $Channel,
    "--release-date", $releaseDate,
    "--commit", $commit,
    "--notes-source", $ReleaseNotes,
    "--page-template", $PageTemplate,
    "--signing-status", $signingStatus,
    "--signing-method", $signingMethod
)
if ($signTool) { $renderArguments += @("--timestamp-url", $TimestampUrl) }
if ($dirtyTracked) { $renderArguments += "--dirty-tracked" }
Invoke-Python $renderArguments

$sumsPath = Join-Path $releaseDir "SHA256SUMS"
foreach ($line in Get-Content -LiteralPath $sumsPath -Encoding ASCII) {
    if (-not $line.Trim()) { continue }
    if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw "Bad SHA256SUMS line: $line" }
    $expected = $matches[1].ToLowerInvariant()
    $file = Join-Path $releaseDir $matches[2]
    if (-not (Test-Path -LiteralPath $file)) { throw "SHA256SUMS references a missing file: $file" }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "SHA-256 mismatch for $file" }
}

Write-Host "[ok] Release directory: $releaseDir"
Write-Host "[ok] SHA-256: all entries verified"
Write-Host "[ok] SBOM: CycloneDX 1.5"
Write-Host "[ok] Signing status: $signingStatus"
if ($signingStatus -eq "unsigned") {
    Write-Warning "This is an unsigned candidate. Do not publish it as the official release."
}
