# notepadcc 详细设计说明书

| 项目 | 内容 |
|---|---|
| 产品名称 | notepadcc |
| 文档名称 | 详细设计说明书 |
| 文档版本 | v1.0 |
| 编制日期 | 2026-10-15 |
| 目标平台 | Windows 10 1809 及以上 / Windows 11 |
| 目标架构 | **x64（Win64）**，仅提供 64 位版本，不规划 Win32/x86 目标 |
| 编译工具链 | 双轨：MSVC v143（正式发布，`/W4 /permissive-`）＋ MinGW-w64 GCC 13+（本地开发与 CI，`-Wall -Wextra -Wpedantic`）；C++20 |
| 编辑内核版本 | Scintilla 5.6.7 ＋ Lexilla 5.5.4（随源码固定版本） |
| 正则引擎版本 | PCRE2 10.49（8-bit，JIT；JIT 后端 sljit 按 PCRE2 固定的提交号拉取） |
| 构建系统 | CMake ≥ 3.25；MinGW 侧用 Ninja，MSVC 侧用 Visual Studio 17 2022 生成器（理由见 ADR-10） |
| 设计基线 | 《Notepad++功能全景梳理.md》（14 功能域 + P0/P1/P2 分级 + 验收自检清单） |
| 对标策略 | 全面对标、功能全覆盖，不做功能裁剪 |

---

## 0. 文档说明

### 0.1 编写目的

本文档把《Notepad++功能全景梳理.md》定义的功能基线，落地为可施工的工程设计：说明技术选型与总体架构、模块边界与职责、核心数据结构、对外与对内接口、关键流程时序、配置文件格式、性能预算与实施路线。

文档的读者是参与 notepadcc 的开发、测试与验收人员。阅读后可回答三个问题：**这一版要做成什么样、代码分成哪些部分、每个部分凭什么接口对接**。

### 0.2 与基线的对应关系

基线文档定义了"做什么"，本文档定义"怎么做"。对应关系为：

| 基线内容 | 本文档对应章节 |
|---|---|
| 二、功能模块全景梳理（14 功能域） | 第 2 章 模块划分与职责 |
| 三、功能优先级分级（P0/P1/P2） | 第 10 章 实施路线 |
| 四、已知短板与差异化机会 | 6.9 差异化能力设计 |
| 五、功能清单总览速查表 | 第 2 章 模块总表 |
| 附录 A 常用快捷键 | 附录 A |
| 附录 B 验收自检清单 | 第 9 章 测试与验收 |

### 0.3 架构决策记录（ADR 摘要）

本文档涉及的几项关键决策先集中列出，后文不再重复论证：

| 编号 | 决策 | 结论 | 主要理由 |
|---|---|---|---|
| ADR-01 | 目标架构 | x64 单一目标 | 见 1.1.2 |
| ADR-02 | 编辑内核 | 嵌入 Scintilla（静态链接，x64） | 与基线 1.2 节一致；语法着色、折叠、列编辑等成熟能力开箱可用 |
| ADR-03 | 外壳框架 | Win32 API + STL 原生，不引入 .NET/Qt/Electron | 保住"轻、快、小"三条底线 |
| ADR-04 | 正则引擎 | 统一使用 PCRE2（8-bit, UTF 模式, JIT） | 单文档与跨文件搜索语义一致，避免两套正则行为 |
| ADR-05 | 配置格式 | XML，集中于 config 目录 | 与基线中宏/快捷键的 XML 迁移能力对齐，便于手工审计与插件读取 |
| ADR-06 | 插件 ABI | C 导出函数 + 版本化函数表，x64 限定 | 二进制兼容可控，避免 C++ 名称修饰与 STL 跨模块问题 |
| ADR-07 | 文档与视图 | 一文档多视图，视图只读共享 | 支撑"克隆到另一视图"与未来的多分屏 |
| ADR-08 | 线程模型 | UI 单线程持有全部编辑控件，重活下沉到线程池 | Scintilla 非线程安全，这是硬约束 |
| ADR-09 | 产品命名 | **notepadcc**（早期设计代号为 codepad++） | 用户指定；全文、代码标识、配置目录与插件 ABI 前缀已同步更名 |
| ADR-10 | 编译工具链 | MSVC v143（正式发布）与 MinGW-w64 GCC（本地与 CI）双轨，均以 CMake 驱动；MinGW 用 Ninja，MSVC 用 Visual Studio 生成器 | 代码不使用任一编译器专有扩展，两边均已构建通过（各 52 项单元测试全绿）。MSVC 不用 Ninja 的原因：本地化 MSVC 的 `/showIncludes` 前缀在写入 `build.ninja` 时编码错乱，导致头文件依赖全部丢失、增量构建失效；MSBuild 的 tlog 机制与界面语言无关。MSVC 侧另需 `/MT` 静态 CRT 以满足便携要求 |

**关于 ADR-09 的更名范围**：本次更名是彻底的，不留旧名残留，涉及以下几类实体——

| 位置 | 旧值 | 新值 |
|---|---|---|
| 产品与窗口标题 | codepad++ | notepadcc |
| 可执行文件 | codepad++.exe | notepadcc.exe |
| CMake 工程与目标 | codepadpp / codepad | notepadcc |
| C++ 命名空间 | `cpp::<module>` | `npcc::<module>` |
| 安装模式配置目录 | `%APPDATA%\CodePad++` | `%APPDATA%\notepadcc` |
| 配置 XML 根元素 | `<CodePadConfig>` | `<NotepadccConfig>` |
| 日志文件 | codepad.log | notepadcc.log |
| 插件 ABI 前缀 | `CpHostServices` / `CpPlugin_*` / `CPPLUGIN_ABI_VERSION` | `NpccHostServices` / `NpccPlugin_*` / `NPCC_PLUGIN_ABI_VERSION` |
| 插件开发包头 | `codepad_plugin.h` | `notepadcc_plugin.h` |

唯一**保持不变**的是对标产品的兼容命名：`shortcuts.xml` 的顶层元素仍为 `<NotepadPlusCompat>`（见 7.3），这是为了让用户能直接沿用旧的快捷键与宏文件，属于功能需要而非命名残留。

### 0.4 术语表

| 术语 | 含义 |
|---|---|
| Buffer / 缓冲区 | 一份文档数据及其编辑状态，对应一个 Scintilla 文档对象 |
| View / 视图 | 呈现缓冲区的窗口区域；一个缓冲区可被多个视图呈现 |
| 视图组（ViewGroup） | 一组并排的视图（主视图、次视图、扩展视图），构成分屏布局 |
| 便携模式 | 配置与缓存写入程序目录而非 `%APPDATA%` 的运行方式 |
| EOL | 行尾符，取值 CRLF / LF / CR |
| UDL | 用户自定义语言（User Defined Language） |
| 命令 | 一个可由菜单、工具栏、快捷键、插件或宏触发的具名动作 |
| 宿主 | notepadcc 主程序，相对于插件而言 |
| 大文件模式 | 针对超过阈值文件的降级编辑策略集合 |

---

## 1. 设计总览

### 1.1 设计目标与约束

#### 1.1.1 必须满足的目标

| 目标 | 量化指标 | 验证方式 |
|---|---|---|
| 轻 | 安装包 ≤ 20 MB；便携版解压后 ≤ 40 MB | 构建产物度量 |
| 快 | 冷启动 ≤ 1.0 s（SSD、默认配置、无插件） | 启动计时埋点 |
| 快 | 打开 50 MB 文本文件到可交互 ≤ 2.0 s | 性能用例 |
| 快 | 50 MB 文件内字面量查找首个匹配 ≤ 300 ms | 性能用例 |
| 稳 | 常驻内存（空闲 + 10 个中等文档）≤ 40 MB | 任务管理器采样 |
| 稳 | 连续编辑 8 小时无内存增长趋势（回归压测） | 长稳测试 |
| 开放 | 插件可安装/卸载/启停，配置与插件可随目录迁移 | 验收用例 |

#### 1.1.2 目标架构定为 x64 的影响

原始规划沿用 Notepad++ 的 Win32 路线。本版**改为 Win64（x64）单一目标**，该调整带来以下必须显式处理的后果：

1. **进程内可直接访问超过 2 GB 的连续地址空间**。这是本文档 6.1 节"大文件零拷贝映射"方案能够成立的前提；32 位下只能靠分块视图模拟。
2. **指针与句柄宽度统一为 64 位**。所有内部结构体、自定义消息参数（`WPARAM`/`LPARAM`）不再需要 `LONG`/`LONG_PTR` 折衷；但仍禁止把指针塞进 `DWORD`。
3. **插件与第三方库必须提供 x64 版本**。宿主对 32 位 DLL 一律拒绝加载并给出明确提示，不提供跨位数桥接。
4. **注册表与文件系统重定向不再是问题**。x64 进程直接访问 `HKLM\Software\...` 与 `%ProgramFiles%` 的真实视图，不再需要处理 WOW64 重定向；但这也意味着 32 位版本的旧配置目录不会被自动继承。
5. **Shell 集成需按 x64 注册**。右键菜单、文件关联若通过 Shell 扩展实现，必须编译为 x64 并与主程序位数一致；本版优先使用注册表纯声明式关联（无需 Shell 扩展 DLL），避免位数与 Explorer 版本耦合。
6. **体积略有增加**。指针与对齐变宽，x64 可执行文件比 x86 大约 10%~20%；通过 `/O2 /Gy /OPT:REF /OPT:ICF` 与静态 CRT 缓解，仍远低于 20 MB 门槛。
7. **不再需要处理 64 位文件系统的超长路径缺失问题之外的兼容分支**，即代码中不保留 `#ifdef _WIN64` 的双路径实现——单一目标意味着不留 32 位死代码。

#### 1.1.3 设计约束

- Scintilla 控件必须在 UI 线程创建与访问，且每次调用必须先取得目标视图，禁止跨线程 `SendMessage`。
- 任何写文件的动作必须是原子替换（临时文件 + `MoveFileEx`），避免断电或崩溃导致源文件截断。
- 所有用户可见字符串必须走语言包键值，不允许硬编码在代码里。
- 不在同一进程内直接执行不受信任的插件代码是理想状态；本版采用"同进程 + 结构化异常兜底"的折中（见 6.7）。

### 1.2 技术选型

| 层次 | 选型 | 说明 |
|---|---|---|
| 语言 | C++20 | `std::string_view`、`std::filesystem`、`std::span` 显著减少样板代码 |
| 编译器 | MSVC v143（`/std:c++20 /permissive- /utf-8`）与 MinGW-w64 GCC 13+（`-std=c++20`） | 见 ADR-10。两边都必需的约束：源文件一律 UTF-8 无 BOM，宽字符 printf 使用 `%ls`（`%s` 在标准 C 里表示窄字符串） |
| 编辑内核 | Scintilla 5.x（静态库，Lexilla 独立库） | 语法着色由 Lexilla 的 lexer 提供 |
| UI | 原生 Win32 + 自绘控件 | 菜单、工具栏、标签栏、停靠面板、状态栏均自绘以保证主题一致性 |
| 正则 | PCRE2（8-bit，UTF，JIT） | 统一引擎，见 ADR-04 |
| XML | 轻量 DOM 解析器（内置，如 pugixml 风格的最小实现） | 配置与会话文件读写，避免引入重型依赖 |
| 字符集 | Unicode（UTF-16 内部 API 接口，UTF-8 文档内部存储） | 窗口类全部使用 `W` 系列 API |
| 压缩 | zlib（仅用于插件的 `.zip` 离线包解压） | 无网络依赖场景 |
| 加密 | Windows CNG（BCrypt） | 更新包签名校验、哈希计算 |

不引入的组件：.NET / CLR、Qt、Electron、Boost 全量（仅按需取用 header-only 部分）、CMake 之外的大型构建框架。

### 1.3 总体架构

采用**五层 + 一条通知总线**的结构。上层依赖下层，反向通信一律通过通知总线或回调接口，禁止下层直接引用上层类型。

```
┌───────────────────────────────────────────────────────────────┐
│ L5 表现层 (shell / ui)                                        │
│   主窗口 · 菜单 · 工具栏 · 标签栏 · 停靠面板 · 状态栏 · 对话框   │
├───────────────────────────────────────────────────────────────┤
│ L4 应用层 (app / core)                                        │
│   命令注册表 · 文档管理器 · 会话管理 · 配置中心 · 最近文件       │
│   通知总线 · 运行命令 · 宏录制/回放                             │
├───────────────────────────────────────────────────────────────┤
│ L3 领域服务层 (edit / search / lang / macro / plugin / io)     │
│   编辑视图适配 · 编码与 EOL · 列编辑 · 搜索引擎 · 跨文件扫描     │
│   语法与 UDL · 样式配置 · 函数列表 · 自动完成 · 插件宿主         │
├───────────────────────────────────────────────────────────────┤
│ L2 基础设施层 (infra)                                         │
│   文件 IO · 内存映射 · 线程池 · 任务队列 · 路径 · 日志 · 加解密  │
│   字符编码转换 · 注册表 · 剪贴板 · i18n 资源                     │
├───────────────────────────────────────────────────────────────┤
│ L1 平台层                                                     │
│   Win32 API (x64) · Scintilla / Lexilla · PCRE2 · CRT         │
└───────────────────────────────────────────────────────────────┘

通知总线 (NotificationBus) 横跨 L3~L5：文档打开/关闭/保存/修改、
语言切换、主题切换、配置变更、插件事件等均以 (code, payload) 广播。
```

分层的三条硬规则：

1. **表现层不含业务判断**。菜单项是否置灰、点击后做什么，全部由命令注册表中的命令对象决定；视图只负责渲染与转发消息。
2. **领域服务层不弹窗**。所有需要用户确认的动作以返回值或事件形式上报，由应用层决定是否提示。这条规则保证服务层可被单元测试直接驱动。
3. **基础设施层不知道"文档"概念**。它只处理字节、文件、线程与句柄。

### 1.4 源码目录与工程结构

```
notepadcc/
├── CMakeLists.txt
├── src/
│   ├── app/            应用入口与生命周期
│   │   ├── main.cpp            wWinMain、单实例判定、命令行解析
│   │   ├── AppContext.h/.cpp   全局服务容器的构造与销毁顺序
│   │   └── CommandLine.h/.cpp  参数词法分析与语义解释
│   ├── shell/          主窗口与外壳
│   │   ├── MainWindow.*        主窗口过程、布局、视图组管理
│   │   ├── MenuBuilder.*       由命令表生成菜单与右键菜单
│   │   ├── Toolbar.*           自绘工具栏（大小/图标/自定义按钮）
│   │   ├── TabBar.*            自绘标签栏（拖拽、颜色组、右键菜单）
│   │   ├── DockHost.*          停靠面板宿主（左右上下、浮动、布局持久化）
│   │   ├── StatusBar.*         多分区状态栏
│   │   └── FindResultsDock.*   查找结果、函数列表等面板
│   ├── core/           应用层
│   │   ├── Command.h           命令描述结构
│   │   ├── CommandRegistry.*   命令注册、查找、执行、状态刷新
│   │   ├── DocumentManager.*   缓冲区与视图的生成、归属、销毁
│   │   ├── SessionManager.*    会话快照与恢复
│   │   ├── ConfigCenter.*      配置加载、校验、保存、变更广播
│   │   ├── RecentFiles.*       最近文件与最近关闭列表
│   │   ├── NotificationBus.*   事件总线
│   │   └── RunCommand.*        F5 运行命令与变量展开
│   ├── edit/           编辑领域
│   │   ├── EditView.h/.cpp     Scintilla 适配封装（唯一允许 SendMessage 的地方）
│   │   ├── ViewGroup.*         分屏布局与视图切换
│   │   ├── DocBuffer.*         缓冲区状态（编码、EOL、只读、语言、书签）
│   │   ├── Encoding.*          编码探测、转换、BOM 处理
│   │   ├── EolMode.*           行尾识别与转换
│   │   ├── Bookmark.*          书签与标记
│   │   ├── ColumnEdit.*        列选择、列块编辑、数字序列插入
│   │   └── Folding.*           折叠状态保存与恢复
│   ├── search/         搜索领域
│   │   ├── SearchConfig.h      搜索参数模型
│   │   ├── RegexEngine.*       PCRE2 封装（编译、匹配、超时保护）
│   │   ├── LiteralSearcher.*   扩展模式转义解析与字面量快速路径
│   │   ├── InDocSearch.*       单文档搜索与替换、高亮指示器
│   │   └── CrossFileSearch.*   跨文件扫描、过滤器、结果模型、可取消任务
│   ├── lang/           语言与语法
│   │   ├── LexerRegistry.*     内置语言表与 Lexilla 绑定
│   │   ├── LanguageDetect.*    扩展名/内容/首行识别
│   │   ├── StyleConfig.*       样式配置器模型与持久化
│   │   ├── UdlLoader.*         UDL 定义解析与导出 Scintilla 关键词表
│   │   ├── FunctionList.*      函数/结构树提取（lexer 驱动）
│   │   └── AutoComplete.*      字词补全、API 词库、调用提示
│   ├── macro/          宏
│   │   ├── MacroRecorder.*     命令流录制
│   │   ├── MacroPlayer.*       回放与重复执行
│   │   └── MacroStore.*        宏的持久化与迁移
│   ├── plugin/         插件
│   │   ├── PluginApi.h         对外 ABI 头（纯 C）
│   │   ├── PluginHost.*        加载、初始化、通知分发、卸载
│   │   ├── PluginAdmin.*       在线/离线安装、升级、卸载、启停
│   │   └── PluginSecurity.*    位数/签名/哈希校验
│   ├── io/             文件与外部世界
│   │   ├── FileReader.*        映射读取、分块读取
│   │   ├── FileWriter.*        原子保存、备份、只读处理
│   │   ├── XmlFile.*           XML 文件的读与写（把文件访问留在 io 层，避免 infra 反向依赖）
│   │   ├── FileWatcher.*       ReadDirectoryChangesW 监听
│   │   ├── FileScanner.*       目录遍历与过滤器匹配
│   │   └── PrintExport.*       打印、导出 HTML/RTF、复制为富文本
│   ├── ui/             通用 UI 设施
│   │   ├── Theme.*             主题与配色解析
│   │   ├── Dpi.*               感知模式下尺寸换算
│   │   ├── DarkMode.*          系统深色模式联动与子类化
│   │   ├── Controls/           自绘按钮、编辑框、列表控件
│   │   └── Dialogs/            首选项、查找替换、列块编辑、关于等
│   ├── infra/          基础设施
│   │   ├── ThreadPool.*        通用线程池
│   │   ├── TaskQueue.h         可取消任务
│   │   ├── Logger.*            分级别日志与滚动文件
│   │   ├── Paths.*             安装/便携目录解析
│   │   ├── Hash.* / Signature.* CNG 封装
│   │   └── Xml.*               最小 XML DOM
│   └── i18n/
│       └── Localization.*      语言包加载、切换、缺键回退
├── res/
│   ├── icons/          图标（多尺寸 ICO + 单色矢量位图）
│   ├── themes/         主题样式定义
│   ├── langs/          内置语言定义 langs.xml + 语言包 *.lang.xml
│   └── lexilla/        静态链接时的 lexer 注册表
├── plugins-sdk/        插件开发包（头文件 + 导入库 + 示例插件）
└── tests/
    ├── unit/           单元测试
    ├── integration/    集成测试
    └── perf/           性能基准
```

### 1.5 进程与线程模型

**进程模型**

- 默认**单实例多标签**。启动时通过命名互斥体 + 隐藏消息窗口（`CPPM_OPEN_FILES` 自定义消息）把文件路径转交给已有实例。
- 提供"多实例模式"开关（对应基线 2.2）。开启后每次启动都新建进程，配置互斥体与文件锁随之调整；配置写入采用最终写入者胜出策略，并对 session 文件加进程级锁。
- **不考虑跨位数共存**（ADR-01）。

**线程模型**

| 线程 | 数量 | 职责 | 禁止事项 |
|---|---|---|---|
| UI 线程 | 1 | 消息泵、窗口、全部 Scintilla 操作、命令执行 | 不做磁盘 IO、不做大范围文本扫描 |
| IO 线程池 | 2~4 | 文件读写、映射建立、保存、备份、目录遍历 | 不触碰任何窗口与 Scintilla |
| 搜索线程池 | 1~N（N = min(4, 逻辑核数-1)） | 跨文件扫描、正则匹配 | 不触碰编辑控件；结果通过 `PostMessage` 回传 |
| 自动更新线程 | 1 | 更新检查、下载、签名校验 | 不在 UI 线程做网络等待 |

**跨线程数据交换约定**

1. 需要后台线程处理文档内容时，先把目标区间拷贝成一份不可变的 `std::string`（或 `shared_ptr<const std::string>`）作为任务载荷，后台只读这份副本，**不允许后台线程持有并访问缓冲区指针**。
2. 后台向 UI 回传使用 `PostMessage(hwnd, WM_APP + n, ...)`，绝不使用 `SendMessage`，避免降级为同步等待而死锁。
3. 长任务必须支持取消：任务持有 `std::shared_ptr<std::atomic_bool> cancelFlag`，扫描循环按块（例如每 4 MB 或每 1000 个文件）检查一次。
4. 任务完成通知统一走 `NotificationBus`，由 UI 线程分发，保证监听者只在 UI 线程被调用。

### 1.6 性能预算分配

把 1.1.1 的目标拆到各阶段，避免后期"整体不达标但找不到瓶颈"。

| 场景 | 总预算 | 分配 |
|---|---|---|
| 冷启动 1.0 s | 进程与 CRT 初始化 80 ms；窗口与控件创建 250 ms；Scintilla 首次实例化 120 ms；配置与会话解析 150 ms；语言定义加载 200 ms；会话文件恢复（首批）200 ms |
| 打开 50 MB 文件 2.0 s | 文件打开与映射 30 ms；编码探测（首 256 KB）10 ms；Scintilla 装载 1.2 s；语法识别与 lexer 绑定 20 ms；视图呈现 100 ms；剩余为索引与 UI 更新 |
| 字面量查找首个匹配 300 ms（50 MB） | 目标定位与映射 10 ms；扫描 250 ms（按 ~200 MB/s 有效吞吐估算）；高亮与滚动 40 ms |

任何一项超出分配超过 50% 时，必须在提交评审中显式说明并给出优化计划。

---

## 2. 模块划分与职责

### 2.1 模块总表

下表与基线文档"五、功能清单总览速查表"的 14 个功能域一一对应，并给出承载模块、关键类与优先级。

| # | 功能域 | 承载模块 | 关键类 | 优先级 |
|---|---|---|---|---|
| M1 | 编辑核心 | `edit` | `EditView` `DocBuffer` `Folding` `Bookmark` | P0 |
| M2 | 文件与文档管理 | `core` `io` | `DocumentManager` `SessionManager` `FileWriter` `FileWatcher` | P0 |
| M3 | 视图与窗口 | `shell` `edit` | `ViewGroup` `MainWindow` `DockHost` | P0 |
| M4 | 查找与替换 | `search` | `InDocSearch` `CrossFileSearch` `RegexEngine` | P0 |
| M5 | 列编辑 / 块编辑 | `edit` | `ColumnEdit` | P0 |
| M6 | 语言与语法 | `lang` | `LexerRegistry` `UdlLoader` `StyleConfig` `FunctionList` `AutoComplete` | P0/P1 |
| M7 | 编码与换行处理 | `edit` | `Encoding` `EolMode` | P0 |
| M8 | 宏与自动化 | `macro` | `MacroRecorder` `MacroPlayer` `MacroStore` | P1 |
| M9 | 插件体系 | `plugin` | `PluginHost` `PluginAdmin` | P1/P2 |
| M10 | 界面与个性化 | `ui` `shell` | `Theme` `Dpi` `DarkMode` `Localization` | P1 |
| M11 | 设置 / 首选项 | `core` `ui` | `ConfigCenter` `CommandRegistry` | P0/P1 |
| M12 | 辅助工具与实用命令 | `core` `edit` | `RunCommand` `TextTools` | P1 |
| M13 | 打印与导出 | `io` | `PrintExport` | P1 |
| M14 | 系统与运维 | `infra` `app` | `Updater` `Paths` `Logger` | P2 |

### 2.2 M1 编辑核心

**职责**：提供文本编辑的一切基础能力，并把 Scintilla 的消息细节完全封闭在 `EditView` 之内。

**设计要点**

1. `EditView` 是**唯一**允许调用 `SendMessage(scintillaHwnd, SCI_*, ...)` 的类。其余代码只能调用 `EditView` 的语义化方法（`InsertText`、`GetLineText`、`Redo`、`GetSelection` 等）。这条约束的直接收益是：将来替换或升级编辑内核时改动面收敛在一个文件里，也让单元测试可以用假实现替换。
2. 撤销与重做交给 Scintilla 原生机制，不自行实现撤销栈；但需要配置撤销缓冲区（`SCI_SETUNDOCOLLECTION`、`SCI_EMPTYUNDOBUFFER`）以保证大文件下内存可控——超过阈值时缩短撤销深度并在状态栏以提示条告知用户。
3. 行操作（复制行、删除行、上下移动、合并行、拆分行）实现为**一组独立命令**并在 `CommandRegistry` 注册，快捷键只绑定命令 ID。这样"快捷键可重绑"和"宏可录制"天然成立。
4. 注释/取消注释按当前语言的 `CommentDef { linePrefix, blockStart, blockEnd }` 实现；UDL 语言同样提供该结构，从而获得一致的注释行为。
5. 括号匹配使用 `SCI_BRACEHIGHLIGHT` 系列；跳转匹配括号用 `SCI_BRACEMATCH`，失败时给出状态栏提示而非静默无响应。
6. 空白可视化（空格、制表符、行尾符）映射到 Scintilla 的 `VIEWWS` 与 `VIEWEOL`；每个开关都是独立命令，可绑定快捷键。

**对应基线 2.1 全部条目**。

### 2.3 M2 文件与文档管理

**职责**：生命周期管理——从"用户点开一个文件"到"这个文档被关闭且资源释放"的全部过程。

**设计要点**

1. `DocumentManager` 持有 `std::vector<std::unique_ptr<DocBuffer>>`，每个 `DocBuffer` 有稳定的 `BufferId`（单调递增整数，循环利用需显式审批）。标签顺序由独立的 `std::vector<BufferId> tabOrder` 维护，保证拖拽排序不影响缓冲区身份。
2. **未保存文档的统一处理**：新建而未保存的文档给予 `filePath = ""`，关闭前走统一的"是否保存"询问流程；崩溃恢复依赖于周期性的自动备份快照（见 6.9）。
3. **外部修改检测**（对应基线 2.2）：为每个已保存文档记录 `FileIdentity { volumeSerial, fileIndex, size, lastWriteTime }`。检测有两个触发源——`FileWatcher` 的目录变更通知（主动），以及在标签切换到前台、窗口重新获得焦点时的兜底校验（被动）。命中变更时弹出带"重新加载 / 保留当前 / 另存为 / 查看差异（若装 Compare 插件）"的对话框。
4. **只读处理**：文件属性为只读时仍允许打开编辑，保存时尝试清除只读属性（对应基线"一键清除只读标记"），并在保存失败路径上给出明确的权限提示，而不是笼统的"保存失败"。
5. **会话管理**（对应基线 2.2）：退出时写 `session.xml`；启动时按配置决定是否恢复。会话恢复采用两阶段——先恢复文件与视图骨架（快），再逐个恢复光标位置、书签、折叠状态（慢，可延后到空闲期），保证恢复过程不阻塞首屏。

   M1 已落地：退出时记录标签顺序、活动标签、每个文档的绝对路径、光标位置、首个可见行、缩放级别与**书签行号**；启动时不带文件参数才恢复，带参数则以参数为准（用户的显式意图优先）。**只记录有路径的文档**——未命名文档在退出时已被逐一询问过"保存还是丢弃"，重启再把它恢复出来会与用户当时的选择相矛盾。会话中的文件若已被删除或改名，跳过它继续恢复其余文档，不打断启动；若一个都恢复不了，则退回新建空白文档。

   两个必须注意的点：① 路径一律存**绝对路径**（用 `GetFullPathNameW` 规范化），否则换工作目录后会话就失效；② 打开文件时同样先规范化再比较，否则同一文件换个写法就会被当成两个文档，各自演化出不同内容——这比"多开一个标签"严重得多，是数据风险。

   更完整的两阶段恢复、书签与折叠状态、分屏布局、崩溃快照留待 M2（见 5.9）。
6. **便携模式判定顺序**：① 命令行 `-portable`；② 程序目录存在 `portable` 空标记文件；③ 程序目录下的 `config` 目录可写。三者任一成立即进入便携模式，配置根目录解析为 `Paths::ConfigRoot()`，全程序只通过该函数取配置路径，不出现第二处拼接。
7. **多实例模式**：进程内互斥体按"模式 + 配置根目录"生成名字，使便携版与安装版、不同配置目录的实例互不干扰。

### 2.4 M3 视图与窗口

**职责**：窗口布局、分屏、停靠面板与全屏等一切与"东西摆在哪儿"相关的能力。

**设计要点**

1. `ViewGroup` 管理 1~4 个视图槽位（主、次、第三、第四），本版实现**主/次双视图 + 可扩展槽位**，为基线中"分屏能力有限"这一短板预留超越空间（6.9 节）。
2. "移动到另一视图"（Move to other view）只是改变 `DocBuffer` 的 `ownerView`；"克隆到另一视图"（Clone）则新建一个视图并**共享同一 Scintilla 文档对象**，克隆视图只读。共享文档的刷新由 Scintilla 的文档引用计数机制负责，两视图在同一 UI 线程内，不存在并发问题。
   
   若实测发现共享文档在折叠/书签等视图态上互相干扰，降级方案为：克隆视图持有独立文档副本，通过 `SCI_SETREADONLY` 锁定，并在主视图保存时同步刷新副本内容。降级方案的触发条件与切换开关在实现阶段确定并记录到 ADR-07 的补充说明中。
3. **同步滚动**是视图组的属性，打开后任一视图的滚动事件按"首可见行号"对齐其他视图，不按像素对齐（行列结构不同时更符合直觉）。
4. 标签栏为自绘控件，支持：拖拽排序、拖出成新窗口、标签颜色分组（颜色存于会话文件）、锁定标签（禁止编辑与关闭）、多行/垂直标签布局（当标签数超过阈值时切换）。

   M1 已落地的部分：拖拽排序、中键关闭、右键菜单（关闭 / 关闭其他 / 关闭右侧 / 关闭全部 / 复制完整路径）、脏标记、宽度自适应与滚轮横向滚动、活动标签自动滚入视野。标签颜色分组与锁定标签留待 M2。

   **实现要点**：每个缓冲区独占一个 Scintilla 控件，切换标签只是 `ShowWindow(SW_SHOW/SW_HIDE)`。这一点很重要——撤销历史、滚动位置、折叠状态都保存在控件内部，若在切换时销毁重建就会全部丢失。相应地，`WM_DESTROY` 到达时子窗口已先被系统销毁，此时只能清空句柄记录（`EditView::Detach`），不能再向它们发送任何消息。

   **勾选态与脏标记的数据来源**：文本改动由 Scintilla 的 `SCN_SAVEPOINTLEFT/REACHED` 通知驱动，注意要按"发出通知的那个控件"定位到对应文档再更新标记——打开文件时会给尚在隐藏状态的控件 `SetText`，此刻活动文档还不是它，按活动文档更新就会记错对象。
5. `DockHost` 提供左、右、下三个停靠区与浮动窗口，面板包括：文件夹工作区、项目、函数列表、查找结果、运行命令输出、插件自定义面板。面板布局（位置、大小、可见性）随会话持久化。
6. 全屏模式（`F11`）为窗口级全屏，隐藏标题栏与菜单，`Esc` 退出；另提供"专注模式"（仅隐藏停靠面板与标签栏）作为差异化小功能。

### 2.5 M4 查找与替换

**职责**：编辑器最核心的生产力模块，也是本产品与竞品拉开差距的主要着力点。

**设计要点**

1. **三种搜索模式**（对应基线 2.4）统一建模为 `SearchMode { Literal, Escaped, Regex }`：
   - `Literal`：走 Scintilla 原生 `SCI_SEARCHINTARGET` 字面量路径，最快；
   - `Escaped`：先由 `LiteralSearcher` 把 `\n \r \t \xNN \uXXXX` 解析为实际字节序列，再走字面量路径（因此性能与普通模式一致）；
   - `Regex`：由 `RegexEngine`（PCRE2）编译与匹配，对单文档搜索采用"按块向前扫描 + 跨块边界重叠"的策略，块大小 1 MB，重叠区长度为 `max(patternMaxWidth, 4 KB)`，解决跨块匹配漏检问题。
2. **匹配选项**：区分大小写、全字匹配、`.匹配换行符`、仅在选区内查找，全部作为 `SearchConfig` 的字段。`.匹配换行符` 在 PCRE2 中通过 `PCRE2_DOTALL` 实现。
3. **匹配高亮**用 indicator 实现：为搜索结果分配专用 indicator 编号，使用 `SCI_INDICATORFILLRANGE` 批量填充，扫描过程中分批提交（每 2000 个匹配或每 50 ms 让出一次），保证 UI 不卡死。
4. **在文件中查找 / 替换**（`Ctrl+Shift+F/H`）由 `CrossFileSearch` 承担：目录遍历 + 过滤器（支持 `*.js;*.html` 分号分隔与 `!排除` 语法）+ 包含子目录开关。执行流程为：枚举候选文件 → 按大小与二进制特征筛除 → 并行分块扫描 → 结果按文件聚合流式回填到面板。
5. **可取消与可暂停**：任务面板提供进度（已扫描文件数 / 命中数 / 耗时）、暂停与取消按钮。取消语义为"立即停止产生新结果，保留已有结果"，与用户直觉一致。
6. **替换安全性**：跨文件替换默认走"先生成变更预览 + 确认 + 写回"三步，每个文件写回前做原子替换并生成 `.bak`（可配置）。这是本产品相对基线对象的一处体验强化——批量替换是不可逆操作，必须留后悔药。
7. **去重**：整行去重、删除重复行作为独立命令（`TextTools`），内部实现为一次排序索引 + 哈希集合，避免 O(n²) 比对。

**M1 已落地与暂缓的部分**

已实现：三种模式（普通走 Scintilla 原生搜索，扩展先解析转义再走字面量路径，正则走 PCRE2）、区分大小写、全字匹配、`.匹配换行`、循环查找（含"已回绕"提示）、计数、标记全部（indicator）、替换当前、全部替换（包在单个撤销单元内）、底部查找栏。

**在文件中替换**（`Ctrl+Shift+H`）已按 5.5 的安全流程落地：先生成逐文件的替换预览（一行一个文件、带勾选框，可逐条取消），用户确认后再写回。写回前有五道关卡：

1. **重新读取并校验文件身份**（大小 + 最后写入时间）。计划里刻意**不保存"预览时的文件内容"**，因此不会照着过期内容覆盖——生成预览之后文件被其他程序改过，该文件直接跳过。
2. **重算替换**，保证写回的内容与预览一致（文件没变时结果必然一致）。
3. **编码可表示性检查**：ANSI 文件在替换前做一次宽字符往返转换比对。`WideCharToMultiByte` 遇到无法表示的字符会**静默替换成 `?`** 而不报错，只看返回值判断不出来。不通过的文件整个跳过，绝不写回。
4. **保留原编码**：从原始字节重新判定一次 BOM 与 UTF-16 字节序，避免替换顺手把文件编码改掉。
5. **原子写回**（见 4.6）。单个文件失败不影响其余文件；可选生成 `.bak`。

批量改动属于敏感操作，用户的"确认/取消"会记进日志便于事后追溯。

暂缓到 M2 的两项，都属于**规模问题而非正确性问题**，这里写清楚以免被误当成已完成：

1. **正则匹配目前是整篇缓冲区的单次匹配**，没有实现 5.4 里按 1 MB 分块 + 重叠区扫描的方案。文件上限 64 MB 时单次拷贝与匹配尚在可接受范围；一旦支持更大的文件，或希望在长搜索中保持界面响应与可取消，就必须补上分块。同时也就没有做扫描过程的进度提示与取消。
2. **ANSI（GBK 等）文档的正则匹配退化为按字节处理**：模式串会按该代码页转成字节，字面量匹配因此是正确的，但带元字符的模式可能切到多字节字符的中间字节上，`\p{...}` 之类 Unicode 属性也不生效。彻底解决要对匹配区间做"转码 + 偏移映射"，安排在 M2。UTF-8 文档不受影响。

另外，替换的"当前"语义做了明确约束：**只有当前选区确实等于一个匹配项时才替换**，否则只查找并选中下一个。否则用户随手选中一段文字再点"替换"，那段文字会被直接改掉——这是会破坏用户内容的操作，宁可不动。

### 2.6 M5 列编辑 / 块编辑

**职责**：实现基线中标志性的矩形块编辑能力。

**设计要点**

1. 列选择依赖 Scintilla 的矩形选择模式（`SCI_SETSELECTIONMODE(SC_SEL_RECTANGLE)`）。鼠标 `Alt+拖动` 与键盘 `Alt+Shift+方向键` 均映射到该模式。
2. 列插入/删除的本质是**对每一行的同列区间做一次替换**。实现时先收集矩形覆盖的行区间与列区间，再按"从后往前"的顺序应用修改，避免前面的修改导致后面行号偏移。这一顺序约束必须写进代码注释，否则是很典型的隐性 bug 源。
3. 列块编辑对话框（`Alt+C`）支持：
   - 插入固定文本；
   - 插入数字序列：参数为 `起始值`、`步长`、`补零位数`、`进制`（十进制/十六进制）、`每 N 行重置`；
   - 序列号与文本混排（如 `item_\i\`，`\i\` 为序号占位符），这是基线未覆盖但实际高频的用法，作为差异化增强。
4. 列粘贴：当源与目标均为矩形选区时保持列对齐，行数不足时自动补行到所需数量（并记入撤销单元，一次撤销可回退整个粘贴）。

**M1 已落地的部分与实测发现**

已实现：`Alt+鼠标拖动`选矩形（需要显式打开 `SCI_SETMOUSESELECTIONRECTANGULARSWITCH`，Scintilla 默认是关的）、`Alt+Shift+方向键`扩展、虚拟空间（`SCVS_RECTANGULARSELECTION`，让选区能越过短行行尾）、`Alt+C` 列块编辑对话框（模板里的 `\i\` 占位符 + 起始值 / 步长 / 补零位数 / 进制 / 每 N 行重置，带实时预览）、列块删除、`Insert` 切换插入/改写并在状态栏显示。

三个从实测里得到的结论，写下来免得后人重踩：

1. **`SCI_SETSEL` 不保留矩形模式**。先 `SCI_SETSELECTIONMODE(SC_SEL_RECTANGLE)` 再 `SCI_SETSEL`，选区会退回流式（`SCI_SELECTIONISRECTANGLE` 变回 0）。建立矩形选区必须用 `SCI_SETRECTANGULARSELECTIONANCHOR/CARET`，它们自己会置模式。
2. **批量修改前必须先把所有行的位置取出来**。改一次文档就会清掉选区，之后再问"第 N 行的选区从哪开始"只能得到 -1。第一版就是这么错的：只有最后一行被改了，前面几行悄无声息地没动。
3. **矩形选区在 Scintilla 内部会被展开成"每行一条"的多个选区**（`SCI_GETSELECTIONS` 可以量到），Scintilla 的字符输入路径（`InsertCharacter`）正是靠这个实现"逐行插入"。

一处**没有完成端到端验证**的行为，如实记在这里：在矩形选区上直接打字会逐行插入，这条依赖 Scintilla 的输入路径，而界面回归脚本用跨进程合成的 `WM_CHAR` 复现不出真实键盘输入（实测只改到主选区那一行）。矩形选区本身是否正确建立（展开成 3 条）是有断言的，经由 `Alt+C` 对话框的列插入也是端到端验证过的（真填模板、真点插入、回读文本核对）。

### 2.7 M6 语言与语法

**职责**：把"这段文字是什么语言、该怎么着色、怎么折叠、怎么补全"这件事做对。

**设计要点**

1. **语言注册表**：`LexerRegistry` 维护 `LangId → { 显示名, 扩展名集合, Lexilla lexer 名称, 关键字表, 注释定义, 折叠规则 }`。内置语言定义存放于 `res/langs/langs.xml`，与代码解耦，新增语言不需要改代码。
2. **语言识别顺序**：① 会话或显式指定；② Shebang/首行特征（如 `#!/usr/bin/env python`）；③ XML/HTML 特征嗅探（看首 4 KB 是否以 `<?xml`、`<!DOCTYPE html` 开头）；④ 扩展名映射；⑤ 兜底为"普通文本"。识别结果写入状态栏，用户可手工切换（手工结果优先于自动结果，且随会话保存）。
3. **UDL（用户自定义语言）**：以 XML 定义，支持关键字分组（多组、多套样式）、注释符、字符串界定符、运算符、数字识别、转义字符、大小写敏感性、扩展名绑定。加载时把 UDL 定义**编译**为 Scintilla 的关键词表与属性字符串（`SCI_SETKEYWORDS`、`SCI_SETPROPERTY`），复用 Lexilla 的 `container` 或 `null` lexer 承载。这样 UDL 语言与内置语言在后续所有环节（折叠、注释、样式配置）行为对等。
4. **样式配置器**：按语言逐项调整前景色、背景色、字体、字号、粗体斜体下划线。数据模型为 `StyleDef { langId, styleIndex, fg, bg, font, size, attrs }`，持久化到 `stylers.xml`，可直接对应 Scintilla 的 `SCI_STYLESET*` 系列。
5. **函数列表**：由 lexer 驱动的结构提取（每种语言一个轻量提取器，多数可通过正则 + 大括号深度实现），输出树形模型给停靠面板。解析在后台线程执行，节流到停止输入 300 ms 后触发，避免每次按键都重解析。
6. **自动完成**：三级来源——文档内已出现词（最快）、API 词库（每种语言一份文本文件，可自定义）、插件提供的候选。触发方式可配（输入即触发 / `Ctrl+Space` 手动触发 / 输入 N 个字符后触发）。调用提示（CallTip）从 API 词库的括号说明中提取。

### 2.8 M7 编码与换行处理

**职责**：中文用户的高频刚需，也是本产品重点打磨的差异化区域。

**设计要点见 6.2 节完整算法说明**，此处只列职责边界与对外行为：

1. 内部存储约定：Scintilla 缓冲区保存**字节序列**，通过代码页机制解释显示。UTF-8 与 ANSI 系列（GBK/Big5/Shift-JIS 等）交给 Scintilla 的代码页；UTF-16LE/BE **由适配层转换为 UTF-8 后在控件中呈现**，保存时按文档记录的原始编码回写。
2. "以 XX 编码打开"与"转为 XX 编码"必须是两个明确区分的动作：前者只改解释方式（字节不变，可能直接修好乱码），后者重写字节（真正修复文件）。UI 上通过分区展示与二次确认防止误操作。
3. 状态栏持续显示当前编码与 BOM 状态，编码展示格式统一为 `UTF-8-BOM` / `UTF-8` / `ANSI(GBK)` / `UCS-2 LE` 等规范短名。
4. EOL 转换提供 CRLF / LF / CR 三向互转，同时提供"打开时按当前 EOL 规范统一"的选项；状态栏显示当前文档的 EOL 类型，混合行尾时显示为 `Mixed` 并给出"统一为…"的快捷入口。
5. 默认新建文档的编码与行尾可在首选项配置（对应基线"新建"分页）。

### 2.9 M8 宏与自动化

**职责**：把用户操作序列变成可重复执行的资产。

**设计要点**

1. **录制对象是命令流，不是键鼠事件**。`MacroRecorder` 监听 `CommandRegistry` 的执行事件与若干文本编辑事件，记录为 `MacroStep` 序列。相比录制原始键鼠事件，命令流方案对窗口位置、焦点、DPI 变化都不敏感，跨机器回放成功率高得多。
2. 录制期间状态栏显示 `REC` 指示，可随时停止。
3. **回放**支持指定重复次数（对应基线"重复执行 N 次"）。回放过程中任何一步失败（例如找不到匹配文本）默认立即停止并提示失败步骤序号；提供"跳过失败步骤继续"的宽松模式开关。
4. **持久化**：宏存储于 `shortcuts.xml`（与基线一致，便于用户从对标产品迁移），包含名称、步骤列表、绑定快捷键。支持导出/导入单个宏与全部宏。
5. 宏可绑定到快捷键、菜单项（宏菜单）与工具栏按钮。

### 2.10 M9 插件体系

**职责**：在不牺牲主程序稳定性和体积的前提下提供扩展生态。

**设计要点**

1. **ABI 设计**（ADR-06）：插件为 x64 DLL，导出三个 C 函数：

   ```c
   // plugins-sdk/notepadcc_plugin.h
   #define NPCC_PLUGIN_ABI_VERSION 1u

   typedef struct NpccHostServices {
       uint32_t size;                 // 本结构体字节数，宿主与插件双向校验
       uint32_t abiVersion;
       void*    hostCookie;
       /* --- 以下为函数指针表，只允许在尾部追加 --- */
       void     (*RegisterCommand)(void* hostCookie, const NpccCommandDesc*);
       void     (*AddDockPanel)(void* hostCookie, const NpccPanelDesc*);
       intptr_t (*ScintillaSend)(void* hostCookie, intptr_t viewId, uint32_t msg, uintptr_t w, intptr_t l);
       void     (*Notify)(void* hostCookie, uint32_t code, void* payload);
       void*    (*Alloc)(size_t);      // 宿主分配器，避免跨 CRT 释放
       void     (*Free)(void*);
       size_t   (*GetConfigPath)(void* hostCookie, wchar_t* buf, size_t cch);
   } NpccHostServices;

   typedef struct NpccPluginInfo {
       uint32_t abiVersion;
       const wchar_t* name;
       const wchar_t* version;
       const wchar_t* author;
       const wchar_t* description;
   } NpccPluginInfo;

   /* 导出符号（extern "C" __declspec(dllexport)） */
   const NpccPluginInfo* NpccPlugin_GetInfo(void);
   int  NpccPlugin_Init(const NpccHostServices* host);   // 0 = 成功
   void NpccPlugin_Shutdown(void);
   ```

2. **兼容性规则**：`NpccHostServices` 只允许在尾部追加字段，任何已有字段的类型、顺序、语义都不得变更；宿主与插件通过 `size` 与 `abiVersion` 双向校验，不匹配时拒绝加载并给出可读原因。跨模块内存一律使用 `Alloc`/`Free`（或 `CoTaskMemAlloc` 族），禁止 `new` 在插件、`delete` 在宿主。
3. **通知码**采用 `NPPN_*` 风格的稳定枚举（见 4.8），插件通过 `NpccNotificationSink` 接收。所有通知在 UI 线程发出。
4. `PluginHost` 在加载与调用插件入口时用 SEH（`__try/__except`）与 C++ 异常兜底包裹，捕获到崩溃时把该插件标记为故障、禁用并提示用户，而不带崩主程序（见 6.7 的局限说明）。
5. `PluginAdmin` 提供列表、启停、安装、卸载、更新。在线源采用签名索引文件；离线安装支持 `.zip` 与裸 `.dll`。安装目录固定为 `config/plugins/<PluginName>/`，保证便携迁移整体可用。
6. 首批兼容方向以基线 2.9 的插件清单为参照（Compare、JSON Viewer、XML Tools、NppExport、NppFTP、NppExec、Python Script、MIME Tools、MarkdownPreview、Explorer、Converter、Spell Checker、Function List、Customize Toolbar、AutoHide）。这些插件的原生版本与其原宿主 ABI 绑定，**不承诺二进制兼容**；本版的做法是随包提供功能等价的内置实现或原生重写版（Compare、JSON、XML、MIME、Explorer、Function List 优先内置化，见第 10 章）。

### 2.11 M10 界面与个性化

1. **主题系统**：`Theme` 定义一套命名颜色令牌（`windowBg`、`tabActiveBg`、`toolbarBg`、`editorFg`、`editorBg`、`accent`、`border`…），所有自绘控件只引用令牌不引用具体色值。内置"浅色 / 深色 / 深黑 / 高对比"四套，并允许用户基于任一套派生自定义主题。
2. **深色模式**：跟随系统时，通过 `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` 让标题栏与系统对话框进入深色；按钮、列表、滚动条等标准控件以子类化 + 主题 API（`SetWindowTheme(L"", L"DarkMode_Explorer")`）+ 自绘兜底三者结合处理。
3. **高 DPI**：清单声明 `PerMonitorV2`，所有尺寸以逻辑单位定义、经 `Dpi::Scale()` 换算；图标按 100%/125%/150%/200%/250%/300% 准备多尺寸资源；跨显示器拖动时响应 `WM_DPICHANGED` 重排布局并让 Scintilla 重新设置字号与边距宽度。
4. **字体**：按语言独立配置字体与字号，支持连字字体（Fira Code、JetBrains Mono 等）与渲染质量设置（`SCI_SETFONTQUALITY`），中文场景下额外提供"中英文分别指定字体"的能力（中文用等宽中文字体、西文用代码字体），这是基线未点明但中文用户实际很在意的一项。
5. **多语言界面**：语言包为 XML 键值对，键采用 `区域.控件.用途` 命名（如 `dialog.find.matchCase`）。切换语言无需重启（重建菜单与对话框文本即可）。内置简体中文、繁体中文、英文三套，便于社区扩充。
6. **工具栏与右键菜单定制**：工具栏按钮集合与顺序、菜单项显隐均由配置文件驱动，提供可视化定制界面。

### 2.12 M11 设置 / 首选项

1. `ConfigCenter` 是配置的唯一权威来源。所有配置项以强类型结构体 + 默认值表定义，加载时逐项校验（类型、范围、枚举合法性），非法值记为警告日志并回退默认值，绝不因为一个坏配置导致启动失败。这是"稳"的具体体现。
2. 配置分页与基线一致：常规、编辑、边距与符号、新建、备份、自动完成、高亮、打印、MISC、多实例、最近文件历史、文件关联、语言菜单、暗色模式。
3. **快捷键管理**：支持对主菜单命令、Scintilla 原生命令、宏、插件命令、运行命令分别绑定快捷键，并提供冲突检测（同一 `(修饰键, 键码)` 组合被多处占用时高亮并给出替换建议）。
4. **配置变更广播**：任何修改写入 `ConfigCenter` 后由 `NotificationBus` 广播 `NFY_CONFIG_CHANGED`，各模块自行响应（例如"显示符号"变更后更新所有视图）。禁止模块间直接调用对方的 `ApplyConfig`。
5. **持久化策略**：写盘采用"延迟合并 + 原子替换"——变更后 1 秒内合并写盘一次，程序退出与系统关机（`WM_QUERYENDSESSION`）时强制刷盘。

### 2.13 M12 辅助工具与实用命令

1. **行操作工具集**（`TextTools`）：升/降序排序（区分或忽略大小写）、按长度排序、删除空行、删除重复行（可选"仅保留首次出现"或"保留最后一次"）、反转行序、行尾补/去空白、行号增删。
2. **转换工具**：URL 编码/解码、Base64 编码/解码、HTML 实体转换、JSON/XML 格式化（内置实现，对应基线中本属插件的能力）、进制与大小写转换。
3. **运行命令（`F5`）**：支持内置变量展开
   `$(FULL_CURRENT_PATH)`、`$(CURRENT_DIRECTORY)`、`$(FILE_NAME)`、`$(NAME_PART)`、`$(EXT_PART)`、`$(CURRENT_WORD)`、`$(CURRENT_LINE)`、`$(NPP_DIRECTORY)`（为兼容保留，值为程序目录），以及编码/行尾占位。
4. **浏览器预览**：对 HTML/Markdown 直接调用默认或指定浏览器打开；Markdown 另提供内置实时预览面板（内置实现，不依赖插件）。
5. **十六进制查看**：内置只读十六进制视图（左侧偏移、中间十六进制、右侧 ASCII），支持跳转与选区联动；转换为可编辑模式时以副本打开，避免误改二进制文件。

### 2.14 M13 打印与导出

1. **打印**：使用 Win32 打印 API（`StartDoc`/`StartPage`）+ 自绘文本。分页算法按"行高 × 可用高度"计算，支持行号、页眉页脚（可插入文件名、路径、页码、日期）、黑白与彩色两种着色模式、页边距设置。打印预览复用同一渲染函数，保证所见即所得。
2. **导出 HTML**：把每个语法样式映射为 CSS 类，生成带 `pre` 容器与行号表格的独立 HTML 文件（可选择内嵌样式或外部样式表）。
3. **导出 RTF**：样式表映射为 RTF 颜色表与字体表，保留配色、粗体斜体、行号。
4. **复制为富文本**：向剪贴板同时写入 `CF_UNICODETEXT`（纯文本）与 Rich Text Format（`"Rich Text Format"` 注册格式），这样粘贴到记事本是纯文本、粘贴到 Word 是带色代码。

### 2.15 M14 系统与运维

1. **自动更新**：启动后延迟 30 秒在后台检查（可配置为不检查）。更新包为签名 ZIP，校验链为：HTTPS 下载 → SHA-256 比对 → 清单签名验证（Ed25519 或 RSA-2048，公钥硬编码于程序）→ Authenticode 校验可执行文件。任一步失败即中止并提示，不提供"仍然继续"的旁路。这是基线"安全与合规"卖点最直接的落地。
2. **代理支持**：复用 WinHTTP 的系统代理配置，并为插件下载单独提供手动代理设置项。
3. **绿色免安装**：便携版不写注册表、不写 `%APPDATA%`、不写 `%TEMP%`（临时文件放在配置目录下的 `tmp/`），卸载即删除目录。这一条需要在 CI 里用沙箱试用测试做验证。
4. **日志**：分 `Error / Warn / Info / Trace` 四级，默认记录 `Info` 以上，滚动保留最近 5 个文件各 4 MB。日志路径可在关于对话框中一键打开。

---

## 3. 核心数据结构

### 3.1 标识与句柄

```cpp
// core/Ids.h
using BufferId = int32_t;   // 文档缓冲区标识，-1 表示无效；0 起单调递增
using ViewId   = int32_t;   // 视图槽位标识，-1 表示无效；0 = 主视图，1 = 次视图
using LangId   = int32_t;   // 语言标识，0 = 普通文本；由 langs.xml 定义
using CmdId    = uint32_t;  // 命令标识，见附录 B 的分配规范
using MacroId  = int32_t;
constexpr int32_t kInvalidId = -1;
```

标识必须是**不透明整数**：任何代码不得假设 `BufferId == 标签索引`，也不得用 `ViewId` 直接索引容器。容器访问统一走 `DocumentManager::FindBuffer(BufferId)`。

### 3.2 文档与视图

```cpp
// edit/DocBuffer.h
enum class EolMode : uint8_t { Crlf, Lf, Cr, Mixed, Unknown };
enum class UniMode : uint8_t {
    Ansi,        // 以 codePage 指定的代码页解释，无 BOM
    Utf8,        // UTF-8 无 BOM
    Utf8Bom,     // UTF-8 带 BOM
    Utf16LE,     // UCS-2 / UTF-16 小端（以 BOM 或探测确定）
    Utf16BE,
    Utf16LE_NoBom,
    Utf16BE_NoBom
};

struct FileIdentity {
    uint32_t volumeSerial = 0;
    uint64_t fileIndex    = 0;   // NTFS FileId，用于识别"改名后仍是同一文件"
    uint64_t size         = 0;
    uint64_t lastWriteMs  = 0;   // FILETIME 转 100ns 单位后再折算
    bool     Valid() const noexcept { return volumeSerial != 0 && fileIndex != 0; }
};

struct EncodingState {
    UniMode  uniMode   = UniMode::Ansi;
    uint32_t codePage  = 0;          // 仅 Ansi 模式有效：936/950/932/1252...
    bool     hasBom    = false;
    bool     bomDetected = false;
    /* 探测置信度 0~100；低于阈值时在状态栏提示"编码可能不准确" */
    uint8_t  confidence = 100;
};

struct DocBuffer {
    BufferId  id          = kInvalidId;
    std::wstring filePath;           // 空串 = 未保存的新建文档
    std::wstring displayName;        // 标签显示名（文件名或"新建 N"）
    bool      isModified  = false;
    bool      isReadOnly  = false;   // 用户锁定标签
    bool      isDiskReadOnly = false;// 文件系统属性只读
    bool      isUntitled  = true;

    EncodingState encoding;
    EolMode   eol          = EolMode::Crlf;
    EolMode   defaultEol   = EolMode::Crlf;  // 探测得到的"原始"行尾
    LangId    langId       = 0;
    bool      langUserSet  = false;          // 用户手工指定过语言，自动识别不再覆盖

    FileIdentity lastKnown;                  // 上次与磁盘一致的快照
    ViewId    ownerView    = 0;              // 主归属视图
    std::vector<ViewId> cloneViews;          // 克隆视图（只读）
    bool      isLargeFile  = false;          // 处于大文件模式

    /* 视图态：随会话保存，双视图共享文档时以主视图为准 */
    struct ViewState {
        int32_t  caretPos      = 0;
        int32_t  anchorPos     = 0;
        int32_t  firstVisibleLine = 0;
        int32_t  xOffset       = 0;
        int32_t  zoomLevel     = 0;
        uint32_t wrapMode      = 0;          // 0 = 关闭，1 = 按窗口宽，2 = 按列宽
        uint32_t wrapColumn    = 0;
    } viewState;

    std::vector<int32_t> bookmarks;          // 升序行号
    std::vector<int32_t> collapsedLines;     // 折叠的头部行号
    std::optional<COLORREF> tabColor;        // 标签颜色分组
    std::chrono::system_clock::time_point backupAt;
};
```

视图槽位：

```cpp
// edit/ViewGroup.h
enum class SplitAxis : uint8_t { None, Vertical /* 左右*/, Horizontal /*上下*/ };

struct ViewSlot {
    ViewId   id      = kInvalidId;
    HWND     hwnd    = nullptr;    // 承载 Scintilla 的容器窗口
    HWND     sci     = nullptr;    // Scintilla 控件
    BufferId buffer  = kInvalidId; // 当前呈现的缓冲区
    bool     visible = false;
    RECT     rc      = {};
};

struct ViewGroup {
    SplitAxis axis = SplitAxis::None;          // 本版：None 或 Vertical
    std::array<ViewSlot, 4> slots;             // 索引 0/1 为主/次视图，2/3 预留
    ViewId   activeSlot = 0;
    bool     syncScrollVertical = false;
    int32_t  splitRatioPercent  = 50;
};
```

### 3.3 命令与快捷键

```cpp
// core/Command.h
enum class CmdCategory : uint8_t {
    File, Edit, Search, View, Format, Language, Settings,
    Macro, Run, Plugin, Window, Help
};

enum CmdFlags : uint32_t {
    kCmdNone          = 0,
    kCmdCheckable     = 1u << 0,  // 带勾选态（如"显示符号"）
    kCmdRecordable    = 1u << 1,  // 可被宏录制
    kCmdNeedsBuffer   = 1u << 2,  // 需要活动缓冲区，否则置灰
    kCmdNeedsSelection= 1u << 3,  // 需要非空选区，否则置灰
    kCmdNeedsWritable = 1u << 4,  // 需要可写缓冲区
    kCmdHidden        = 1u << 5   // 不在菜单显示（仅供快捷键/插件调用）
};

struct Command {
    CmdId        id        = 0;
    CmdCategory  category  = CmdCategory::Edit;
    std::wstring key;                 // 语言包键，如 "cmd.edit.duplicateLine"
    std::wstring defaultText;         // 语言包缺失时的回退文本
    uint32_t     flags     = kCmdNone;
    std::function<void(const CmdContext&)> execute;
    std::function<bool(const CmdContext&)>  queryEnabled;  // 可空
    std::function<bool(const CmdContext&)>  queryChecked;  // 可空
    /* 录制/回放用：把执行时的参数序列化，便于宏重放与插件调用 */
    std::function<std::string(const CmdContext&)> captureArgs;
    std::function<void(const std::string&)>       restoreArgs;
};

struct CmdContext {
    BufferId buffer = kInvalidId;
    ViewId   view   = kInvalidId;
    void*    userData = nullptr;   // 命令自定义参数（如"折叠到第 3 层"的层数）
};

struct ShortcutBinding {
    CmdId    cmdId = 0;
    uint8_t  modifiers = 0;   // MOD_CTRL / MOD_ALT / MOD_SHIFT / MOD_WIN 组合
    uint16_t keyCode   = 0;   // 虚拟键码
    uint8_t  scope     = 0;   // 0 = 全局，1 = 仅编辑器聚焦时
    bool     Valid() const noexcept { return cmdId != 0 && keyCode != 0; }
};
```

### 3.4 搜索

```cpp
// search/SearchConfig.h
enum class SearchMode : uint8_t { Literal, Escaped, Regex };
enum class SearchScope : uint8_t { CurrentBuffer, AllOpenBuffers, FilesInDirectory };

struct SearchConfig {
    std::wstring pattern;
    std::wstring replaceTo;
    SearchMode   mode           = SearchMode::Literal;
    SearchScope  scope          = SearchScope::CurrentBuffer;
    bool         matchCase      = false;
    bool         wholeWordOnly  = false;
    bool         dotMatchesNewline = false;
    bool         wrapAround     = true;
    bool         inSelection    = false;
    bool         useUnicodeProps = false;   // \p{...} 属性匹配，仅正则模式

    /* 跨文件场景 */
    std::vector<std::wstring> directories;
    std::wstring filter = L"*.*";           // 支持 "*.js;*.html" 与 "!node_modules"
    bool         includeSubDirs = true;
    bool         skipBinary     = true;
    uint64_t     maxFileSizeBytes = 64ull * 1024 * 1024;
};

struct MatchRange { int64_t start; int64_t end; };

struct FileSearchResult {
    std::wstring path;
    uint64_t     fileSize = 0;
    std::vector<struct LineHit> hits;   // 行号 + 行文本 + 命中列区间
    bool         truncated = false;     // 单文件命中过多被截断
    std::wstring error;                 // 读取失败原因
};
```

### 3.5 宏

```cpp
// macro/Macro.h
enum class MacroStepKind : uint8_t {
    Command,           // 执行某个命令（带参数）
    ScintillaMessage,  // 直接发送 SCI_* 消息（覆盖 Scintilla 原生动作）
    InsertText,        // 插入文本（批量输入合并为一步，避免逐字符记录）
    FindReplace,       // 查找/替换（参数完整保存）
    Delay              // 显式等待，用于需要时间的外部动作
};

struct MacroStep {
    MacroStepKind kind = MacroStepKind::Command;
    CmdId   cmdId   = 0;
    uint32_t sciMsg = 0;
    uintptr_t sciWParam = 0;
    intptr_t  sciLParam = 0;
    std::string params;      // 步骤参数（JSON 或自定义紧凑格式）
    uint32_t  delayMs = 0;
};

struct Macro {
    MacroId   id = 0;
    std::wstring name;
    std::vector<MacroStep> steps;
    std::optional<ShortcutBinding> binding;
    uint32_t  playCount = 1;      // 默认重复次数
};
```

### 3.6 语言与样式

```cpp
// lang/LexerRegistry.h
struct CommentDef {
    std::wstring linePrefix;        // 如 "//"；空表示不支持行注释
    std::wstring blockStart;        // 如 "/*"
    std::wstring blockEnd;          // 如 "*/"
    bool         nestedBlock = false;
};

struct LanguageDef {
    LangId       id = 0;
    std::wstring name;                 // 显示名
    std::wstring englishName;
    std::vector<std::wstring> extensions;   // {"cpp","hpp","h"}
    std::wstring lexerName;            // Lexilla lexer 名，如 "cpp"
    std::vector<std::vector<std::wstring>> keywordGroups; // 对应 SCI_SETKEYWORDS 的组序
    CommentDef   comment;
    bool         caseSensitive = true;
    bool         foldable = true;
    bool         isUdl = false;        // true = 来自用户自定义语言
};

// lang/StyleConfig.h
struct StyleDef {
    LangId   langId = 0;
    int      styleIndex = 0;   // Scintilla STYLE_*
    COLORREF fg = RGB(0, 0, 0);
    COLORREF bg = RGB(255, 255, 255);
    bool     fgUsed = false;   // 未设置时沿用主题默认，避免"改一处坏全局"
    bool     bgUsed = false;
    std::wstring font;         // 空 = 使用全局字体
    int      size = 0;         // 0 = 使用全局字号
    bool     bold = false, italic = false, underline = false;
};
```

### 3.7 会话与配置

```cpp
// core/SessionManager.h
struct SessionDoc {
    std::wstring filePath;
    bool      isUntitled = false;
    std::wstring untitledText;     // 未保存文档的内容快照（受限长度）
    LangId    langId = 0;
    ViewId    viewId = 0;
    EncodingState encoding;
    EolMode   eol = EolMode::Crlf;
    DocBuffer::ViewState viewState;
    std::vector<int32_t> bookmarks;
    std::vector<int32_t> collapsedLines;
    std::optional<COLORREF> tabColor;
    bool      isReadOnly = false;
    bool      isActiveTab = false;
};

struct SessionSnapshot {
    uint32_t schemaVersion = 1;
    std::chrono::system_clock::time_point savedAt;
    SplitAxis axis = SplitAxis::None;
    int32_t   splitRatioPercent = 50;
    bool      syncScrollVertical = false;
    std::vector<SessionDoc> docs;          // 顺序即标签顺序
    std::vector<std::wstring> recentFiles;
};
```

配置结构体按分页组织，每项必须有默认值，禁止裸字段无初值：

```cpp
// core/ConfigCenter.h
struct GeneralConfig {
    std::wstring uiLanguage = L"zh-CN";
    std::wstring themeName  = L"Light";
    bool  followSystemDarkMode = true;
    bool  restoreSessionOnStart = true;
    bool  checkUpdateOnStart = true;
    bool  portableMode = false;
    bool  multiInstance = false;
    uint32_t recentFileCount = 20;
    uint32_t maxRecentClosed  = 20;
};

struct EditConfig {
    bool  tabToSpaces = true;
    uint32_t tabWidth = 4;
    bool  autoIndent = true;
    bool  smartIndent = true;
    bool  autoCloseBrackets = false;
    bool  showIndentGuide = true;
    bool  showWhitespace = false;
    bool  showEol = false;
    bool  caretLineHighlight = true;
    bool  multiCursorEnabled = true;      // 差异化能力，见 6.9
    uint32_t undoDepth = 500;
    uint32_t largeFileUndoDepth = 20;
};

struct NewFileConfig {
    UniMode  defaultUniMode = UniMode::Utf8;
    uint32_t defaultCodePage = 936;
    EolMode  defaultEol = EolMode::Crlf;
};

struct BackupConfig {
    bool  enabled = true;
    uint32_t intervalSeconds = 30;
    uint32_t keepSnapshots = 10;
    bool  backupOnSave = false;     // 保存时生成 .bak
    std::wstring backupDir;         // 空 = 配置目录下 backup/
};
```

### 3.8 插件 ABI 结构

见 2.10 节的 `NpccHostServices` / `NpccPluginInfo` 定义。补充宿主侧运行时结构：

```cpp
// plugin/PluginHost.h
struct PluginEntry {
    std::wstring name;
    std::wstring version;
    std::wstring dllPath;
    std::wstring configDir;
    HMODULE      module = nullptr;
    bool         enabled = true;
    bool         failed  = false;      // 因崩溃或异常被禁用
    std::wstring failReason;
    const NpccPluginInfo* info = nullptr;
    std::vector<CmdId> ownedCommands;
    std::vector<int>   ownedPanels;
    PFN_NpccPlugin_Init     init = nullptr;
    PFN_NpccPlugin_Shutdown shutdown = nullptr;
};
```

### 3.9 通知与事件

```cpp
// core/NotificationBus.h
struct Notification {
    uint32_t code = 0;
    void*    payload = nullptr;   // 生命周期由发布者保证，仅在同一次同步分发内有效
    intptr_t lParam = 0;
};
using NotifySink = std::function<void(const Notification&)>;

class NotificationBus {
public:
    /* 返回订阅 token，用于退订；要求所有订阅与退订都在 UI 线程发生 */
    uint64_t Subscribe(uint32_t code, NotifySink sink);
    void     Unsubscribe(uint64_t token);
    void     Publish(const Notification& n);   // 同步分发；调用方仍在 UI 线程
    void     PostAsync(const Notification& n); // 拷贝 payload 后投递到 UI 线程队列
};
```

约定：`Publish` 为**同步**语义，订阅者不得在其中执行耗时操作（如需耗时则投递任务）。`PostAsync` 用于跨线程场景，只有它允许把 payload 的所有权转移给队列。

---

## 4. 接口定义

本节的接口为模块之间唯一的通信契约。除这些接口外，模块间不得直接 `#include` 对方的内部头文件。

### 4.1 IEditView（编辑视图）

```cpp
// edit/IEditView.h
class IEditView {
public:
    virtual ~IEditView() = default;

    /* --- 身份 --- */
    virtual ViewId   Id() const noexcept = 0;
    virtual HWND     ScintillaHandle() const noexcept = 0;
    virtual BufferId Buffer() const noexcept = 0;

    /* --- 文本读写（全部按字节偏移，编码解释在内部完成） --- */
    virtual std::string GetText(int64_t start, int64_t end) const = 0;
    virtual std::string GetAllText() const = 0;
    virtual int64_t     TextLength() const = 0;
    virtual int32_t     LineCount() const = 0;
    virtual std::string GetLine(int32_t line) const = 0;
    virtual int32_t     LineFromPosition(int64_t pos) const = 0;
    virtual int64_t     LineStart(int32_t line) const = 0;

    /* --- 编辑操作：所有写入必须走这里，保证撤销单元与录制一致 --- */
    virtual void ReplaceRange(int64_t start, int64_t end, std::string_view text) = 0;
    virtual void InsertText(int64_t pos, std::string_view text) = 0;
    virtual void Undo() = 0;
    virtual void Redo() = 0;
    virtual void BeginUndoAction() = 0;   // 复合操作包裹，保证一次撤销
    virtual void EndUndoAction() = 0;

    /* --- 选区与光标 --- */
    virtual MatchRange Selection() const = 0;
    virtual void       SetSelection(int64_t start, int64_t end) = 0;
    virtual void       ScrollToLine(int32_t line, bool center) = 0;

    /* --- 视图属性 --- */
    virtual void SetReadOnly(bool ro) = 0;
    virtual void SetLanguage(LangId lang) = 0;
    virtual void SetZoom(int32_t deltaSteps) = 0;
    virtual void SetWrapMode(uint32_t mode, uint32_t column) = 0;
    virtual void ToggleFold(int32_t line) = 0;
    virtual void FoldAll(int32_t level) = 0;   // level = -1 表示全部展开

    /* --- 指示器（搜索结果、标记、拼写等共用 16 个指示器槽位） --- */
    virtual void ClearIndicator(int indicatorId) = 0;
    virtual void MarkRange(int indicatorId, int64_t start, int64_t end) = 0;

    /* --- 逃生通道：仅供插件与迁移期使用，内部代码禁止调用 --- */
    virtual intptr_t SendScintillaRaw(uint32_t msg, uintptr_t w, intptr_t l) = 0;
};
```

**指示器槽位分配表**（全局固定，避免冲突）：

| 槽位 | 用途 |
|---|---|
| 0 | 查找高亮（当前匹配） |
| 1 | 查找高亮（全部匹配） |
| 2 | 智能高亮（当前词的其他出现） |
| 3 | 增量搜索 |
| 4 | 拼写检查错误 |
| 5 | 语法/标签对匹配 |
| 6~9 | 插件预留（按插件名哈希分配，最多 4 个活跃插件） |
| 10~15 | 未来扩展 |

### 4.2 IDocumentManager（文档管理）

```cpp
// core/IDocumentManager.h
enum class OpenResult {
    Ok, NotExist, AccessDenied, TooLarge, AlreadyOpen,
    EncodingFailed, Cancelled, InternalError
};

class IDocumentManager {
public:
    virtual ~IDocumentManager() = default;

    /* 打开：source = 磁盘路径 / 未命名文档 / 只读资源 */
    virtual std::pair<BufferId, OpenResult> Open(
        const OpenRequest& req) = 0;

    virtual bool Close(BufferId id, ClosePolicy policy) = 0;    // policy 决定是否询问保存
    virtual bool Save(BufferId id, const SaveOptions& opt) = 0;
    virtual bool SaveAll() = 0;
    virtual bool SaveAs(BufferId id, const std::wstring& newPath, const SaveOptions& opt) = 0;
    virtual bool Reload(BufferId id, bool discardChanges) = 0;

    virtual DocBuffer* Get(BufferId id) = 0;
    virtual std::vector<BufferId> AllBuffers() const = 0;
    virtual std::vector<BufferId> TabOrder() const = 0;
    virtual void  ReorderTabs(const std::vector<BufferId>& newOrder) = 0;
    virtual BufferId ActiveBuffer() const = 0;
    virtual void  Activate(BufferId id) = 0;

    virtual bool MoveToView(BufferId id, ViewId target) = 0;
    virtual bool CloneToView(BufferId id, ViewId target) = 0;

    virtual void  SetModified(BufferId id, bool modified) = 0;
    virtual bool IsAnyUnsaved() const = 0;
};

struct OpenRequest {
    std::wstring path;
    UniMode      forcedUniMode = UniMode::Ansi;   // 有效时跳过自动探测
    uint32_t     forcedCodePage = 0;
    LangId       forcedLangId = -1;
    bool         readOnly = false;
    bool         asHexView = false;
    bool         addToRecent = true;
    bool         restoreFromSession = false;
};

struct SaveOptions {
    std::optional<UniMode> convertToUniMode;   // 有值则同时转换编码（写坏可回退）
    std::optional<EolMode> convertToEol;
    bool keepBackup = false;
    bool writeBom   = true;                    // 仅在 Utf8Bom / Utf16 下生效
};

enum class ClosePolicy { AskIfModified, DiscardChanges, SaveThenClose };
```

### 4.3 ICommandRegistry（命令注册表）

```cpp
// core/ICommandRegistry.h
enum class ExecResult { Ok, Disabled, NotFound, Failed, Unsupported };

class ICommandRegistry {
public:
    virtual ~ICommandRegistry() = default;

    virtual CmdId Register(Command cmd) = 0;         // 返回分配的命令 ID
    virtual bool  Unregister(CmdId id) = 0;          // 插件卸载时调用
    virtual Command* Find(CmdId id) = 0;
    virtual std::vector<CmdId> ByCategory(CmdCategory c) const = 0;

    virtual ExecResult Execute(CmdId id, const CmdContext& ctx) = 0;
    /* 供 UI 刷新使用：置灰/勾选状态集中查询，避免各处重复判断 */
    virtual bool IsEnabled(CmdId id, const CmdContext& ctx) const = 0;
    virtual bool IsChecked(CmdId id, const CmdContext& ctx) const = 0;
    virtual std::wstring DisplayText(CmdId id) const = 0;   // 含加速键提示

    /* 快捷键 */
    virtual bool BindShortcut(CmdId id, const ShortcutBinding& b, bool replace) = 0;
    virtual std::vector<ShortcutBinding> BindingsOf(CmdId id) const = 0;
    virtual std::vector<CmdId> FindConflict(const ShortcutBinding& b) const = 0;
    virtual CmdId ResolveShortcut(uint8_t mods, uint16_t vk) const = 0;
};
```

模板注册辅助函数（减少样板）：

```cpp
template <class Fn>
CmdId RegisterCommand(ICommandRegistry& reg, CmdId id, CmdCategory cat,
                      std::wstring key, uint32_t flags, Fn&& fn);
```

### 4.4 ISearchService（搜索服务）

```cpp
// search/ISearchService.h
class ISearchJob {
public:
    virtual ~ISearchJob() = default;
    virtual void Cancel() = 0;
    virtual bool IsCancelled() const noexcept = 0;
    virtual void Pause()  = 0;
    virtual void Resume() = 0;
    virtual SearchProgress Progress() const = 0;
};

struct SearchProgress {
    uint64_t filesScanned = 0;
    uint64_t filesTotal   = 0;
    uint64_t hits         = 0;
    uint64_t bytesScanned = 0;
    std::wstring currentFile;
    bool     finished = false;
};

class ISearchService {
public:
    virtual ~ISearchService() = default;

    /* 单文档：同步执行，直接操作视图（必须从 UI 线程调用） */
    virtual std::optional<MatchRange> FindNext(IEditView& view, const SearchConfig& cfg,
                                               int64_t fromPos, bool forward) = 0;
    virtual int  CountMatches(IEditView& view, const SearchConfig& cfg) = 0;
    virtual int  MarkAll(IEditView& view, const SearchConfig& cfg) = 0;
    virtual int  ReplaceAll(IEditView& view, const SearchConfig& cfg,
                            bool inSelectionOnly) = 0;

    /* 跨文件：异步任务，UI 线程订阅结果流 */
    virtual std::shared_ptr<ISearchJob> FindInFiles(
        const SearchConfig& cfg,
        std::function<void(const FileSearchResult&)> onFileDone,   // UI 线程回调
        std::function<void(const SearchProgress&)>   onProgress,
        std::function<void(bool /*ok*/, const std::wstring& /*err*/)> onFinish) = 0;

    virtual std::shared_ptr<ISearchJob> ReplaceInFiles(
        const SearchConfig& cfg,
        const ReplaceSafetyOptions& safety,
        std::function<void(const ReplaceReport&)> onFinish) = 0;

    /* 正则预校验：返回错误信息（供查找对话框实时提示） */
    virtual std::optional<std::wstring> ValidatePattern(const SearchConfig& cfg) = 0;
};

struct ReplaceSafetyOptions {
    bool previewBeforeApply  = true;   // 默认开启：先出预览
    bool keepBackup          = false;
    bool abortOnFileError    = false;
    uint64_t maxReplacements = 200000; // 防止正则失误造成大规模改坏
};
```

### 4.5 IConfigStore / IFileWatcher / IFileWriter

```cpp
// core/IConfigStore.h
class IConfigStore {
public:
    virtual ~IConfigStore() = default;
    virtual bool Load() = 0;
    virtual bool Save(bool force = false) = 0;   // 合并延迟写盘；force 立即落盘
    virtual std::filesystem::path Root() const = 0;
    virtual bool IsPortable() const noexcept = 0;

    /* 强类型访问；写入后自动发布 NFY_CONFIG_CHANGED */
    virtual GeneralConfig& General() = 0;
    virtual EditConfig&    Edit() = 0;
    virtual NewFileConfig& NewFile() = 0;
    virtual BackupConfig&  Backup() = 0;

    /* 任意扩展键（插件与实验特性使用），值为字符串 */
    virtual std::optional<std::wstring> GetString(std::wstring_view path) const = 0;
    virtual void SetString(std::wstring_view path, std::wstring_view value) = 0;
};

// io/IFileWatcher.h
class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;
    virtual void Watch(const std::wstring& path) = 0;      // 监视其所在目录
    virtual void Unwatch(const std::wstring& path) = 0;
    virtual void SetCallback(std::function<void(const std::wstring& path, FileChangeKind)> cb) = 0;
    virtual void Rescan() = 0;   // 窗口重新激活时兜底全量校验
};

enum class FileChangeKind { Modified, Deleted, Renamed, Replaced };
```

### 4.6 保存接口的原子性契约

```cpp
// io/IFileWriter.h
struct WriteResult {
    bool ok = false;
    bool diskReadOnly = false;
    bool accessDenied = false;
    bool diskFull     = false;
    std::wstring message;
};

class IFileWriter {
public:
    virtual ~IFileWriter() = default;
    /* 契约：失败时不改变原文件内容（原子性）；成功后更新 DocBuffer.lastKnown */
    virtual WriteResult WriteAtomic(const std::wstring& path,
                                    std::string_view bytes,
                                    const WriteOptions& opt) = 0;
};
```

`WriteAtomic` 的实现步骤（这是全程序最重要的一段 IO 代码，必须严格按序）：

1. 读取目标文件当前属性，若为只读且配置允许，`SetFileAttributes` 清除只读；失败则返回 `diskReadOnly`。
2. 在**同一目录**创建临时文件 `.<name>.cpptmp`（同目录才能保证 `MoveFileEx` 是重命名而非跨卷复制）。
3. 写入全部字节并 `FlushFileBuffers`。
4. 若开启备份，先把原文件重命名为 `<name>.bak`（或复制）。
5. `ReplaceFile` 优先，失败则退化为 `MoveFileEx(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`。
6. 失败时删除临时文件；任何一步失败都不得留下半截目标文件。
7. 成功后重新读取目标文件的 `FileIdentity` 写入 `DocBuffer.lastKnown`。

### 4.7 IPlugin（宿主侧接口）

```cpp
// plugin/IPluginHost.h
class IPluginHost {
public:
    virtual ~IPluginHost() = default;
    virtual bool LoadAll() = 0;
    virtual bool Load(const std::wstring& dllPath, std::wstring* err) = 0;
    virtual bool Unload(std::wstring_view name) = 0;
    virtual bool SetEnabled(std::wstring_view name, bool enabled) = 0;
    virtual std::vector<PluginEntry> List() const = 0;
    virtual void NotifyAll(uint32_t code, void* payload) = 0;
    /* 提供给插件的宿主服务表（由宿主填充，指针在宿主生命周期内稳定） */
    virtual const NpccHostServices* Services() const noexcept = 0;
};
```

### 4.8 通知码表

通知码一旦发布即冻结，不得复用于其他语义；新增只能追加。

| 码 | 名称 | payload | 触发时机 |
|---|---|---|---|
| 1001 | `NFY_FILE_OPENED` | `const OpenedInfo*` | 文档成功打开 |
| 1002 | `NFY_FILE_CLOSED` | `const ClosedInfo*` | 文档关闭（含是否已保存） |
| 1003 | `NFY_FILE_SAVED` | `const SavedInfo*` | 保存成功 |
| 1004 | `NFY_FILE_BEFORE_SAVE` | `const BeforeSaveInfo*` | 保存前（内容已定稿，可取消） |
| 1005 | `NFY_FILE_RENAMED` | `const RenamedInfo*` | 另存为/重命名 |
| 1006 | `NFY_FILE_EXTERNALLY_CHANGED` | `const ChangedInfo*` | 检测到磁盘变化 |
| 1007 | `NFY_MODIFIED_CHANGED` | `const ModChangedInfo*` | 修改标记变化 |
| 1051 | `NFY_LANG_CHANGED` | `const LangChangedInfo*` | 语言模式切换 |
| 1052 | `NFY_ENCODING_CHANGED` | `const EncChangedInfo*` | 编码或 BOM 变化 |
| 1053 | `NFY_EOL_CHANGED` | `const EolChangedInfo*` | 行尾模式变化 |
| 1101 | `NFY_TAB_ACTIVATED` | `const TabInfo*` | 活动标签切换 |
| 1102 | `NFY_VIEW_LAYOUT_CHANGED` | `const LayoutInfo*` | 分屏/停靠布局变化 |
| 1201 | `NFY_CONFIG_CHANGED` | `const ConfigChangedInfo*` | 任意配置项写入 |
| 1202 | `NFY_SHORTCUT_CHANGED` | `const ShortcutChangedInfo*` | 快捷键重绑 |
| 1203 | `NFY_THEME_CHANGED` | `const ThemeChangedInfo*` | 主题切换 |
| 1204 | `NFY_LANGUAGE_CHANGED` | `const UiLangInfo*` | 界面语言切换 |
| 1301 | `NFY_BUFFER_ACTIVATED` | `const BufInfo*` | 编辑器获得焦点所在缓冲区变化 |
| 1302 | `NFY_BUFFER_UPDATED` | `const BufUpdateInfo*` | 文本内容变化（节流后发出） |
| 1401 | `NFY_MACRO_START_RECORD` / `1402 STOP` | `nullptr` | 宏录制 |
| 1501 | `NFY_PLUGIN_LOADED` / `1502 UNLOADED` / `1503 FAILED` | `const PluginInfoEx*` | 插件生命周期 |
| 1601 | `NFY_SEARCH_JOB_PROGRESS` | `const SearchProgress*` | 跨文件搜索进度 |
| 1602 | `NFY_SEARCH_JOB_FINISHED` | `const SearchJobResult*` | 跨文件搜索结束 |
| 1701 | `NFY_APP_READY` | `nullptr` | 首屏就绪、全部服务可用 |
| 1702 | `NFY_APP_EXITING` | `nullptr` | 退出流程开始（最后一次保存机会） |

`1302 NFY_BUFFER_UPDATED` 必须节流（默认 200 ms 合并一次），否则状态栏字符统计与函数列表会被逐键触发拖垮性能。

---

## 5. 关键流程设计

### 5.1 冷启动流程

目标：1 秒内出现可输入窗口。为此把"必须"和"可以稍后"的工作分开。

```
[UI 线程]
 1. wWinMain → 设置 DPI 感知 → 初始化 COM（STA，供打印/Shell 用）
 2. Paths::Init()            解析程序目录、判定便携模式、确定配置根       < 5 ms
 3. 单实例检查                 有实例则转交文件参数并退出                   < 10 ms
 4. Logger 初始化              打开日志文件（失败则降级为无日志，不阻塞）
 5. 注册窗口类、创建主窗口      仅创建外壳与标签栏，不创建 Scintilla 控件    < 250 ms
 6. ConfigCenter::Load()      解析全部分页配置，校验并回退非法值            < 150 ms
 7. CommandRegistry 构建      注册全部内置命令 + 绑定快捷键                < 30 ms
 8. 创建停靠面板容器           不加载面板内容
 9. ShowWindow(SW_SHOW) + 更新  ★ 用户此刻已看到窗口并可操作菜单
10. 发布 NFY_APP_READY
--- 以下为首屏之后的异步工作 ---
11. [后台] 会话文件解析 → 得 SessionSnapshot
12. [UI]   按快照逐个打开文档（前 N=3 个同步，其余进入延迟队列）
13. [UI]   恢复活动标签、光标、滚动、书签            ★ 首屏恢复完成
14. [后台] 语言与主题资源完整加载、函数列表解析、更新检查（延迟 30 s 启动）
```

关键设计点：

- **Scintilla 控件按需创建**。启动时零文档则一个控件都不建，冷启动体积与时间显著优于"预先建 6 个空标签"的做法。
- **配置解析失败不阻塞启动**。任何一项读失败都回退默认值并记日志，不弹模态框。
- **会话恢复分两级**：级 1 恢复文件与视图（可见结果），级 2 恢复书签、折叠、滚动偏移（空闲时补完）。用户感知到的是"马上打开了"。
- 若命令行带文件参数，则该文件插入级 1 的首位，优先于会话恢复。

### 5.2 打开文件流程

```
OpenRequest
   │
   ├─ 1. 路径规范化 + 去重检查（已打开则直接激活对应标签，返回 AlreadyOpen）
   ├─ 2. 文件属性查询：不存在 → NotExist；无读权限 → AccessDenied
   ├─ 3. 体积判定
   │       ≤  8 MB  → 常规路径（一次性读取到内存）
   │       ≤ 64 MB  → ILoader 分块装载（大文件模式）
   │        > 64 MB → 询问用户："只读浏览 / 仍然编辑 / 取消"
   ├─ 4. 编码探测（见 6.2）：读首 256 KB
   │       · BOM 命中 → 直接确定，confidence = 100
   │       · 无 BOM   → UTF-8 合法性校验 + 启发式统计 → 得 UniMode 与置信度
   │       · 用户显式指定 → 跳过探测
   ├─ 5. 行尾探测：统计首个 1 MB 中 CRLF/LF/CR 的数量
   │       单一类型占 ≥ 99% → 该类型；否则 Mixed（默认按 CRLF 显示）
   ├─ 6. 创建 DocBuffer（分配 BufferId，记录 FileIdentity）
   ├─ 7. 创建/复用目标视图的 Scintilla 控件，设置代码页与文档
   ├─ 8. 语言识别（见 2.7）→ 绑定 lexer → 应用样式
   ├─ 9. 大文件模式判定：命中则关闭语法着色、折叠、自动换行、词法高亮，
   │       状态栏显示"大文件模式"并提供"仍要启用语法着色"的入口
   ├─ 10. 加入标签栏、更新最近文件、加入 FileWatcher
   └─ 11. 发布 NFY_FILE_OPENED
```

错误处理约定：第 3、4 步之后的任何失败都必须**回滚已分配的资源**（销毁控件、释放 BufferId、从最近文件移除），不允许出现"打开失败但留下一个空标签"的中间态。

### 5.3 保存流程

```
Save(bufferId, options)
   │
   ├─ 1. 前置校验：文档未修改且无编码/EOL 转换请求 → 直接返回成功（不写盘）
   ├─ 2. 冲突检测：重新读取磁盘 FileIdentity
   │        与 lastKnown 不一致 → 弹出"文件已被外部修改"对话框
   │            · 覆盖磁盘 / 另存为 / 取消
   │        一致 → 继续
   ├─ 3. 内容定稿：发布 NFY_FILE_BEFORE_SAVE（插件可注入最后修改，
   │        如"保存时自动去除行尾空白"）
   ├─ 4. 编码处理
   │        · 当前为 UTF-8 / ANSI → 直接取出缓冲区字节
   │        · 当前为 UTF-16  → 从内部 UTF-8 转回原字节序，并保证 BOM
   │        · 请求了编码转换 → 先按目标编码转码，转码失败则终止并提示
   │        · 目标编码无法表示某些字符 → 提示"存在无法转换的字符"，
   │          给出"以 UTF-8 保存 / 用 ? 替换 / 取消"三个选项
   ├─ 5. EOL 处理：若文档为 Mixed 或请求了转换，按目标行尾重写
   ├─ 6. FileWriter::WriteAtomic（见 4.6 的七步契约）
   ├─ 7. 成功：更新 lastKnown、清除修改标记、发布 NFY_FILE_SAVED
   └─ 8. 失败：按 WriteResult 给出精确原因（只读 / 占用 / 权限 / 空间不足），
           并提供"重试 / 另存为 / 取消"；文档修改标记保持不变
```

补充设计：

- "占用中"是最常见的保存失败（文件被 Excel、日志查看器打开）。处理策略是先尝试一次写入，失败后提示"文件正被其他程序占用，是否另存为副本？"，而不是让用户反复点保存。
- 编码转换是**高风险操作**，UI 上在转换前对文件同步生成一份 `<name>.bak`（可在首选项关闭），这是对"编码转换可能写坏文件"这一真实场景的兜底。

### 5.4 查找与替换流程（单文档）

```
FindNext(view, cfg, fromPos, forward)
   │
   ├─ 模式判定
   │   ├─ Literal  → SCI_SETTARGETRANGE(from, end) + SCI_SEARCHINTARGET
   │   │             命中 flags: SCFIND_MATCHCASE | SCFIND_WHOLEWORD
   │   ├─ Escaped  → LiteralSearcher 解析转义为字节串，再走 Literal 路径
   │   └─ Regex    → RegexEngine::Compile(cfg) → 分块扫描
   │
   ├─ Regex 扫描细节（这是性能与正确性的核心）
   │   · 块大小 1 MB；块起点取 "上一个换行符之后的第一个字节"，
   │     避免把多字节字符或行结构切断
   │   · 相邻块保留 overlap = max(最长可能匹配长度, 4 KB) 的重叠区
   │   · 用 PCRE2 的 partial-match 能力识别"匹配跨越块尾"的情况，
   │     若命中跨块则把窗口向前扩展一个块再试，最多重试 2 次
   │   · 每次 match 调用传入 match_limit / depth_limit，
   │     超出则终止并提示"正则过于复杂，请简化或限制搜索范围"
   │   · 扫描循环每 16 ms 检查取消标志与消息队列，保证可中断
   │
   ├─ 命中：SetSelection + ScrollToLine(center) + 更新 indicator 0
   ├─ 未命中且 wrapAround：从头再来一次，避免死循环（限一轮）
   └─ 未命中且不循环：状态栏提示"未找到"，不弹模态框
```

`ReplaceAll` 的数据流：

1. **先统计后替换**：先跑一遍计数（这也是"替换全部"对话框里 `X 个匹配` 的来源），用户确认后才真正修改。
2. 修改以单个撤销单元包裹（`BeginUndoAction`/`EndUndoAction`），`Ctrl+Z` 一次可全部回退。
3. 替换顺序为**从后往前**，避免偏移量失效；这比"重新搜索下一个匹配"快得多。
4. 正则替换支持捕获组 `$1`~`$9`、`${name}`、`$0`（整体匹配）、`\n \t \uXXXX` 转义；PCRE2 的替换接口直接支持，但需注意 `$` 转义规则与对标产品的一致性，此处按"`$` 需写成 `$$`"处理并在帮助中说明。
5. 超过 `maxReplacements`（默认 20 万）时中止并提示，防止正则误配导致灾难性修改。

### 5.5 在文件中查找 / 替换流程

```
FindInFiles(cfg, callbacks)
   │
   ├─ UI 线程：参数校验（目录存在、正则可编译）→ 创建 FindResultsDock → 禁用重复发起
   ├─ 后台：FileScanner 枚举（BFS，尊重 filter 与 includeSubDirs）
   │        · 跳过目录：.git .svn node_modules __pycache__（可配置）
   │        · 跳过"隐藏/系统"属性的文件（可配置）
   │        · 按扩展名白名单/黑名单预筛
   ├─ 后台：文件级处理（线程池并行，粒度 = 单个文件）
   │        1) 打开并读取（映射或分块）
   │        2) 二进制判定：前 8 KB 内出现 NUL 字节或不可打印字符比例 > 30% → 跳过
   │        3) 编码判定：BOM → 确定；否则按 UTF-8 合法性 → 决定按 UTF-8 或按
   │           本地代码页解码为 UTF-16 供正则匹配（匹配在 UTF-16 域进行，
   │           保证中文与 Unicode 类的正确性），同时保留字节偏移映射以回写
   │        4) 逐行匹配，收集命中行（行号 + 行文本 + 列区间）
   │        5) 单文件命中超过 5000 条 → 截断并标记，避免结果面板被撑爆
   ├─ 回传：每完成一个文件 PostMessage(NFY_SEARCH_JOB_PROGRESS + 结果)
   │        结果面板增量插入，保持已展开/折叠状态
   └─ 结束：NFY_SEARCH_JOB_FINISHED（含总数、耗时、跳过文件数与原因统计）
```

跨文件替换在查找基础上追加：

1. `previewBeforeApply = true` 时先生成"文件 → 替换前后对照"的预览列表，用户可逐条取消勾选。
2. 逐个文件按 4.6 的原子契约写回；每个文件写回**独立**并立即报告结果（成功/失败原因），不做"全部成功才提交"的事务。
3. 编码与行尾与原文件保持一致；若目标编码无法表示替换后的字符，该文件记为失败并保留原文件。
4. 结束时给出汇总报告（成功 N、失败 M、跳过 K），失败项可一键定位。

### 5.6 列块编辑数字序列流程

```
用户：Alt+拖动 选择矩形区域 → Alt+C 打开列块编辑对话框
   │
   ├─ 1. 读取矩形选区：由 SCI_GETSELECTIONSTART/END + SC_SEL_RECTANGLE 推得
   │       (起始行, 结束行, 起始列, 结束列)
   ├─ 2. 用户配置：文本模板（可含 \i\ 占位符）、起始值、步长、进制、
   │       补零位数、每 N 行重置
   ├─ 3. 预生成待插入字符串列表（长度 = 行数），支持在对话框中预览前 20 行
   ├─ 4. 应用（关键顺序约束）：
   │       for (line = 末行; line >= 起始行; --line)
   │           在 (line, 起始列..结束列) 区间替换为该行的待插入文本
   │       ※ 必须从后往前，否则前序修改会使后续行号/列号失效
   ├─ 5. 整个过程包裹在单个撤销单元中
   └─ 6. 若"补零位数"小于实际数字宽度，不截断，只提示一次
```

### 5.7 宏录制与回放流程

```
录制
  1. 用户触发 CMD_MACRO_START_RECORD → 状态栏显示 REC
  2. 打开一个"录制上下文"：设置 CommandRegistry 的录制钩子
  3. 每次命令执行：若命令带 kCmdRecordable，则追加 MacroStep{Command, cmdId, captureArgs}
  4. 文本输入：以 300 ms 空闲合并为一次 InsertText 步骤（避免逐字符膨胀）
  5. Scintilla 原生快捷键（如 Ctrl+左移按词移动）若未映射到命令，
     则在窗口消息层捕获并记录为 ScintillaMessage 步骤
  6. 用户停止录制 → 弹出命名对话框（默认名 "Macro N"）→ 保存到 MacroStore

回放
  1. 取宏，按 playCount 重复
  2. 逐步骤执行：
     · Command       → Execute(cmdId, restoreArgs(params))
     · ScintillaMsg  → view->SendScintillaRaw(msg, w, l)
     · InsertText    → view->InsertText(caret, text)
     · FindReplace   → ISearchService 对应方法
  3. 每步执行前检查是否被用户中断（Esc 或点击停止按钮）
  4. 失败处理：默认立即停止并报告"第 N 步失败"，宽松模式则跳过继续
```

### 5.8 插件加载与调用流程

```
宿主启动（在 NFY_APP_READY 之后，避免拖慢首屏）
  1. 枚举 config/plugins/*/  与 程序目录/plugins/*/
  2. 读取每个插件目录的 plugin.json（可选，含名称/版本/禁用标记/依赖）
  3. 安全校验（6.7）：PE 位数 = x64 → 哈希与白名单比对（首次加载需用户确认）
  4. LoadLibraryEx(dll, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
  5. GetProcAddress 取 NpccPlugin_GetInfo / Init / Shutdown
  6. 校验 ABI：info->abiVersion == NPCC_PLUGIN_ABI_VERSION，
     且 host.size 不小于插件编译时的 sizeof(NpccHostServices)
  7. __try { rc = NpccPlugin_Init(&g_hostServices); } __except(...)
     · 崩溃 → 标记 failed、记录 failReason、继续加载下一个插件
  8. 插件在 Init 中通过 RegisterCommand / AddDockPanel 注册能力
  9. 发布 NFY_PLUGIN_LOADED

调用
  · 命令：CmdContext 中的 buffer/view 由宿主填充，插件不直接持有 HWND
  · 编辑器访问：统一走 ScintillaSend(hostCookie, viewId, msg, w, l)
  · 事件：宿主在 UI 线程逐个 NotifyAll(code, payload)
  · 定时：插件通过宿主提供的 SetTimerEx 注册，避免自行创建线程

卸载
  1. 发布 NFY_PLUGIN_UNLOADED 之前先 Unregister 该插件注册的全部命令与面板
  2. 调用 NpccPlugin_Shutdown（同样以 SEH 包裹）
  3. FreeLibrary；若插件创建过窗口，强制 DestroyWindow 后再释放
```

### 5.9 会话保存与崩溃恢复

**正常退出**：`WM_CLOSE` → 询问未保存文档 → 发布 `NFY_APP_EXITING`（插件落盘）→ 写 `session.xml` → 写配置 → 写最近文件 → 退出。

**崩溃恢复**：`BackupConfig.enabled` 时，后台按 `intervalSeconds` 扫描所有 `isModified` 的文档，把内容快照写入 `config/backup/<bufferId>@<timestamp>.bak`，并更新 `config/backup/index.xml` 记录原路径与时间。启动时若检测到 index 中存在未被正常清理的记录（即上次异常退出），弹出"检测到 N 个未保存的文档，是否恢复？"对话框，恢复项在标签标题上标记 `[已恢复]`，保存时默认走"另存为"而非覆盖原文件。

快照采用增量策略：只保存自上次快照以来变化的区间（记录 `dirty ranges`），避免每 30 秒重写整个大文件。

---

## 6. 关键专题设计

### 6.1 大文件处理策略

这是"快"这条底线能否成立的关键，单独设计。三档策略如下：

| 档位 | 文件体积 | 加载方式 | 编辑能力 | 降级项 |
|---|---|---|---|---|
| T1 常规 | ≤ 8 MB | 一次性读入内存，`SCI_SETTEXT` 装载 | 全功能 | 无 |
| T2 大文件 | 8 MB ~ 64 MB | 内存映射 + `SCI_CREATELOADER` 分块 `AddData` | 全功能（受限） | 语法着色降级为单色或关闭、关闭自动换行、关闭缩进引导线、撤销深度降至 20、函数列表停用 |
| T3 超大文件 | > 64 MB | 只读映射 + 延迟行索引 + 视口按需解码 | 只读（可另存为副本后编辑） | 除查找定位与另存为外全部关闭 |

**T2 的实现要点**

1. `SCI_CREATELOADER` 返回 `ILoader` 接口，宿主 `AddData(data, len)` 逐块投喂，最后 `ConvertToDocument()` 得到文档。它避免了"先在应用层拼一个完整字节串、再交给 Scintilla"造成的双份内存占用——这一项在 64 MB 文件上就能省下 64 MB 峰值内存。
2. 分块大小取 1 MB（与搜索块一致，便于复用读取器）。每块投喂后不回 UI 消息（`ConvertToDocument` 之前控件不可用），但加载进度显示在状态栏，并提供"取消加载"。
3. 因为 `ConvertToDocument` 之前控件没有内容，T2 的"首屏时间"由整文件装载决定。为改善感知，装载超过 800 ms 时状态栏显示逐 10% 递增的进度，并在装载完成后立刻执行"滚动到用户上次位置"。

**T3 的实现要点**

1. 只读映射整文件（`CreateFileMapping` + `MapViewOfFile`，x64 下不受 2 GB 限制），不复制内容。
2. **延迟行索引**：后台线程按块扫描换行符，构建 `std::vector<uint64_t> lineStarts`，构建过程中显示"正在建立索引 N%"，可用后可定位任意行。索引内存开销约为 `文件行数 × 8 字节`；1 亿行需要 800 MB，因此对行数超过阈值的文件改用**稀疏索引**（每 4096 行记录一个锚点，定位时局部扫描）。
3. **视口按需解码**：滚动时只解码可见区域上下各一屏的字节到 UTF-16 并交给自绘只读视图渲染，带行号与字节偏移。这让 1 GB 级日志也能流畅滚动。
4. 查找在 T3 下用同一套搜索引擎，直接对映射内存分块匹配，命中后可跳转定位。
5. 用户点"编辑此文件"时，提示"将以副本方式打开"，把映射转换成 T1/T2 的可编辑缓冲区。

**阈值可配**：三档阈值写入 `config.xml` 的 `largeFile` 分节，默认 8 MB / 64 MB。用户可上调以强制全功能打开（风险自担，UI 明示）。

### 6.2 编码识别与转换算法

编码问题是中文用户的头号痛点，因此识别流程必须可解释、可覆盖。

**识别顺序（严格按此顺序，命中即返回）**

| 步骤 | 判据 | 结论 | 置信度 |
|---|---|---|---|
| 1 | 用户显式指定（命令行/菜单/首选项"始终以此编码打开"） | 指定值 | 100（用户意图优先） |
| 2 | 首 2 字节为 `FF FE`（且非 `FF FE 00 00`） | UTF-16LE | 100 |
| 3 | 首 2 字节为 `FE FF` | UTF-16BE | 100 |
| 4 | 首 3 字节为 `EF BB BF` | UTF-8-BOM | 100 |
| 5 | 前 64 KB 严格通过 UTF-8 合法性校验（含多字节序列完整性与码点范围） | UTF-8 | 95 |
| 6 | 前 64 KB 中高位字节频繁出现，与配置的候选代码页（默认 GBK/936，可含 Big5/Shift-JIS）逐字节配对校验合法 | 对应代码页（ANSI） | 80 |
| 7 | 前 64 KB 全为 ASCII | ANSI 或 UTF-8（等价），按"新建默认编码"归类 | 100 |
| 8 | 以上均不成立 | ANSI + 当前系统代码页 | 40，状态栏提示"编码可能不正确" |

补充规则：

- **代码页合法性校验**必须做**双字节配对校验**，不能只看"字节是否在 0x80~0xFF"。GBK 中 `0x81~0xFE` 为前导字节、`0x40~0xFE`（除 0x7F）为后续字节，配对失败即判定不合法。常见的"UTF-8 文件被误判为 GBK"往往就是因为省掉了这一步。
- 用户可在状态栏点击编码名称直接切换（"以 XX 编码重新打开"），切换后置信度置为 100 并随会话保存——**用户的判断永久覆盖自动探测**。
- 探测结果与文件的实际写入编码无关；写入时始终使用 `DocBuffer.encoding` 的当前值。

**转换动作的三条路径**

| 动作 | 字节是否改变 | 使用场景 |
|---|---|---|
| 以 XX 编码打开 / 重新解释 | 否 | 乱码但文件本身没问题（选错了解释方式） |
| 转为 XX 编码 | 是 | 文件确实是别的编码，需要真正修复（写盘前自动生成 `.bak`） |
| 带/不带 BOM 切换 | 是（增删 3 字节） | 服务端脚本首行异常、Windows 工具链兼容 |

**UTF-16 的内部处理**：Scintilla 缓冲区统一以 UTF-8 存储，`DocBuffer.encoding.uniMode` 记录原始字节序。打开时按原字节序解码为 UTF-8；保存时按 `uniMode` 编码回原字节序并保证 BOM。这样 Scintilla 侧的样式、折叠、搜索全部只需处理一种字符编码路径，避免为每种编码维护分支。

### 6.3 分屏与克隆视图

1. 布局由 `ViewGroup` 描述，本版支持"单视图 / 左右双视图"。`ViewSlot` 数组预留到 4 个槽位，实现四分屏时只需扩展布局算法与 `axis` 枚举，不需要改动接口。
2. 分屏交互：通过视图边框拖动调整比例（`splitRatioPercent`），比例随会话保存；双击分隔条在 50% 与"最大化当前视图"之间切换。
3. `Move to other view`：修改 `DocBuffer.ownerView`，把控件从原槽位迁移到目标槽位（Scintilla 控件是独立子窗口，`SetParent` 即可迁移，无需重建文档）。
4. `Clone to other view`：为目标槽位创建一个新 Scintilla 控件，通过文档指针共享同一文档，并设置 `SCI_SETREADONLY(true)`。两视图在同一 UI 线程，共享文档的变更通知由 Scintilla 内部机制传播，因此主视图编辑时克隆视图自动刷新。
5. 克隆视图在标签栏中标注为 `(克隆)`，关闭克隆视图不影响原文档；关闭原文档时克隆视图一并关闭并提示。
6. 同步滚动按"首可见行号"对齐；对行结构差异巨大的两文件，额外提供"按行号百分比"的对齐模式（差异化小功能）。

### 6.4 深色模式与高 DPI

**高 DPI 清单与实现**

- 应用清单声明 `dpiAwareness = PerMonitorV2`，同时设置 `dpiAware = true/pm` 作为老系统回退。
- 所有尺寸常量为逻辑单位（96 DPI 基准），调用 `Dpi::Scale(int)` 换算；自绘代码中禁止出现魔法像素值。
- `WM_DPICHANGED` 中：① 按建议矩形调整主窗口；② 重新加载匹配尺寸的图标与位图；③ 通知 Scintilla 重设字号、行距、边距宽度、字体渲染质量（`SCI_SETMARGINWIDTHN`、`SCI_SETFONTQUALITY`）；④ 重新计算停靠面板与标签栏布局。
- 位图与图标资源按 100/125/150/200/250/300 六档准备；工具栏图标使用单色矢量位图光栅化，避免六套位图的维护成本。

**深色模式**

| 对象 | 处理方式 |
|---|---|
| 标题栏、系统边框 | `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` |
| 菜单栏、下拉菜单 | 自绘（`MF_OWNERDRAW` 或完全自绘菜单） |
| 滚动条 | `SetWindowTheme(hwnd, L"", L"DarkMode_Explorer")` + 自定义绘制兜底 |
| 标准对话框控件 | 子类化 + `WM_CTLCOLOR*` 返回主题画刷 |
| 消息框 | 自绘对话框替代 `MessageBox`（否则深色下白底刺眼） |
| 编辑器 | 通过 `StyleConfig` 切换整套样式（浅色主题与深色主题各一份样式表） |

主题切换必须**原子生效**：先切换主题令牌，再广播 `NFY_THEME_CHANGED`，各控件收到后统一重绘，避免出现半明半暗的过渡态。

### 6.5 主题与样式系统

两层结构，职责分明：

1. **界面主题（UI Theme）**：管理外壳控件的颜色令牌，定义于 `res/themes/*.xml`，一套主题约 40 个令牌。
2. **编辑器样式（Editor Style）**：管理各语言各标记类别的前景色/背景色/字体，定义于 `stylers.xml`，细粒度可调（对应基线"样式配置器"）。

两者通过 `editorFg`/`editorBg` 等少数令牌弱耦合：切换 UI 主题会给出"是否同时切换编辑器配色"的选项，用户可固定编辑器配色不随主题变化（很多用户有这个需求）。

主题令牌示例：

```xml
<theme name="Dark" base="dark">
  <color name="windowBg"         value="#1E1E1E"/>
  <color name="windowFg"         value="#D4D4D4"/>
  <color name="tabActiveBg"      value="#2D2D30"/>
  <color name="tabInactiveBg"    value="#252526"/>
  <color name="toolbarBg"        value="#2D2D30"/>
  <color name="border"           value="#3F3F46"/>
  <color name="accent"           value="#0E639C"/>
  <color name="editorBg"         value="#1E1E1E"/>
  <color name="editorFg"         value="#D4D4D4"/>
  <color name="caretLineBg"      value="#2A2D2E"/>
  <color name="selectionBg"      value="#264F78"/>
  <color name="indicatorMatchBg" value="#515C6A"/>
  <color name="indicatorMarkBg"  value="#6A5ACD"/>
</theme>
```

### 6.6 本地化

1. 语言包为 `res/langs/<locale>.lang.xml`，键为点分层级字符串，值为目标语言文本；支持占位符 `{0}`、`{1}`。
2. 键缺失时的回退顺序：当前语言 → 简体中文 → 英文 → 键本身（并在日志中记录缺失键，便于补齐）。
3. 切换语言不重启：遍历已注册的 UI 元素（菜单项、按钮、面板标题）重新设置文本；已创建的对话框在下次打开时生效。
4. 加速键（`&` 前缀）由语言包单独给出字段 `accel`，因为不同语言下合适的加速字母不同，硬编码会导致中文界面上出现奇怪的加速键。
5. 中文场景的额外要求：界面字体默认值必须能在中文 Windows 上正常显示（默认 `Microsoft YaHei UI`），且文本宽度测量按实际字体进行，避免中文按钮文字被截断——这是很多本地化软件的常见问题。

### 6.7 安全与合规设计

对应基线中"安全与合规"这一核心信任卖点，落地为四条：

1. **分发链路可信**：官方安装包与更新包均签名；程序内嵌公钥，更新时强制校验签名与哈希。下载源只允许 HTTPS，证书校验失败即中止（不提供忽略选项）。
2. **插件准入**：加载前校验 PE 机器类型为 `IMAGE_FILE_MACHINE_AMD64`；首次加载未知插件时列出其路径、作者、签名状态，由用户确认；可选开启"仅允许白名单插件"的严格模式。
3. **配置只有一份权威来源**：配置目录与插件目录之外，程序不写任何位置（便携模式尤其要求"不写注册表、不写 %APPDATA%、不写 %TEMP%"）；文件关联的写入必须由用户显式点击"注册文件关联"触发，且提供等价的注销动作。
4. **崩溃隔离的局限与现状说明**：同进程加载插件无法做到真正的故障隔离。本版用 SEH 包裹插件入口能拦住访问违例等同步异常，但拦不住插件自行创建线程后的崩溃、死锁或内存破坏。因此明确声明：插件运行于宿主进程内，其行为由插件作者负责；未来版本评估"插件子进程 + IPC"方案（记录为 ADR-06 的演进方向）。

### 6.8 可访问性与输入法

1. **输入法支持**：Scintilla 的 IME 处理已较完整，但需在自绘标签栏与停靠面板中正确处理 `WM_IME_*` 消息，避免"在标签上无法输入中文"这类问题。所有接受文本输入的自绘控件必须转发 IME 消息到默认处理。
2. **键盘可达**：所有功能必须有菜单路径或快捷键，不依赖纯鼠标操作；查找对话框支持 `Tab` 完整遍历。
3. **屏幕阅读器**：主窗口与关键控件实现 `WM_GETOBJECT` + UI Automation 的最小暴露，至少让活动标签、状态栏信息可被读取。
4. **高对比模式**：检测系统高对比设置后，主题令牌整体替换为系统颜色（`GetSysColor`），自绘控件改为按系统颜色绘制。

### 6.9 差异化能力设计

对应基线"四、已知短板与差异化机会"。这些能力不属于 P0 必做，但需要在架构上留好位置，避免后期改造成本过高。

| 差异化点 | 架构预留 | 计划版本 |
|---|---|---|
| 多光标编辑 | `EditView` 的编辑接口按"操作区间集合"设计，而非仅单区间；搜索命中、`Ctrl+D` 加选均可复用 | v1.1 |
| 嵌套/分层语法（HTML 内嵌 JS/CSS） | 语言定义中预留 `embeddedLangs` 与区域规则；lexer 层用 Lexilla 的 container/子 lexer 能力承接 | v1.2 |
| LSP 接入（智能补全、诊断、跳转） | `AutoComplete` 的候选来源设计为可插拔 provider；预留 `ILanguageService` 接口与 JSON-RPC 通道 | v1.3 |
| 更多分屏（四分屏） | `ViewGroup.slots` 已预留 4 槽位与布局算法扩展点 | v1.2 |
| 内置终端 / 任务运行 | 停靠面板体系已支持任意面板；终端用 ConPTY 承载 | v1.4 |
| 快捷键"迁移预设" | `ShortcutBinding` 支持方案名（Default / VSCode 风格 / Sublime 风格），一键批量重绑 | v1.1 |
| 批量替换安全预览 | 已纳入 P0 设计（5.5 节） | v1.0 |
| 中英文分设字体 | 已在 `StyleConfig` 与字体设置中落地 | v1.0 |

### 6.10 性能优化清单

按"投入产出比"排序，按需启用：

1. **Scintilla 侧**：关闭未使用的边距、限制边的绘制、缩小撤销深度、避免整文档重着色（使用 `SCI_COLOURISE` 的区间调用）。
2. **渲染侧**：自绘控件启用双缓冲（`WS_EX_COMPOSITED` 或手工位图缓存），减少闪烁。
3. **文本侧**：行统计、字符数/字节数计算改为惰性 + 缓存，仅在状态栏需要且内容变化后重算；在选区内统计时只算选区。
4. **搜索侧**：字面量查找优先用 `SCI_SEARCHINTARGET`（内存搜索，无需解码）；正则在 UTF-8 域直接匹配（PCRE2 的 UTF 模式），避免为匹配而先转 UTF-16。
5. **启动侧**：静态链接时用 `/OPT:REF /OPT:ICF` 去重；语言定义延迟到首次使用时加载；会话恢复采用 5.1 的两级策略。
6. **内存侧**：所有大缓冲区用 RAII 包装，禁止裸 `new[]`；文件映射在文档关闭时立即 `UnmapViewOfFile` + `CloseHandle`；对 10 个文档的长时间编辑做内存回归基准。

---

## 7. 配置文件与文件格式

### 7.1 目录布局

```
安装版：
  %APPDATA%\notepadcc\
    config.xml          首选项
    shortcuts.xml       快捷键 + 宏 + 运行命令
    session.xml         会话
    stylers.xml         编辑器样式
    langs.xml           内置语言定义（首次运行从程序目录复制，可被用户覆写）
    udl\*.xml           用户自定义语言
    themes\*.xml        自定义主题
    plugins\<Name>\     插件文件与插件私有配置
    backup\             崩溃恢复快照
    logs\               日志

便携版（程序目录）：
  <AppDir>\config\      与上完全相同的结构
  <AppDir>\plugins\     插件（与 config\plugins 合并搜索）
```

判定与解析统一由 `Paths::ConfigRoot()` 提供，禁止其他位置拼接路径。

### 7.2 config.xml（首选项）

```xml
<?xml version="1.0" encoding="UTF-8"?>
<NotepadccConfig schemaVersion="1">
  <general uiLanguage="zh-CN" theme="Dark" followSystemDarkMode="true"
           restoreSessionOnStart="true" checkUpdateOnStart="true"
           multiInstance="false" recentFileCount="20"/>
  <edit tabToSpaces="true" tabWidth="4" autoIndent="true" smartIndent="true"
        autoCloseBrackets="false" showIndentGuide="true" showWhitespace="false"
        showEol="false" caretLineHighlight="true" multiCursor="true"
        undoDepth="500" largeFileUndoDepth="20"/>
  <newFile defaultUniMode="Utf8" defaultCodePage="936" defaultEol="Crlf"/>
  <backup enabled="true" intervalSeconds="30" keepSnapshots="10"
          backupOnSave="false" dir=""/>
  <largeFile tier1MaxMB="8" tier2MaxMB="64" sparseIndexThresholdLines="1000000"/>
  <search maxBlockSizeKB="1024" matchTimeoutMs="2000" maxReplacements="200000"/>
  <print marginLeftMm="20" marginRightMm="20" marginTopMm="15" marginBottomMm="15"
         header="$(FILE_NAME)" footer="第 $(PAGE) 页" colorMode="color" lineNumbers="true"/>
  <misc proxyMode="system" proxyServer="" keepLogDays="7"/>
</NotepadccConfig>
```

加载规则：未知元素忽略并记警告；未知属性忽略；缺失项取默认值；`schemaVersion` 低于当前版本时执行迁移函数链（`Migrate_1_to_2` …），高于当前版本则拒绝启动并提示"配置来自更新版本"。

### 7.3 shortcuts.xml（快捷键 / 宏 / 运行命令）

```xml
<?xml version="1.0" encoding="UTF-8"?>
<NotepadPlusCompat>
  <InternalCommands>
    <Shortcut id="41001" Ctrl="yes" Alt="no" Shift="yes" Key="68"/>  <!-- 复制当前行 -->
    <Shortcut id="41002" Ctrl="yes" Alt="no" Shift="no"  Key="76"/>  <!-- 删除当前行 -->
  </InternalCommands>
  <Macros>
    <Macro name="TrimAndSort" Ctrl="yes" Alt="yes" Shift="no" Key="84">
      <Action type="0" message="2450" wParam="0" lParam="0"/>       <!-- 命令式动作 -->
      <Action type="1" message="2170" wParam="0" lParam="0"/>       <!-- Scintilla 消息 -->
      <Action type="2" message="0" wParam="0" lParam="0" sParam="line1&#10;line2"/>
    </Macro>
  </Macros>
  <UserDefinedCommands>
    <Command name="打开所在目录" Ctrl="yes" Alt="no" Shift="no" Key="0">
      explorer &quot;$(CURRENT_DIRECTORY)&quot;
    </Command>
  </UserDefinedCommands>
</NotepadPlusCompat>
```

顶层元素名沿用对标产品的命名，是为了让用户可以直接把旧的 `shortcuts.xml` 拷进来使用；解析器对新增的动作类型（如 `type="2"` 的文本插入）做兼容读取，遇到不认识的动作类型则跳过并警告，保证迁移过程不会因个别步骤导致整个文件不可用。

### 7.4 session.xml（会话）

```xml
<Session schemaVersion="1" savedAt="2026-10-15T11:20:31+08:00">
  <ViewGroup axis="Vertical" ratio="50" syncScrollVertical="false" activeSlot="0"/>
  <Docs>
    <Doc path="D:\work\app\main.cpp" bufferId="0" view="0" lang="cpp"
         uniMode="Utf8" eol="Crlf" caret="1024" anchor="1024"
         firstVisibleLine="30" xOffset="0" zoom="0" wrap="0" active="yes" readOnly="no">
      <Bookmarks><Line n="12"/><Line n="88"/></Bookmarks>
      <Folds><Line n="40"/><Line n="120"/></Folds>
      <TabColor value="#3B78C7"/>
    </Doc>
    <Doc path="" untitled="yes" bufferId="1" view="1" lang="markdown"/>
  </Docs>
  <RecentFiles>
    <File path="D:\work\app\README.md"/>
  </RecentFiles>
</Session>
```

未保存的未命名文档若内容超过 512 KB，不内联进 `session.xml`（会把文件撑大且拖慢启动），而是引用 `backup\` 下的快照文件。

### 7.5 langs.xml（语言定义）

```xml
<Languages>
  <Language name="cpp" ext="cpp cxx cc h hpp hxx inl" lexer="cpp"
            caseSensitive="yes" foldable="yes">
    <LineComment prefix="//"/>
    <BlockComment start="/*" end="*/" nested="no"/>
    <Keywords group="0">
      alignas alignof auto bool break case catch char class const constexpr
    </Keywords>
    <Keywords group="1">
      size_t wchar_t int64_t uint32_t std
    </Keywords>
    <AutocompleteFile>cpp.api</AutocompleteFile>
  </Language>
</Languages>
```

### 7.6 UDL 定义（用户自定义语言）

```xml
<UserLang name="MyDSL" ext="mydsl mdl" caseSensitive="yes">
  <Comment linePrefix="//" blockStart="(*" blockEnd="*)"/>
  <Keywords group="0" prefixMode="no">
    let fun match with type module end
  </Keywords>
  <Keywords group="1">true false null</Keywords>
  <StringDelim start="&quot;" end="&quot;" escape="\"/>
  <Operators>+ - * / = &lt; &gt; |&gt;</Operators>
  <Numbers dec="yes" hex="yes" float="yes"/>
  <Fold openTokens="let fun match" closeTokens="end"/>
</UserLang>
```

加载器 `UdlLoader` 把该定义编译为 Scintilla 的关键词表与属性串，并注册为一个 `LanguageDef{isUdl=true}`；此后折叠、注释、样式配置、函数列表对这些语言与内置语言一视同仁。

### 7.7 语言包（界面本地化）

```xml
<LangPack locale="zh-CN" name="简体中文">
  <Entry key="cmd.file.save"       value="保存"     accel="S"/>
  <Entry key="cmd.file.saveAll"    value="全部保存" accel="A"/>
  <Entry key="dialog.find.title"   value="查找"/>
  <Entry key="status.encoding"     value="编码: {0}"/>
  <Entry key="msg.externalChanged" value="文件 {0} 已被其他程序修改，是否重新加载？"/>
</LangPack>
```

### 7.8 插件清单（plugin.json）

```json
{
  "name": "Compare",
  "version": "1.0.0",
  "abiVersion": 1,
  "author": "notepadcc Team",
  "description": "双文件差异对比",
  "minHostVersion": "1.0.0",
  "loadOrder": 100,
  "dependencies": []
}
```

`loadOrder` 决定加载顺序（数值小的先加载），用于处理"插件 B 依赖插件 A 注册的命令"这类情况；`dependencies` 未满足时该插件被跳过并在插件管理器中标注原因。

---

## 8. 错误处理、日志与诊断

### 8.1 错误分类与响应策略

| 类别 | 典型场景 | 响应策略 | UI 表现 |
|---|---|---|---|
| 用户输入错误 | 正则语法错误、目录不存在、过滤器写法错误 | 就地提示，阻止动作发起 | 对话框内红字提示，焦点回到出错字段 |
| 可预期操作失败 | 文件被占用、权限不足、磁盘满、编码不可表示 | 返回结构化结果，由调用层决定提示 | 精确原因 + 下一步建议（重试/另存为/取消） |
| 资源不足 | 内存不足、打开超大文件、句柄耗尽 | 降级或拒绝，不崩溃 | 提示可用替代路径（只读打开/关闭其他文档） |
| 环境异常 | 配置损坏、语言包缺失、主题文件损坏 | 回退默认值并继续运行 | 静默 + 日志警告，仅首次提示一次 |
| 程序缺陷 | 断言失败、空指针、数组越界 | 断言在 Debug 中断；Release 下记录并尽力恢复 | 崩溃对话框（含日志路径与最小复现信息），可选上报 |
| 插件缺陷 | 插件入口抛异常或访问违例 | SEH 捕获，禁用该插件 | 提示"插件 X 已发生故障并停用"，提供查看日志 |

### 8.2 日志规范

```cpp
// infra/Logger.h
enum class LogLevel { Trace, Info, Warn, Error };

void Log(LogLevel lv, std::wstring_view category, std::wstring_view message);
void LogF(LogLevel lv, std::wstring_view category, const wchar_t* fmt, ...);

#define LOG_INFO(cat, msg)  ::Log(LogLevel::Info,  cat, msg)
#define LOG_WARN(cat, msg)  ::Log(LogLevel::Warn,  cat, msg)
#define LOG_ERR(cat, msg)   ::Log(LogLevel::Error, cat, msg)
```

日志格式：`[时间(毫秒精度)] [级别] [类别] [线程ID] 消息`。

要求：

1. 日志写入在独立线程完成（生产者-消费者队列），UI 线程调用 `Log` 不得阻塞超过 10 微秒。
2. 日志中禁止出现文件完整内容；文件路径可记录，超长时截断到末 120 字符。
3. 崩溃时（未处理异常/访问违例）先刷日志，再尝试生成 minidump 到 `logs\crash_YYYYMMDD_HHMMSS.dmp`。
4. 用户可在"关于"对话框一键打开日志目录；日志中提供"收集诊断信息"入口，把版本、系统信息、最近日志打包，便于报障。

### 8.3 崩溃处理

1. `SetUnhandledExceptionFilter` + `SetErrorMode(SEM_NOGPFAULTERRORBOX)` 接管顶级异常。
2. 崩溃处理流程：刷日志 → 保存当前编辑内容到 `backup\crash_<时间戳>\`（尽力而为，使用 `MiniDumpWriteDump` 之前先尝试一次同步写盘）→ 生成 minidump → 显示自绘崩溃对话框（含"复制诊断信息"按钮）。
3. 崩溃处理代码必须**只使用最保守的 API**（`CreateFile`、`WriteFile`、`GetModuleFileName`），不得分配堆内存，避免二次崩溃。
4. 下次启动时检测到 crash 目录，提示用户恢复内容。

### 8.4 断言与调试开关

- `NPCC_ASSERT(expr)` 在 Debug 下触发断点，在 Release 下记录 `Warn` 日志并继续执行（对可恢复场景），对不可恢复场景调用 `NPCC_FAIL_FAST`。
- 提供隐藏的"安全模式"启动参数 `--safe-mode`：不加载任何插件、不恢复会话、使用默认主题与默认配置。用于排查"某个插件或某份配置导致启动异常"的问题，第一次启动失败后自动提示用户使用该模式。

---

## 9. 测试与验收

### 9.1 测试分层

| 层次 | 覆盖对象 | 手段 | 门槛 |
|---|---|---|---|
| 单元测试 | `Encoding` `EolMode` `RegexEngine` `LiteralSearcher` `ColumnEdit` 序列生成、`Paths`、`Xml`、`TextTools` | C++ 单元测试框架，纯逻辑无窗口 | 行覆盖 ≥ 70%，核心算法 ≥ 90% |
| 组件测试 | `EditView`（驱动真实 Scintilla，无主窗口）、`ConfigCenter`、`SessionManager` | 创建离屏 Scintilla 实例驱动 | 关键路径全覆盖 |
| 集成测试 | 打开/保存/编码转换/查找替换/分屏/会话恢复/插件加载 | 脚本驱动 UI（UI Automation 或自研测试钩子） | P0 功能全覆盖 |
| 性能测试 | 冷启动、大文件打开、查找、批量替换、内存 | 可复现的基准数据集 + 计时埋点 | 满足 1.1.1 与 1.6 预算 |
| 兼容测试 | 多 DPI、多主题、多语言、Windows 10/11、便携/安装 | 虚拟机矩阵 | 无阻断性缺陷 |
| 长稳测试 | 连续 8 小时编辑、反复开关文档、反复搜索 | 自动化回归 | 无内存趋势性增长、无句柄泄漏 |

### 9.2 专项测试用例（节选）

**编码类**

1. 含中文的 UTF-8 无 BOM 文件：探测为 UTF-8，保存后字节不变。
2. GBK 文件：探测为 ANSI(936)；转为 UTF-8 后字节变化、内容正确；回退到 ANSI 后与原文件字节一致。
3. UTF-16LE 带 BOM：打开显示正常；保存后 BOM 与原字节序保持。
4. 纯 ASCII 文件：探测为 ANSI 或 UTF-8 均可接受，但不得出现乱码。
5. 构造"高位字节分布看似 GBK 实则 UTF-8"的样本：必须探测为 UTF-8（验证双字节配对校验的有效性）。
6. 含不可表示字符的文本转为 GBK：必须给出明确提示，不得静默丢字符。

**搜索类**

7. 正则跨行匹配（`\r\n` 边界、`(?s).`）。
8. 正则匹配恰好跨越 1 MB 分块边界：必须命中（验证 overlap 策略）。
9. 灾难性回溯正则（如 `(a+)+$` 作用于长 a 串）：必须在超时阈值内终止并提示。
10. 替换全部后一次 `Ctrl+Z` 完整回退。
11. 跨文件替换遇到只读文件：该文件失败、其余成功、报告准确。

**列编辑类**

12. 1000 行矩形选区插入数字序列（起始 1、步长 1、补零 4 位）：结果与逐行手工结果一致，且一次撤销可回退。
13. 列粘贴时目标行数不足：自动补行且列对齐正确。

**文件与并发类**

14. 编辑过程中用外部程序修改同一文件：200 ms 内弹出提示；选择"保留当前"后再次保存，弹出冲突确认。
15. 保存时文件被独占锁定：提示"被占用"并提供另存为，原文件不受损。
16. 双视图克隆同一文档：主视图输入，克隆视图实时同步且不可编辑。

**性能类**

17. 50 MB 单行巨型文件（无换行）：打开不卡死，查找可用。
18. 200 万个短行文件：行号边距、跳转到行功能正常，滚动流畅。

### 9.3 与基线验收清单的对照

基线附录 B 的十条自检项逐条对应到本文档的可验证位置：

| 自检项 | 验证依据 |
|---|---|
| 冷启动、体积、内存达标 | 1.1.1 指标表 + 性能测试报告 |
| 数十 MB 日志流畅打开与检索 | 6.1 三档策略 + 用例 17/18 |
| 三种搜索模式行为一致 | 5.4 流程 + 用例 7~11 |
| 编码/BOM/EOL 覆盖中文场景 | 6.2 算法 + 用例 1~6 |
| 列编辑与数字序列可用 | 5.6 流程 + 用例 12/13 |
| 分屏、克隆、同步滚动可用 | 6.3 + 用例 16 |
| 宏录制、保存、重复、迁移可用 | 2.9 + 5.7 + 7.3 |
| 插件管理器与接口可扩展 | 2.10 + 4.7 + 5.8 |
| 便携模式与配置迁移可用 | 2.3 第 6 条 + 7.1 |
| 快捷键可自定义并检测冲突 | 4.3 `FindConflict` + 2.12 第 3 条 |

### 9.4 缺陷分级

| 级别 | 定义 | 处理要求 |
|---|---|---|
| S1 阻断 | 崩溃、数据丢失、文件写坏、启动失败 | 立即修，发布前必须清零 |
| S2 严重 | 核心功能不可用（如保存失败、查找错误结果） | 发布前修复或给出明确规避方案 |
| S3 一般 | 非核心功能异常、界面错乱 | 排入最近版本 |
| S4 轻微 | 文案、图标、轻微视觉问题 | 择机修复 |

数据安全类问题（S1 中的数据丢失与文件写坏）视为最高优先级，任何情况下不允许带着已知的写坏风险发布。

---

## 10. 实施路线

### 10.1 里程碑划分

| 阶段 | 目标 | 交付内容 | 主要依赖 |
|---|---|---|---|
| M0 骨架 | 能开窗、能打字、能存盘 | 主窗口、Scintilla 集成、命令注册表、配置中心、原子保存、基础菜单与快捷键 | 无 |
| M1 可替代（P0 内核） | 达到"可以当默认编辑器用"的水准 | 多标签与会话、文件状态检测、编码/EOL 全套、查找替换三模式、在文件中查找替换、语法高亮与折叠、列编辑、书签、分屏与克隆、显示符号与缩放、快捷键自定义、便携模式 | M0 |
| M2 完整对标（P1） | 与基线功能全面对齐 | UDL、样式配置器、宏录制回放、插件宿主与插件管理器、主题与字体、多语言界面、函数列表、自动完成、文件夹工作区/项目、打印与导出、行操作工具集与转换工具、运行命令 | M1 |
| M3 生态与体验（P2） | 拉开差距 | 内置 Compare/JSON/XML/MIME/十六进制/预览等能力、远程编辑（FTP/SFTP）、脚本插件、自动更新与签名、代理、护眼模式、工具栏与右键菜单深度定制 | M2 |
| M4 超越项 | 落实差异化 | 多光标编辑、四分屏、嵌套语法、快捷键迁移预设，视情况推进 LSP 与内置终端 | M3 |

### 10.2 P0 阶段的任务拆解与依赖

```
M0 骨架
 ├─ A1 工程与构建（CMake + MSVC x64 + 静态链接 + CI 产出）
 ├─ A2 平台基础（窗口类、消息循环、DPI、日志、Paths、线程池）
 ├─ A3 命令注册表与快捷键框架        ← A2
 ├─ A4 Scintilla/Lexilla 集成与 EditView 适配   ← A2
 ├─ A5 配置中心（加载/校验/原子写盘）  ← A2
 └─ A6 原子保存与文件读取             ← A2

M1 可替代
 ├─ B1 标签栏与缓冲区管理（DocumentManager）      ← A3 A4 A6
 ├─ B2 编码与 EOL（探测、转换、BOM）              ← A4 A6
 ├─ B3 查找替换（三模式）+ 匹配高亮               ← A4
 ├─ B4 跨文件查找替换（线程池 + 结果面板）        ← B3 A2
 ├─ B5 语法高亮与折叠（语言定义加载 + 识别）      ← A4
 ├─ B6 列编辑与列块对话框                          ← A4
 ├─ B7 书签与标记                                 ← A4
 ├─ B8 视图：分屏、克隆、自动换行、缩放、符号显示  ← B1
 ├─ B9 会话管理（保存/恢复/崩溃快照）              ← A5 B1
 ├─ B10 文件状态检测（FileWatcher）                ← B1
 ├─ B11 首选项界面与快捷键管理界面                  ← A3 A5
 └─ B12 便携模式与绿色发行                          ← A5
```

关键路径为 `A2 → A4 → B1 → B2/B3/B5`，其余任务可与主路径并行。B9 与 B10 是唯一涉及"跨文档一致性"的两项，建议由同一人负责以降低协调成本。

### 10.3 风险登记

| 风险 | 影响 | 概率 | 应对 |
|---|---|---|---|
| Scintilla 大文件装载性能不达预期 | 打开 50 MB 文件超时 | 中 | 提前做 6.1 的 T2 验证原型（M0 阶段即验证 64 MB 装载耗时） |
| 克隆视图共享文档在折叠/书签上互相干扰 | 分屏体验降级 | 中 | 已备降级方案（独立副本 + 同步刷新），在 M1 早期验证并决策 |
| 深色模式下系统控件绘制异常 | 视觉体验受损 | 高 | 全部关键控件自绘；对无法自绘的系统对话框做深色化兜底处理 |
| 插件崩溃拖垮主程序 | 稳定性口碑受损 | 中 | SEH 兜底 + 故障插件自动禁用 + 明确的责任边界声明 |
| 编码探测误判 | 中文用户直接感知 | 中 | 双字节配对校验 + 置信度提示 + 一键重新解释 + 大规模语料回归 |
| 高 DPI 下自绘布局错位 | 视觉体验受损 | 中 | 全量使用逻辑单位；CI 中加入多 DPI 截图对比 |
| 跨文件批量替换误伤 | 数据破坏（S1） | 低 | 预览确认 + 数量上限 + 可选备份 + 原子写回 |

### 10.4 完成定义（DoD）

一个功能视为完成，需要同时满足：

1. 代码合入主干并通过静态检查（`/W4` 无新增警告）。
2. 对应的单元/集成测试已提交且通过。
3. 用户可见文本已加入语言包（不留硬编码）。
4. 命令已注册到命令注册表，可在快捷键管理中重绑。
5. 涉及配置项的功能已在 `config.xml` 与首选项界面中双向打通。
6. 涉及会话状态的功能已在 `session.xml` 中持久化。
7. 相关文档（本文档对应章节）已同步更新。

---

## 附录 A：快捷键映射表

与基线附录 A 对齐，作为默认方案（Default）。全部可在"设置 → 管理快捷键"中重绑。

| 功能 | 快捷键 | 命令 ID |
|---|---|---|
| 新建 / 打开 / 保存 | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | 10101 / 10102 / 10103 |
| 另存为 | `Ctrl+Shift+S` | 10104 |
| 关闭当前文档 | `Ctrl+W` | 10105 |
| 关闭全部文档 | `Ctrl+Shift+W` | 10106 |
| 下一个 / 上一个标签 | `Ctrl+Tab` / `Ctrl+Shift+Tab` | 11101 / 11102 |
| 下一个 / 上一个标签（备用键） | `Ctrl+PgDn` / `Ctrl+PgUp` | 11101 / 11102 |
| 查找 / 替换 | `Ctrl+F` / `Ctrl+H` | 20101 / 20102 |
| 在文件中查找 / 替换 | `Ctrl+Shift+F` / `Ctrl+Shift+H` | 20103 / 20104 |
| 查找下一个 / 上一个 | `F3` / `Shift+F3` | 20105 / 20106 |
| 标记全部匹配 | `Ctrl+Shift+M` | 20107 |
| 跳转到指定行 | `Ctrl+G` | 20108 |
| 复制当前行 / 删除当前行 | `Ctrl+D` / `Ctrl+L` | 30101 / 30102 |
| 上移 / 下移当前行 | `Ctrl+Shift+Up` / `Ctrl+Shift+Down` | 30103 / 30104 |
| 转小写 / 转大写 | `Ctrl+U` / `Ctrl+Shift+U` | 30105 / 30106 |
| 注释 / 取消注释 | `Ctrl+Q` | 30107 |
| 区块注释 | `Ctrl+Shift+Q` | 30108 |
| 跳到匹配括号 | `Ctrl+B` | 30109 |
| 添加/取消书签 / 下一书签 / 上一书签 | `Ctrl+F2` / `F2` / `Shift+F2` | 40101 / 40102 / 40103 |
| 列块编辑 | `Alt+C` | 50101 |
| 列选择 | `Alt + 鼠标左键拖动` | —— |
| 自动完成 | `Ctrl+Space` | 60101 |
| 折叠 / 展开当前层级 | `Ctrl+Alt+F` / `Ctrl+Alt+Shift+F` | 70101 / 70102 |
| 折叠全部 / 展开全部 | `Alt+0` / `Alt+Shift+0` | 70103 / 70104 |
| 开始/停止录制宏 / 播放 / 重复执行 | `Ctrl+Shift+R` / `Ctrl+Shift+P` / `Ctrl+Shift+L` | 80101 / 80102 / 80103 |
| 全屏 | `F11` | 90101 |
| 运行命令 | `F5` | 100101 |
| 放大 / 缩小 / 恢复默认缩放 | `Ctrl+加号` / `Ctrl+减号` / `Ctrl+0` | 90201 / 90202 / 90203 |
| 切换分屏 | `Ctrl+Alt+V` | 90102 |
| 移到另一视图 | `Ctrl+Alt+M` | 90103 |
| 显示符号开关 | `Ctrl+Alt+W` | 90301 |

命令 ID 编码规则（实现见 `core/Ids.h` 的 `MakeCmdId`）：**高 4 位为类别，低 12 位为类别内序号**，即 `(类别 << 12) | 序号`。

之所以不用"类别位 + 四位序号"那种 5 位数方案，是因为 **ID 必须落在 16 位以内**：菜单项 ID 取自 `WM_COMMAND` 的 `LOWORD(wParam)`，加速键表的 `ACCEL.cmd` 也是 `WORD`，两者都是 16 位。按本方案，12 个类别 × 4096 个序号，最大 ID 为 53247，安全落在范围内。

插件命令若需要更大的 ID 空间（例如从 1000000 起编号），**不能直接放进菜单或加速键表**：宿主必须为它另行分配一个 16 位菜单槽位，并在槽位与内部 CmdId 之间做映射。这一点在实现插件体系时容易踩。

缓冲区 ID（`BufferId`）另有一套约定：**从 1 开始编，0 永远不是合法 ID**，这样 0 可以安全地用作"无活动标签"之类的哨兵值。

## 附录 B：编码与命名约定

| 项 | 约定 |
|---|---|
| 源文件编码 | UTF-8 **无 BOM**。MSVC 侧靠 `/utf-8` 同时指定源码字符集与执行字符集，GCC 默认即为 UTF-8；不要依赖 BOM |
| 字符串类型 | 面向用户的文本一律 `std::wstring` / `const wchar_t*`；文件内容与协议一律 `std::string`（UTF-8） |
| 类名 | 大驼峰（`DocumentManager`） |
| 成员变量 | 尾下划线（`hwnd_`）或小驼峰不加前缀，全项目统一取前者 |
| 常量 | `k` 前缀（`kInvalidId`） |
| 头文件保护 | `#pragma once` |
| 命名空间 | `npcc::<module>`（如 `npcc::edit`、`npcc::search`） |
| 错误传递 | 优先返回值（`enum class` 结果或 `std::expected` 风格），异常仅用于不可恢复场景 |
| 资源管理 | RAII 包装全部 HANDLE/GDI 对象，禁止裸 `CloseHandle` 分散在各处 |

**已经踩过的语言陷阱**（留在这里提醒后来人，两条都真实发生过）：

| 陷阱 | 表现 | 规避 |
|---|---|---|
| 宽字符 `printf` 的 `%s` | 按标准 C 语义 `%s` 表示**窄**字符串（MSVC 恰好相反），把 `wchar_t*` 传进去只会打印出首字符。日志曾经变成 `[2] [I] [a]` | 一律写 `%ls`，并给日志格式加一条自检 |
| 同名重载同时接受字符串与 `bool` | `SetAttribute(L"path", L"")` 里 `const wchar_t*` 转 `bool` 是**标准转换**，优于转 `wstring_view` 的用户定义转换，于是空串被静默写成 `"no"`；同理 `SetAttribute(name, L"yes")` 会递归调用自己 | 不按类型重载同名函数，改用 `SetAttributeInt` / `SetAttributeBool` 这类以名字区分的写法 |
| 成员名撞上 Windows 宏 | `windows.h` 里有一批 `#define FindText FindTextW`、`#define GetObject GetObjectW` 之类的宏。把成员函数命名为 `FindText` 后，调用点会被预处理悄悄改写成 `FindTextW`，编译器报的是"没有成员 FindTextW"，可代码怎么看都是对的 | 成员名避开 Win32 宽字符宏族（`FindText`、`GetObject`、`LoadImage`…），或显式写成 `::FindTextW` |
| 跨进程 `SendMessage` 不封送指针 | Windows 只为 `WM_USER` 以下的**系统消息**做参数封送。`SCI_*` 全在 `WM_USER` 以上，从别的进程发过去时指针参数不会被翻译，目标进程按自己的地址空间解引用，读到的是垃圾——界面回归脚本因此一度把文档清空了 | 跨进程只发**只传值**的消息（如 `SCI_GETCHARAT` 逐字节读回），需要传指针时改用系统消息或让程序自己准备数据 |

## 附录 C：Windows 消息与自定义消息分配

| 消息 | 值 | 用途 |
|---|---|---|
| `CPPM_OPEN_FILES` | `WM_APP + 1` | 单实例转交文件路径 |
| `CPPM_SEARCH_PROGRESS` | `WM_APP + 2` | 跨文件搜索进度回传 |
| `CPPM_SEARCH_RESULT` | `WM_APP + 3` | 单文件结果回传 |
| `CPPM_TASK_FINISHED` | `WM_APP + 4` | 后台任务完成通知 |
| `CPPM_FILE_CHANGED` | `WM_APP + 5` | 文件监视通知 |
| `CPPM_PLUGIN_NOTIFY` | `WM_APP + 6` | 插件事件转发 |
| `CPPM_APP_ACTIVATE` | `WM_APP + 7` | 其他实例请求前置窗口 |
| `WM_APP + 100` 起 | | 保留给外壳内部控制使用 |

自定义消息必须集中定义在一个头文件中，禁止在各处直接书写 `WM_APP + n`，否则极易发生值冲突。

---

## 附录 D：与基线文档的差异说明

本设计在完全覆盖基线功能域的前提下，对以下三处做了调整或强化，在此集中说明以便评审：

1. **目标架构由 Win32 调整为 Win64（x64）**。这是本版的显式决策，影响面见 1.1.2。主要收益是可以利用 64 位地址空间实现超大文件的零拷贝映射（6.1 的 T3 档），这恰好对应基线中"大文件流畅打开"这一核心卖点。代价是彻底放弃 32 位环境（Windows 10 1809 起 64 位覆盖率已接近 95%，可接受）。
2. **正则引擎统一为 PCRE2**。基线对标产品的编辑器内搜索与跨文件搜索在正则语义上存在细微差异，本设计统一为一套引擎，避免用户在两种场景下得到不同结果。
3. **新增数据安全类设计**（4.6 原子写盘、5.4 替换数量上限、5.5 批量替换预览、6.2 编码转换自动备份、8.3 崩溃快照）。这些不属于基线列出的功能点，但属于"编辑器出错时用户无法承受"的问题，因此提升为设计级要求。





