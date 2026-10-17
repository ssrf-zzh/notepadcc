<#
    校验 B10 的三件交付物：
      1. per-user MSI：确认真实默认路径未被占用 → 静默安装到
         %LOCALAPPDATA%\Programs\notepadcc → 文件哈希 / 开始菜单 / HKCU 产品登记
         → 静默卸载并核对清理；
      2. per-machine MSI：管理安装（msiexec /a）解出文件表并核对内容；
      3. 绿色 zip：核对可执行文件、portable 标记与说明文件。

    当前脚本不要求管理员：per-machine 的真实安装需要提权，普通用户只用 /a 校验包内容。
    真正的整机安装应由 CI 或管理员在干净虚拟机上补跑。

    用法：
      powershell -ExecutionPolicy Bypass -File scripts\verify_package.ps1 `
          -BuildDir build-msvc-final -Version 1.0.0
#>
param(
    [string]$Version = "",
    [string]$BuildDir = "build-msvc"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
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

$dist = Join-Path $root "dist"
$perUserMsi = Join-Path $dist "notepadcc-$Version-x64-peruser.msi"
$perMachineMsi = Join-Path $dist "notepadcc-$Version-x64-permachine.msi"
$portableZip = Join-Path $dist "notepadcc-$Version-portable-x64.zip"
$buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $root $BuildDir
}
$sourceExe = Join-Path $buildPath "bin\notepadcc.exe"

foreach ($required in @($perUserMsi, $perMachineMsi, $portableZip, $sourceExe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "缺少 $required；先运行 scripts\package_msi.ps1"
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Remove-SafeDirectory([string]$Path, [string]$AllowedParent) {
    $full = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($AllowedParent).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除校验目录之外的路径：$full"
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

function Invoke-Msi([string]$Arguments, [string]$LogPath, [string]$Label) {
    if (Test-Path -LiteralPath $LogPath) {
        Remove-Item -LiteralPath $LogPath -Force
    }
    Write-Host "[msi] $Label ..."
    $process = Start-Process -FilePath msiexec.exe `
                             -ArgumentList $Arguments `
                             -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -notin @(0, 3010)) {
        if (Test-Path -LiteralPath $LogPath) {
            Get-Content -LiteralPath $LogPath -Tail 20 | Write-Host
        }
        throw "$Label 失败，msiexec 退出码 $($process.ExitCode)，日志：$LogPath"
    }
}

function Find-ArpEntry([string]$rootPath) {
    if (-not (Test-Path -LiteralPath $rootPath)) {
        return @()
    }
    $found = @()
    foreach ($key in Get-ChildItem -LiteralPath $rootPath -ErrorAction SilentlyContinue) {
        $props = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
        if ($props -and $props.DisplayName -like "notepadcc*") {
            $found += [pscustomobject]@{
                ProductCode = $key.PSChildName
                DisplayName = $props.DisplayName
                InstallLocation = $props.InstallLocation
            }
        }
    }
    return $found
}

function Find-UserArpEntry {
    return @(Find-ArpEntry "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall")
}

function Find-MachineArpEntry {
    $found = @()
    foreach ($rootPath in @(
        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall",
        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
    )) {
        $found += @(Find-ArpEntry $rootPath)
    }
    return $found
}

function Find-AllArpEntry {
    return @(Find-UserArpEntry) + @(Find-MachineArpEntry)
}

function Find-UserProductEntry {
    $rootPath = "HKCU:\Software\Microsoft\Installer\Products"
    if (-not (Test-Path -LiteralPath $rootPath)) {
        return @()
    }
    $found = @()
    foreach ($key in Get-ChildItem -LiteralPath $rootPath -ErrorAction SilentlyContinue) {
        $props = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
        if ($props -and $props.ProductName -like "notepadcc*") {
            $found += [pscustomobject]@{
                Key = $key.PSChildName
                ProductName = $props.ProductName
                Path = $key.PSPath
            }
        }
    }
    return $found
}

function Get-PackedProductCode([string]$ProductCode) {
    $guid = $ProductCode.Trim()
    if ($guid.StartsWith('{')) {
        $guid = $guid.Substring(1)
    }
    if ($guid.EndsWith('}')) {
        $guid = $guid.Substring(0, $guid.Length - 1)
    }
    $guid = $guid.Replace('-', '')
    if ($guid.Length -ne 32) {
        throw "产品代码格式不正确：$ProductCode"
    }
    $reverse = {
        param([string]$value)
        return -join $value[($value.Length - 1)..0]
    }
    $packed = (& $reverse $guid.Substring(0, 8)) +
              (& $reverse $guid.Substring(8, 4)) +
              (& $reverse $guid.Substring(12, 4))
    for ($i = 16; $i -lt 32; $i += 2) {
        $packed += $guid.Substring($i + 1, 1) + $guid.Substring($i, 1)
    }
    return $packed.ToUpperInvariant()
}

function Get-MsiProductCode([string]$MsiPath) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($MsiPath, 0)
    $view = $database.OpenView("SELECT Value FROM Property WHERE Property='ProductCode'")
    $view.Execute()
    $record = $view.Fetch()
    if (-not $record) {
        throw "读不到 $MsiPath 的 ProductCode"
    }
    return $record.StringData(1)
}

$localPrograms = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Programs"
$programMenu = [Environment]::GetFolderPath("Programs")
$shortcutFolder = Join-Path $programMenu "notepadcc"
$testRoot = Join-Path $localPrograms "notepadcc"
$adminRoot = Join-Path $env:TEMP ("notepadcc-msi-admin-" + [guid]::NewGuid().ToString("N"))
$installLog = Join-Path $env:TEMP "notepadcc-msi-verify-install.log"
$uninstallLog = Join-Path $env:TEMP "notepadcc-msi-verify-uninstall.log"
$adminLog = Join-Path $env:TEMP "notepadcc-msi-verify-admin.log"
$guardLog = Join-Path $env:TEMP "notepadcc-msi-verify-launch-guard.log"
$perUserProductCode = Get-MsiProductCode $perUserMsi
$perUserPackedCode = Get-PackedProductCode $perUserProductCode
$perUserProductKey = "HKCU:\Software\Microsoft\Installer\Products\$perUserPackedCode"

$existingArp = @(Find-AllArpEntry)
$existingProducts = @(Find-UserProductEntry)
if ($existingArp.Count -gt 0 -or $existingProducts.Count -gt 0) {
    throw "系统里已经登记过 notepadcc，为避免覆盖用户安装，停止校验"
}
if (Test-Path -LiteralPath $shortcutFolder) {
    throw "开始菜单里已经有 $shortcutFolder，为避免覆盖用户快捷方式，停止校验"
}
if (Test-Path -LiteralPath $testRoot) {
    throw "默认安装目录已经存在：$testRoot，为避免覆盖用户安装，停止校验"
}

$sourceHash = Get-Sha256 $sourceExe
try {
    New-Item -ItemType Directory -Path $localPrograms -Force | Out-Null
    if (Test-Path -LiteralPath $guardLog) {
        Remove-Item -LiteralPath $guardLog -Force
    }
    $guardArgs = "/i `"$perUserMsi`" /qn /norestart ALLUSERS=1 /L*v `"$guardLog`""
    Write-Host "[msi] 验证按用户包拒绝 ALLUSERS=1 ..."
    $guard = Start-Process -FilePath msiexec.exe `
                           -ArgumentList $guardArgs `
                           -Wait -PassThru -WindowStyle Hidden
    if ($guard.ExitCode -ne 1603) {
        throw "按用户包传 ALLUSERS=1 应被拒绝（1603），实际退出码 $($guard.ExitCode)"
    }
    if (Test-Path -LiteralPath $testRoot) {
        throw "按用户包传 ALLUSERS=1 被拒绝，但留下了安装目录：$testRoot"
    }
    if (@(Find-AllArpEntry).Count -ne 0) {
        throw "按用户包传 ALLUSERS=1 被拒绝，但留下了卸载登记"
    }
    if (Test-Path -LiteralPath $perUserProductKey) {
        throw "按用户包传 ALLUSERS=1 被拒绝，但留下了 HKCU 产品登记"
    }
    Write-Host "  边界：ALLUSERS=1 被 Launch 条件拒绝，未留下安装"

    $installArgs = "/i `"$perUserMsi`" /qn /norestart /L*v `"$installLog`""
    Invoke-Msi $installArgs $installLog "按用户静默安装"

    $installedExe = Join-Path $testRoot "notepadcc.exe"
    $installedReadme = Join-Path $testRoot "安装说明.txt"
    if (-not (Test-Path -LiteralPath $installedExe)) {
        throw "安装完成但找不到 $installedExe"
    }
    if (-not (Test-Path -LiteralPath $installedReadme)) {
        throw "安装完成但找不到 $installedReadme"
    }
    foreach ($name in @("LICENSE.txt", "NOTICE.txt", "源代码说明.txt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $testRoot $name))) {
            throw "安装完成但缺少许可证材料：$name"
        }
    }
    if ((Get-Sha256 $installedExe) -ne $sourceHash) {
        throw "安装后的 notepadcc.exe 与构建产物哈希不一致"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $shortcutFolder "notepadcc.lnk"))) {
        throw "按用户安装没有创建开始菜单快捷方式"
    }

    $installLogText = Get-Content -LiteralPath $installLog -Raw
    if ($installLogText -notmatch "Per-User mode") {
        throw "安装日志没有显示 Per-User 模式"
    }
    if (-not (Test-Path -LiteralPath $perUserProductKey)) {
        throw "按用户安装没有写入 HKCU 产品登记：$perUserProductKey"
    }
    $userProduct = Get-ItemProperty -LiteralPath $perUserProductKey -ErrorAction Stop
    if ($userProduct.ProductName -notlike "notepadcc*") {
        throw "HKCU 产品登记的 ProductName 不正确：$($userProduct.ProductName)"
    }
    $arp = @(Find-AllArpEntry)
    $arpLocation = if ($arp.Count -gt 0) { "卸载登记 $($arp[0].ProductCode)" } else { "无 ARP 项" }
    Write-Host "  安装：Per-User 上下文、文件哈希、开始菜单与 HKCU 产品登记一致（$arpLocation）"

    $uninstallArgs = "/x $perUserProductCode /qn /norestart /L*v `"$uninstallLog`""
    Invoke-Msi $uninstallArgs $uninstallLog "按用户静默卸载"
    if (Test-Path -LiteralPath $testRoot) {
        throw "卸载后安装目录仍然存在：$testRoot"
    }
    if (Test-Path -LiteralPath $shortcutFolder) {
        throw "卸载后开始菜单目录仍然存在：$shortcutFolder"
    }
    if (Test-Path -LiteralPath $perUserProductKey) {
        throw "卸载后 HKCU 产品登记仍然存在：$perUserProductKey"
    }
    if (@(Find-AllArpEntry).Count -ne 0) {
        throw "卸载后仍能查到 notepadcc 的卸载登记（HKCU 或 HKLM）"
    }
    Write-Host "  卸载：安装目录、开始菜单与卸载登记均已清理"

    New-Item -ItemType Directory -Path $adminRoot -Force | Out-Null
    $adminArgs = "/a `"$perMachineMsi`" /qn TARGETDIR=`"$adminRoot`" /L*v `"$adminLog`""
    Invoke-Msi $adminArgs $adminLog "整机包管理安装"
    $adminExe = Get-ChildItem -LiteralPath $adminRoot -Recurse -Filter "notepadcc.exe" |
                Select-Object -First 1
    $adminReadme = Get-ChildItem -LiteralPath $adminRoot -Recurse -Filter "安装说明.txt" |
                   Select-Object -First 1
    $adminLicense = Get-ChildItem -LiteralPath $adminRoot -Recurse -Filter "LICENSE.txt" |
                    Select-Object -First 1
    $adminSourceNotice = Get-ChildItem -LiteralPath $adminRoot -Recurse -Filter "源代码说明.txt" |
                         Select-Object -First 1
    if (-not $adminExe -or -not $adminReadme -or -not $adminLicense -or -not $adminSourceNotice) {
        throw "整机包管理安装后没有找到程序、说明或许可证材料"
    }
    if ((Get-Sha256 $adminExe.FullName) -ne $sourceHash) {
        throw "整机包管理安装后的 notepadcc.exe 与构建产物哈希不一致"
    }
    Write-Host "  整机包：管理安装解出的文件表与内容正确（真实安装需管理员权限）"

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($portableZip)
    try {
        $leaves = @($archive.Entries | ForEach-Object { [IO.Path]::GetFileName($_.FullName) })
        foreach ($name in @("notepadcc.exe", "portable", "说明.txt", "LICENSE.txt", "源代码说明.txt")) {
            if ($leaves -notcontains $name) {
                throw "绿色 zip 缺少 $name"
            }
        }
        $entry = $archive.Entries | Where-Object { [IO.Path]::GetFileName($_.FullName) -eq "notepadcc.exe" } |
                 Select-Object -First 1
        $stream = $entry.Open()
        try {
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $zipHash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
            } finally {
                $sha.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
        if ($zipHash -ne $sourceHash) {
            throw "绿色 zip 里的 notepadcc.exe 与构建产物哈希不一致"
        }
    } finally {
        $archive.Dispose()
    }
    Write-Host "  绿色 zip：程序、portable 标记与说明文件齐全，程序哈希一致"
} finally {
    $cleanupCodes = @(Find-AllArpEntry | ForEach-Object { $_.ProductCode })
    if (Test-Path -LiteralPath $perUserProductKey) {
        $cleanupCodes += $perUserProductCode
    }
    foreach ($productCode in @($cleanupCodes | Select-Object -Unique)) {
        $cleanupLog = Join-Path $env:TEMP "notepadcc-msi-verify-cleanup-$productCode.log"
        try {
            Invoke-Msi "/x $productCode /qn /norestart /L*v `"$cleanupLog`"" `
                       $cleanupLog "清理遗留安装 $productCode"
        } catch {
            Write-Warning "清理遗留安装失败：$($_.Exception.Message)"
        }
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-SafeDirectory $testRoot $localPrograms
    }
    if (Test-Path -LiteralPath $adminRoot) {
        Remove-SafeDirectory $adminRoot $env:TEMP
    }
}

Write-Host "PASS: MSI 静默安装/卸载、整机包内容与绿色 zip 校验通过"
