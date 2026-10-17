<#
    下载并解出固定版本的 WiX CLI（不需要管理员，也不安装到系统）。

    固定用 WiX 5.0.2：v6 起引入 OSMF，v7 起在命令行强制接受其维护费 EULA，
    不适合作为这个商业项目的默认构建依赖；v5 没有这层收费门槛。

    为什么单独做这一步：WiX 的官方 CLI MSI 可以做“管理安装”（msiexec /a），
    把工具解到项目下的 .build-tools\；这样构建机不需要改系统 PATH，也不会因为
    不同机器上装着别的 WiX 版本而生成不同的包。

    用法：
      powershell -ExecutionPolicy Bypass -File scripts\fetch_wix.ps1
      powershell -ExecutionPolicy Bypass -File scripts\fetch_wix.ps1 -Force

    输出：成功后把 wix.exe 的绝对路径写到标准输出，供 package_msi.ps1 捕获。
#>
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$version = "5.0.2"
$downloadUrl = "https://github.com/wixtoolset/wix/releases/download/v$version/wix-cli-x64.msi"
$expectedSha256 = "097383B2773BDD3D76A6A2CD3F4DE6357BAE59172E1B6FA5AD87CE062074E8D0"

$root = Split-Path -Parent $PSScriptRoot
$toolsRoot = Join-Path $root ".build-tools"
$cache = Join-Path $toolsRoot "wix-$version"

function Remove-SafeDirectory([string]$Path, [string]$AllowedParent) {
    $full = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($AllowedParent).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除构建工具目录之外的路径：$full"
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

$existing = Get-ChildItem -LiteralPath $cache -Recurse -Filter "wix.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1
if ($existing -and -not $Force) {
    Write-Output $existing.FullName
    exit 0
}

New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
if ($Force) {
    Remove-SafeDirectory $cache $toolsRoot
}
New-Item -ItemType Directory -Path $cache -Force | Out-Null

$msi = Join-Path $env:TEMP "wix-cli-x64-$version.msi"
$needDownload = -not (Test-Path -LiteralPath $msi)
if (-not $needDownload) {
    $actual = (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash
    $needDownload = $actual -ne $expectedSha256
}
if ($needDownload) {
    Write-Host "[wix] 下载 WiX CLI $version ..."
    if (Test-Path -LiteralPath $msi) {
        Remove-Item -LiteralPath $msi -Force
    }
    Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -OutFile $msi
}

$actualSha256 = (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash
if ($actualSha256 -ne $expectedSha256) {
    throw "WiX CLI 下载文件的 SHA-256 不匹配：$actualSha256"
}

$log = Join-Path $env:TEMP "notepadcc-wix-admin-$version.log"
$arguments = @(
    "/a",
    "`"$msi`"",
    "/qn",
    "TARGETDIR=`"$cache`"",
    "/L*v",
    "`"$log`""
)
Write-Host "[wix] 解出到 $cache ..."
$process = Start-Process -FilePath msiexec.exe -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
if ($process.ExitCode -ne 0) {
    throw "WiX 管理安装失败，退出码 $($process.ExitCode)，日志：$log"
}

$wix = Get-ChildItem -LiteralPath $cache -Recurse -Filter "wix.exe" -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $wix) {
    throw "WiX 解出完成，但找不到 wix.exe；日志：$log"
}

Write-Host "[ok] $( & $wix.FullName --version )"
Write-Output $wix.FullName
