# Refresh the TOC field and page-number fields of a .docx using Word (.NET COM).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\update_toc.ps1 -Path <docx>
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI.
param(
    [Parameter(Mandatory = $true)][string]$Path
)

$ErrorActionPreference = 'Stop'
$full = [System.IO.Path]::GetFullPath($Path)
if (-not (Test-Path -LiteralPath $full)) { throw "file not found: $full" }

$word = $null
$doc = $null
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0

    $doc = $word.Documents.Open($full, $false, $false)
    Write-Output ("opened. paragraphs = " + $doc.Paragraphs.Count)

    $doc.Fields.Update()
    $tocCount = $doc.TablesOfContents.Count
    Write-Output ("tables of contents = " + $tocCount)
    for ($i = 1; $i -le $tocCount; $i++) {
        $doc.TablesOfContents.Item($i).Update()
    }
    $doc.Repaginate()
    Write-Output ("pages = " + $doc.ComputeStatistics(2))
    $doc.Save()
    Write-Output "saved"
}
catch {
    Write-Output ("FAILED: " + $_.Exception.Message)
    exit 1
}
finally {
    if ($doc) { try { $doc.Close(0) } catch {} }
    if ($word) { try { $word.Quit() } catch {} }
    [System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) | Out-Null
    Start-Sleep -Milliseconds 500
}
