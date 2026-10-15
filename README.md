# notepadcc

原生 Windows x64 轻量文本 / 源码编辑器。内核嵌入 Scintilla + Lexilla，外壳用 Win32 API 与 C++20
直接编写，不依赖 .NET / Qt / Electron。

设计依据见 [notepadcc详细设计说明书.md](notepadcc详细设计说明书.md)（功能基线为
[Notepad++功能全景梳理.md](Notepad++功能全景梳理.md)）。

## 当前进度

已完成 **M0（骨架）**：能开窗、能打字、能存盘。

| 能力 | 状态 |
|---|---|
| 主窗口、菜单、状态栏 | 已完成 |
| **多标签**：新建/打开/关闭/关闭其他/关闭右侧/关闭全部、中键关闭、拖拽排序、脏标记 | 已完成 |
| 自绘标签栏（宽度自适应、滚轮横向滚动、右键菜单、复制完整路径） | 已完成 |
| **会话恢复**：退出记住打开的文件与光标/滚动/缩放，下次启动原样回来 | 已完成 |
| 轻量 XML 读写（供会话使用，后续配置 / 快捷键 / 宏复用） | 已完成 |
| 命令注册表（菜单 / 快捷键 / 勾选态同源） | 已完成 |
| Scintilla 集成与静态链接 | 已完成（5.6.7） |
| 语法着色（19 类扩展名，Lexilla） | 已完成（M1 起改为 langs.xml 驱动） |
| 编码识别：UTF-8 / UTF-8-BOM / ANSI(GBK) / UTF-16LE / UTF-16BE | 已完成 |
| 行尾识别与保存时统一（CRLF / LF / CR / Mixed） | 已完成 |
| 原子写盘（临时文件 + ReplaceFile，失败不动原文件） | 已完成 |
| 命令行传入文件、拖放打开、便携模式 | 已完成 |
| 单元测试（146 项检查，含多文档管理、XML、会话） | 已完成 |
| 双工具链构建 | MinGW-w64 与 MSVC 均构建通过、无警告，各自 146 项单元测试全绿 |
| 查找替换、列编辑、大文件分档 | 未开始（M1 后续） |

## 环境要求

- CMake ≥ 3.25、Ninja
- 下列任一编译器：
  - **MinGW-w64 GCC 13+**（本机已装 16.2.0，免管理员取得）
  - **MSVC v143**（本机已装 Visual Studio 2022 Build Tools 17.14，MSVC 14.44 + Windows SDK 10.0.26100）
- 第三方源码（Scintilla / Lexilla）不入库，首次构建前拉取

免管理员的工具链获取方式（本机已装好，仅供换机参考）：

```powershell
scoop install cmake ninja mingw
```

## 构建

```powershell
# 1. 拉取第三方源码（Scintilla 5.6.7 + Lexilla 5.5.4，解压到 third_party/）
powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1

# 2. 配置与编译（MinGW）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

MSVC（正式发布链）**必须用 Visual Studio 生成器，不要配 Ninja**：

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

这里有一个坑值得记下来，否则会白白踩一次：MSVC 配 Ninja 时，CMake 会把 `/showIncludes`
的提示前缀写进 `build.ninja`，而中文版 MSVC 的前缀（`注意: 包含文件:`）在写入过程中编码
错乱（UTF-8 字节被当成 GBK），于是 Ninja 永远匹配不上 cl 的输出，**头文件依赖全部丢失**
——改一个 `.h` 不会触发任何重编，只有 `--clean-first` 才有效。MSBuild 用自己的 tlog 跟踪
依赖，不受界面语言影响，所以 MSVC 侧走 VS 生成器。Ninja 留给 MinGW 用，GCC 走的是
`-MD -MF` 真正的 depfile，没有这个问题。

两个工具链的产物都落在各自的 `bin/` 下，路径一致：

```
build\bin\notepadcc.exe          # MinGW-w64（7.7 MB，静态链接）
build-msvc\bin\notepadcc.exe     # MSVC（2.2 MB，/MT 静态 CRT）
```

第三方源码需要可编译访问 `www.scintilla.org`。若网络受限，把两个 zip 手动放到
`third_party/_dl/` 后重跑 `scripts\fetch_deps.ps1` 即可。

## 运行与测试

```powershell
build\bin\notepadcc.exe                     # 普通启动（配置在 %APPDATA%\notepadcc）
build\bin\notepadcc.exe -portable           # 便携模式（配置在程序目录 config\）
build\bin\notepadcc.exe ..\some\file.cpp    # 启动即打开文件
build\bin\notepadcc_tests.exe               # 单元测试
```

## 目录结构

```
src/
  app/     入口、命令行解析、启动顺序
  core/    命令注册表、ID 与命令模型
  edit/    Scintilla 唯一适配层（EditView）、编码与行尾、文档状态
  infra/   路径与便携判定、日志
  io/      原子写盘、整文件读取
  shell/   主窗口、菜单生成、状态栏
res/       图标与清单资源（app.manifest 声明 PerMonitorV2 DPI）
tests/     单元测试
tools/     文档生成与校验脚本（md2docx.py / verify_docx.py / update_toc.ps1）
third_party/  由脚本拉取，不入库
```

## 几条需要遵守的约定

- **只有 `src/edit/EditView.*` 允许调用 Scintilla 消息**。其他模块一律走它的语义化方法，
  这样将来换内核或升级 Scintilla 时改动面收敛在一个文件里。
- **Scintilla 只能在 UI 线程访问**，且新窗口/DestroyWindow 之后不得再向其发送消息。
- **命令 ID 必须落在 16 位以内**（`core::MakeCmdId`，高 4 位类别 + 低 12 位序号），
  因为菜单项 ID 与 `ACCEL.cmd` 都是 `WORD`。插件命令超范围时由宿主分配菜单槽位映射。
- **写文件必须走 `io::WriteFileAtomic`**，任何直接 `fopen(...,"w")` 都可能截断用户数据。
- **宽字符 printf 用 `%ls`**，`%s` 在标准 C 语义下是窄字符串（MSVC 差异）。
