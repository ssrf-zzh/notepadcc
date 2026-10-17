<p align="center">
  <img src="res/notepadcc-logo.png" width="140" alt="notepadcc logo">
</p>

<h1 align="center">notepadcc</h1>

<p align="center">轻量、原生、可扩展的 Windows 文本与源码编辑器</p>

<p align="center">
  <a href="https://github.com/ssrf-zzh/notepadcc/releases/latest">
    <img src="https://img.shields.io/github/v/release/ssrf-zzh/notepadcc?label=release" alt="Latest release">
  </a>
  <img src="https://img.shields.io/badge/Windows-10%201809%2B%20x64-0078D4?logo=windows" alt="Windows 10 1809+ x64">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--only-blue" alt="GPL-3.0-only"></a>
</p>

notepadcc 是一款面向 Windows 的原生文本 / 源码编辑器。编辑内核基于
**Scintilla + Lexilla**，界面使用 **Win32 API + C++20** 直接构建，不依赖 .NET、Qt
或 Electron。程序体积小、启动快，发布版静态链接运行库，解压即可使用。项目完全开源
免费，按 GPL-3.0-only 发布。

> [!TIP]
> 普通用户推荐下载 **绿色便携版**，解压后直接运行 `notepadcc.exe`。

## 下载

| 版本 | 适用场景 | 直接下载 |
|---|---|---|
| 绿色便携版 x64 | 解压即用，配置保存在程序目录 | [下载 ZIP](https://github.com/ssrf-zzh/notepadcc/releases/download/v1.0.0/notepadcc-1.0.0-portable-x64.zip) |
| 对应源码包 | GPL-3.0 对应源代码 | [下载源码](https://github.com/ssrf-zzh/notepadcc/releases/download/v1.0.0/notepadcc-1.0.0-source.zip) |

完整发布页：[github.com/ssrf-zzh/notepadcc/releases](https://github.com/ssrf-zzh/notepadcc/releases)

下载后建议使用同目录的 `SHA256SUMS` 校验文件完整性。

## 主要特性

- **原生轻量**：Win32 + C++20 桌面程序，不使用 Web 运行时；MSVC 版本静态链接 CRT。
- **现代界面**：自绘菜单栏、工具栏、标签栏、状态栏和对话框，支持浅色、深色与护眼主题。
- **高效编辑**：多标签、拖拽排序、会话恢复、代码折叠、自动完成、书签、宏与列编辑。
- **搜索替换**：文档内查找替换、跨文件搜索替换、正则表达式、通配符与结果跳转。
- **大文件支持**：内存映射视口，适合查看和搜索大型日志及文本文件。
- **开发工具**：文件比较、JSON / XML 格式化与树视图、XPath、Markdown 预览、十六进制查看。
- **导入导出**：打印、导出 HTML / RTF、复制为带语法着色内容。
- **远程与扩展**：FTP / SFTP 远程编辑、脚本插件宿主、插件 SDK、运行命令与界面定制。
- **可靠恢复**：外部文件变更检测、崩溃快照、未保存内容恢复、单实例与文件关联。

## 快速开始

### 绿色便携版

1. 下载并解压 `notepadcc-1.0.0-portable-x64.zip`。
2. 运行 `notepadcc.exe`。
3. 保持 `portable` 文件与程序在同一目录，配置、会话、日志和备份会写入程序目录下的
   `config\`，不会写入 `%APPDATA%` 或注册表。

### 命令行

```text
notepadcc.exe <文件>...        打开一个或多个文件
notepadcc.exe --portable       强制以便携模式运行
notepadcc.exe --multi-instance 允许多个程序实例
```

## 系统要求

- Windows 10 1809 或更高版本
- x64 处理器
- 无需另行安装 VC++ 可再发行组件

> [!IMPORTANT]
> 当前 1.0.0 发布包使用自签名证书完成 Authenticode 签名与 RFC 3161 时间戳。
> 该证书链默认不受 Windows 信任，SmartScreen 仍可能提示“未知发布者”；GitHub 上的
> 自签名构建仅用于测试与预览，正式发布须改用受信任的 CA 代码签名证书重新构建。
> 请优先核对 `SHA256SUMS`，并只从本仓库的 Releases 页面下载。

## 从源码构建

### 环境要求

- CMake 3.25+
- PowerShell 5.1+
- 以下任一编译器：
  - MSVC v143 / Visual Studio 2022 Build Tools
  - MinGW-w64 GCC 13+（搭配 Ninja）
- 首次构建时能够访问 Scintilla、Lexilla、PCRE2 与 SLJIT 的下载地址

### 1. 获取第三方源码

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1
```

依赖会解压到 `third_party/`，不会提交进仓库。

### 2. MinGW / Ninja 构建

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：`build\bin\notepadcc.exe`

### 3. MSVC 构建

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

产物：`build-msvc\bin\notepadcc.exe`

MSVC 请使用 Visual Studio 生成器；Ninja 构建路径主要用于 MinGW。

### 4. 生成发行包

```powershell
# 绿色便携包
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Toolchain msvc -BuildDir build-msvc

# 完整发布包（EXE、便携 ZIP、源码包、校验文件、SBOM 等）
# 使用证书存储中的 CA 代码签名证书（正式发布）
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 1.0.0 -BuildDir build-msvc -SigningThumbprint <sha1-thumbprint>

# 或使用 PFX 文件签名
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 1.0.0 -BuildDir build-msvc -PfxPath D:\certs\notepadcc.pfx -PfxPassword <password>

# 自签名测试版：签名有效但证书链不受信任，产物标记为 signed-untrusted（仅用于测试/预览）
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 1.0.0 -BuildDir build-msvc -SigningThumbprint <sha1-thumbprint> -AllowUntrustedSignature

# 无证书的本地验收（仅用于测试）
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 1.0.0 -BuildDir build-msvc -AllowUnsigned
```

发布脚本会先为 EXE 签名，再生成便携 ZIP；签名算法为 SHA-256，并使用 RFC 3161
时间戳。写入 `SHA256SUMS` 前会执行 `signtool verify /pa /all`。正式证书必须通过
信任链校验；自签名证书须显式传入 `-AllowUntrustedSignature`，产物在清单和下载页中
标记为 `signed-untrusted`，不得作为正式可信版本发布。

## 项目结构

```text
src/app/        程序入口
src/core/       文档、配置、命令、搜索、更新等核心逻辑
src/edit/       Scintilla 编辑视图、语言、编码与样式
src/shell/      主窗口、菜单、工具栏、标签栏和对话框
src/search/     文档内与跨文件搜索
src/infra/      Win32、HTTP、FTP、加密与系统集成
src/io/         文件读写、原子保存与内存映射
src/plugin/     插件宿主
plugins-sdk/    插件开发头文件
packaging/      打包与下载页模板
scripts/        依赖获取、构建、打包和发布脚本
res/            图标、资源与应用程序清单
samples/        示例插件
tests/          单元测试
```

## 许可证与第三方组件

notepadcc 按 **GNU General Public License v3.0 only**（`GPL-3.0-only`）发布，
版权归 notepadcc contributors 所有。许可证全文见 [LICENSE](LICENSE)。

项目静态链接 Scintilla、Lexilla、PCRE2 与 SLJIT。第三方版权和许可信息见
[NOTICE](NOTICE) 与 `licenses/` 目录。转发绿色版时，必须同时提供同一版本的
`notepadcc-<版本>-source.zip`，不能只转发二进制文件。

notepadcc 是自由软件，官方发行永久免费。你可以按 GPL-3.0 的条款使用、研究、修改和
再分发本项目。

## 作者与联系

- 作者：zzh（网络昵称：人亨云恒）
- 邮箱：16940454@qq.com
- GitHub：[@ssrf-zzh](https://github.com/ssrf-zzh)

如果 notepadcc 对你有帮助，欢迎 Star 支持项目。
