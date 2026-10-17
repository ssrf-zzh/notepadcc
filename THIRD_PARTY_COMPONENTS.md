# 第三方组件清单

本清单对应 notepadcc 1.0.0 的 MSVC 正式发布链。组件均以静态库或静态运行库形式
进入 `notepadcc.exe`，发行目录不附带 DLL。发布前由 `scripts\release.ps1` 重新生成
CycloneDX SBOM，并核对本表、`NOTICE` 与源码树中的许可证原文三者一致。

notepadcc 项目主体按 `GPL-3.0-only` 发布，版权人为 `notepadcc contributors`。
下表组件仍按各自许可证授权；它们均与 GPL-3.0 兼容，项目许可证不会替代或削弱其原有条款。

| 组件 | 版本 | 许可证 | 使用方式 | 修改 | 上游 |
|---|---|---|---|---|---|
| Scintilla | 5.6.7（`version.txt` = 567） | HPND | 静态库，编辑器控件与文本渲染 | 无 | https://www.scintilla.org/ |
| Lexilla | 5.5.4（`version.txt` = 554） | HPND | 静态库，语法词法器 | 无 | https://www.scintilla.org/LexillaDoc.html |
| PCRE2 | 10.49 | BSD-3-Clause WITH PCRE2-exception | 静态库，8-bit，自研查找/替换与格式化共用 | 无；只改 CMake 开关（仅 8-bit、开 JIT、关测试与 grep） | https://www.pcre.org/ |
| SLJIT | PCRE2 10.49 内置修订 | BSD-2-Clause | PCRE2 JIT 后端 | 无 | https://github.com/zherczeg/sljit |
| Microsoft Visual C++ Runtime | 随 MSVC 14.44 工具链 | Microsoft 软件许可条款 | `/MT` 静态运行库 | 无 | https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist |

## 许可证准入

- 本项目主体为 `GPL-3.0-only`。新增第三方代码必须与 GPL-3.0 兼容；当前主程序只使用
  MIT / BSD / Apache-2.0 / HPND 等宽松组件，禁止引入无法满足 GPL 的专有代码。
- 引入 GPL / AGPL / LGPL 或其他带额外义务的组件前，先评审链接方式、插件边界、源代码
  交付与许可证兼容性。
- 任何新增第三方代码必须同时更新本表、`NOTICE` 与 `scripts\generate_sbom.py`，并在
  发布流程中重新生成 SBOM。
- 构建工具（CMake、Ninja、MinGW-w64、WiX）只在构建期使用，不进入最终安装包；
  本清单因此不把它们列为随产品分发的组件。

## 许可证材料位置

| 组件 | 仓库内位置 |
|---|---|
| Scintilla | `third_party\scintilla\License.txt` |
| Lexilla | `third_party\lexilla\License.txt` |
| PCRE2 | `third_party\pcre2\LICENCE.md` |
| SLJIT | `third_party\pcre2\deps\sljit\LICENSE` |
| Microsoft Visual C++ Runtime | `licenses\MSVC-RUNTIME-NOTICE.txt`（静态链接说明；微软完整条款不随仓库复制） |

发布脚本把四份上游原文和 MSVC 运行时说明复制到
`dist\release-<版本>\licenses\`，再把 `NOTICE` 与本表放进同一发行目录。MSVC
运行时的完整许可条款随 Microsoft Visual Studio Build Tools 提供，发布者应确认
其工具链许可允许相应分发。正式对外发布前还必须确认来源哈希与
`scripts\fetch_deps.ps1` 固定的版本一致。`scripts\package_source.ps1` 会把上述第三方
组件的源码一并放进 `notepadcc-<版本>-source.zip`，满足 GPL 对对应源代码的要求。
