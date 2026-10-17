<#
    notepadcc 绿色发行打包（设计文档 2.15 的"绿色免安装"）。

    产出 dist\notepadcc-<版本>-portable-x64\ 目录与同名 zip：
      notepadcc.exe      程序本体（静态链接，不依赖 VC++ 可再发行组件）
      portable           便携模式标记：放在程序旁边就不再读写 %APPDATA%
      说明.txt           用法与"绿色"的边界说明

    用法：
      powershell -ExecutionPolicy Bypass -File scripts\package.ps1
      powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Toolchain mingw -NoBuild
#>
param(
    [string]$Version = "",
    [ValidateSet("msvc", "mingw")][string]$Toolchain = "msvc",
    [string]$BuildDir = "",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $cmake = Join-Path $root "CMakeLists.txt"
    $match = Select-String -Path $cmake -Pattern 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
             Select-Object -First 1
    if (-not $match) { throw "从 CMakeLists.txt 里读不到版本号" }
    $Version = $match.Matches[0].Groups[1].Value
}

$buildDir = if ($BuildDir) {
    if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
} elseif ($Toolchain -eq "msvc") {
    Join-Path $root "build-msvc"
} else {
    Join-Path $root "build"
}
$exePath = Join-Path $buildDir "bin\notepadcc.exe"

if (-not $NoBuild) {
    if ($Toolchain -eq "msvc") {
        cmake --build $buildDir --config Release
    } else {
        cmake --build $buildDir
    }
    if ($LASTEXITCODE -ne 0) { throw "构建失败" }
}
if (-not (Test-Path $exePath)) { throw "找不到 $exePath，请先构建" }

$distDir = Join-Path $root "dist"
$stageName = "notepadcc-$Version-portable-x64"
$stage = Join-Path $distDir $stageName
$zipPath = Join-Path $distDir "$stageName.zip"

# 只清自己上一次的产物：路径先解析出来、确认落在 dist 下面再删
if (Test-Path $stage) {
    $resolved = (Resolve-Path -LiteralPath $stage).Path
    $distResolved = (Resolve-Path -LiteralPath $distDir).Path
    if (-not $resolved.StartsWith($distResolved, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除 dist 之外的目录：$resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item -LiteralPath $exePath -Destination (Join-Path $stage "notepadcc.exe")
# 便携标记：文件名就叫 portable，内容写一句人话，方便用户自己看懂
Set-Content -LiteralPath (Join-Path $stage "portable") -Encoding UTF8 -Value @"
这个文件的存在表示 notepadcc 以「便携模式」运行。
配置、会话、日志、备份、崩溃转储都会写在程序目录下的 config\ 里，
不会写 %APPDATA%，也不会写注册表；整个目录拷走或删掉即可。
想去掉便携模式：删掉本文件，并把程序放到没有可写 config\ 目录的位置。
"@

Set-Content -LiteralPath (Join-Path $stage "说明.txt") -Encoding UTF8 -Value @"
notepadcc $Version（便携版 x64）

用法
  直接运行 notepadcc.exe；双击文本文件、或把它拖到程序窗口上也能打开。
  命令行：notepadcc.exe <文件>...       打开若干文件
          notepadcc.exe --multi-instance 允许同时运行多个实例
          notepadcc.exe --crash-test     自检用：验证崩溃处理（会故意崩一次）

配置
  全部写在程序目录下的 config\ 里（config.xml 首选项、shortcuts.xml 快捷键、
  session.xml 会话、backup\ 未保存内容的快照、logs\ 日志）。
  这个目录连同程序一起拷走，换台机器接着用；删掉程序目录即卸载干净。

默认关联
  本程序不会自动抢任何文件的默认打开方式。需要的话在「设置 → 加入打开方式列表」
  里登记，然后在资源管理器里选「始终使用此应用打开」。

系统要求
  Windows 10 1809 或更高（x64）。已静态链接运行库，不需要装 VC++ 可再发行组件。
"@

if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -CompressionLevel Optimal

$exeSize = [math]::Round((Get-Item (Join-Path $stage "notepadcc.exe")).Length / 1MB, 2)
$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 2)
Write-Host "[ok] $stage"
Write-Host "[ok] $zipPath"
Write-Host "     程序 $exeSize MB，压缩包 $zipSize MB（工具链：$Toolchain）"
