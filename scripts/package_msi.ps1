<#
    notepadcc MSI 打包（B10）。

    产出两份包，便于企业和个人走不同的权限路径：
      dist\notepadcc-<版本>-x64-peruser.msi     双模式包，默认按用户装到
                                               %LOCALAPPDATA%\Programs\notepadcc
      dist\notepadcc-<版本>-x64-permachine.msi  装到 %ProgramFiles%\notepadcc

    默认同时重新生成绿色 zip（复用 scripts\package.ps1），便于把两种交付物放在同一轮验证里。

    首次使用先取 WiX：
      powershell -ExecutionPolicy Bypass -File scripts\fetch_wix.ps1

    然后：
      powershell -ExecutionPolicy Bypass -File scripts\package_msi.ps1
      powershell -ExecutionPolicy Bypass -File scripts\package_msi.ps1 -BuildDir build-msvc-final -NoBuild
#>
param(
    [string]$Version = "",
    [string]$BuildDir = "build-msvc",
    [string]$WixPath = "",
    [switch]$NoBuild,
    [switch]$SkipPortable
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
    $match = Select-String -Path (Join-Path $root "CMakeLists.txt") `
                           -Pattern "VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)" |
             Select-Object -First 1
    if (-not $match) {
        throw "从 CMakeLists.txt 里读不到版本号"
    }
    $Version = $match.Matches[0].Groups[1].Value
}

$buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $root $BuildDir
}
$exePath = Join-Path $buildPath "bin\notepadcc.exe"

if (-not $NoBuild) {
    if ($BuildDir -match "msvc") {
        & cmake --build $buildPath --config Release
    } else {
        & cmake --build $buildPath
    }
    if ($LASTEXITCODE -ne 0) {
        throw "构建失败（$BuildDir）"
    }
}
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "找不到 $exePath，请先构建或检查 -BuildDir"
}

if (-not $WixPath) {
    if ($env:WIX_EXE -and (Test-Path -LiteralPath $env:WIX_EXE)) {
        $WixPath = $env:WIX_EXE
    } else {
        $preferredWix = Join-Path $root ".build-tools\wix-5.0.2\PFiles64\WiX Toolset v5.0\bin\wix.exe"
        if (Test-Path -LiteralPath $preferredWix) {
            $WixPath = $preferredWix
        }
        $localWix = Get-ChildItem -LiteralPath (Join-Path $root ".build-tools") `
                                  -Recurse -Filter "wix.exe" -ErrorAction SilentlyContinue |
                    Select-Object -First 1
        if (-not $WixPath -and $localWix) {
            $WixPath = $localWix.FullName
        } elseif (-not $WixPath) {
            $command = Get-Command wix.exe -ErrorAction SilentlyContinue
            if ($command) {
                $WixPath = $command.Source
            }
        }
    }
}
if (-not $WixPath -or -not (Test-Path -LiteralPath $WixPath)) {
    throw "找不到 wix.exe。先运行：powershell -ExecutionPolicy Bypass -File scripts\fetch_wix.ps1"
}

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$stageName = "notepadcc-$Version-msi-stage"
$stage = Join-Path $dist $stageName
if (Test-Path -LiteralPath $stage) {
    $resolved = (Resolve-Path -LiteralPath $stage).Path
    $distResolved = (Resolve-Path -LiteralPath $dist).Path.TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($distResolved, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除 dist 之外的目录：$resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$stageExe = Join-Path $stage "notepadcc.exe"
$stageReadme = Join-Path $stage "安装说明.txt"
Copy-Item -LiteralPath $exePath -Destination $stageExe
$readme = @"
notepadcc $Version（Windows x64 安装版）

启动
  从开始菜单打开 notepadcc；也可以双击文本文件后选择本程序。

配置
  安装版默认把配置写在 %APPDATA%\notepadcc\。
  如果程序目录下存在可写的 config\，或者在程序目录放一个 portable 标记文件，
  程序会改走便携模式（配置落在程序旁边）。MSI 不会预建 config\。

静默安装
  按用户（不需要管理员）：
    msiexec /i notepadcc-$Version-x64-peruser.msi /qn /norestart
  整机（需要管理员）：
    msiexec /i notepadcc-$Version-x64-permachine.msi /qn /norestart
  需要自定义目录时追加 INSTALLFOLDER="D:\Apps\notepadcc"。

卸载
  msiexec /x notepadcc-$Version-x64-peruser.msi /qn /norestart
  msiexec /x notepadcc-$Version-x64-permachine.msi /qn /norestart
  也可以从 Windows 的“应用和功能”里卸载。
"@
[IO.File]::WriteAllText($stageReadme, $readme, (New-Object System.Text.UTF8Encoding($true)))

$wxs = Join-Path $root "packaging\notepadcc.wxs"
$commonArguments = @(
    "build",
    "-arch", "x64",
    "-d", "ProductVersion=$Version",
    "-d", "SourceExe=$stageExe",
    "-d", "SourceReadme=$stageReadme"
)

function Build-Msi([string]$Scope, [string]$PerUser, [string]$UpgradeCode,
                   [string]$Label, [hashtable]$ComponentGuids, [string]$Output) {
    if (Test-Path -LiteralPath $Output) {
        Remove-Item -LiteralPath $Output -Force
    }
    $arguments = $commonArguments + @(
        "-d", "PackageScope=$Scope",
        "-d", "PackageLabel=$Label",
        "-d", "UpgradeCode=$UpgradeCode",
        "-d", "PerUser=$PerUser"
    )
    foreach ($name in $ComponentGuids.Keys) {
        $arguments += @("-d", "$name=$($ComponentGuids[$name])")
    }
    $arguments += @(
        "-o", $Output,
        $wxs
    )
    Write-Host "[msi] 生成 $Scope ..."
    & $WixPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "WiX 生成失败（$Scope，退出码 $LASTEXITCODE）"
    }
    if (-not (Test-Path -LiteralPath $Output)) {
        throw "WiX 报告成功但没找到产物：$Output"
    }
}

$perUserMsi = Join-Path $dist "notepadcc-$Version-x64-peruser.msi"
$perMachineMsi = Join-Path $dist "notepadcc-$Version-x64-permachine.msi"
$perUserGuids = @{
    MainExecutableGuid = "{9F9A07D7-C082-4BDB-8AE4-5396921A7D21}"
    UserGuideGuid = "{93521E01-1FBC-4BA6-9E67-3B36A8C97EC9}"
    ShortcutGuid = "{BB57E140-4071-4E22-94D2-A13DF3F1FD3A}"
}
$perMachineGuids = @{
    MainExecutableGuid = "{3652CF72-B1F1-4288-BC03-B5C968E176F1}"
    UserGuideGuid = "{0E6F9AB5-D8D6-4EA3-BE9F-E42C21EC7B6E}"
    ShortcutGuid = "{B987A9BF-D762-4FB3-8457-3D6088E3388E}"
}
Build-Msi "perUserOrMachine" "1" "BE196C54-6DC1-4EC8-BF1A-7A7CB1EAD38C" "per-user" $perUserGuids $perUserMsi
Build-Msi "perMachine" "0" "C7D59922-9CE9-4857-A44A-70E047F80B50" "per-machine" $perMachineGuids $perMachineMsi

if (-not $SkipPortable) {
    $packageScript = Join-Path $PSScriptRoot "package.ps1"
    & $packageScript -Version $Version -NoBuild -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "绿色 zip 生成失败"
    }
}

Write-Host "[ok] $perUserMsi"
Write-Host "[ok] $perMachineMsi"
if (-not $SkipPortable) {
    Write-Host "[ok] $(Join-Path $dist "notepadcc-$Version-portable-x64.zip")"
}
