#pragma once

#include <windows.h>

#include <functional>
#include <atomic>
#include <string>
#include <vector>

#include "core/CommandRegistry.h"
#include "core/DocumentManager.h"
#include "core/Macro.h"
#include "core/RunCommand.h"
#include "core/ScriptEngine.h"
#include "core/Messages.h"
#include "core/ExportHtml.h"
#include "core/ShortcutStore.h"
#include "core/ConfigCenter.h"
#include "core/SessionManager.h"
#include "core/SnapshotStore.h"
#include "edit/Document.h"
#include "edit/EditView.h"
#include "edit/LanguageRegistry.h"
#include "edit/StyleScheme.h"
#include "infra/CrashGuard.h"
#include "infra/ThreadPool.h"
#include "infra/UpdateService.h"
#include "infra/ScriptRunner.h"
#include "shell/StatusBar.h"
#include "shell/TabBar.h"
#include "shell/ToolBar.h"
#include "shell/FolderPanel.h"
#include "shell/FunctionListPanel.h"
#include "shell/HexView.h"
#include "shell/MarkdownPreview.h"
#include "shell/StructureTreePanel.h"
#include "plugin/PluginHost.h"
#include "shell/FindBar.h"
#include "shell/SearchPanel.h"
#include "search/CrossFileSearch.h"
#include "search/CrossFileReplace.h"
#include "io/FileWatcher.h"

namespace npcc::shell {

// MainWindow 实现插件宿主需要的那几个动作（文档、命令、日志都在外壳手里）。
// 用继承而不是回调函数簇：接口只有六个方法，插件宿主也不认识别的窗口类型
class MainWindow : public plugin::HostActions {
public:
    static constexpr const wchar_t* kClassName = L"notepadccMainWindow";

    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool RegisterWindowClass(HINSTANCE instance) const;
    bool Create(HINSTANCE instance, const std::vector<std::wstring>& filesToOpen);

    HWND   Handle() const noexcept { return hwnd_; }
    HACCEL Accelerators() const noexcept { return accel_; }

    // 自检开关（--crash-test）：等窗口就绪后故意崩一次，用来验证崩溃处理这条路径
    void SetCrashTest(bool enabled) noexcept { crashTest_ = enabled; }
    // 全屏里按 Esc 退出（设计文档 2.4 第 6 条）。**只能由消息循环调用**：
    // 编辑控件拿着焦点，WM_KEYDOWN 根本送不到主窗口来，而把 Esc 绑成全局快捷键
    // 又会在非全屏时反过来触发全屏。返回 true 表示这个键已经被消化掉了。
    bool ExitFullScreenOnEscape();

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void RegisterCommands();
    void RebuildMenu();
    void RebuildAccelerators();
    void LayoutChildren() const;
    void UpdateTitle() const;
    void UpdateStatus() const;
    // 只刷"文档信息"两段（编码 / 行尾 / 语言 / 修改标记），不碰第一段。
    // WM_COMMAND 那一段用它：命令自己可能刚在第一段留了一句提示（"没有可配对的括号"
    // 这类），整条 UpdateStatus 会把提示当场盖掉，界面上看起来就是"点了没反应"。
    void UpdateDocumentInfo() const;
    void SyncTabBar();
    void SyncFileWatcher();
    void ApplyViewToggles(edit::Document& document) const;
    core::CmdContext Context();

    // ---- 命令实现 ----
    void CommandNew();
    void CommandOpen();
    bool CommandSave();
    bool CommandSaveAs();
    bool CommandCloseCurrent();
    bool CommandCloseAll();
    void CommandNextTab();
    void CommandPrevTab();
    void CommandFind(bool replaceMode);
    void CommandFindNext(bool forward);
    bool HandleFindBarCommand(int controlId);
    void CommandFindInFiles();
    void CommandReplaceInFiles();
    void StartReplacePlan();
    void ApplyReplacements();
    void CommandToggleBookmark();
    void CommandGotoBookmark(bool forward);
    void CommandClearBookmarks();
    void CommandColumnEdit();
    void CommandToggleOverwrite();
    void ApplyConfigToRuntime();
    // 按配置装配工具栏（按钮表 + 可见性 + 图标大小），随后重排布局
    void ApplyToolbarConfig();
    // 按配置装配右键菜单项表（键的解析与灰显在弹出时逐条做，这里只管整理与留痕）
    void ApplyContextMenuConfig();
    void ScheduleAutosave();
    void WriteSnapshots();
    void RemoveSnapshotFor(core::BufferId id);
    void CheckStartupRecovery();
    void RestoreSnapshot(const core::SnapshotInfo& info);
    void RefreshRescueSlots();
    static int RescueLengthOf(void* context, int index);
    static int RescueCopyInto(void* context, int index, char* buffer, int capacity);
    void ApplyEditConfigToAllViews() const;
    void ApplyEditConfigToView(edit::EditView& view) const;
    void HandleExternalChange(const std::wstring& path, io::FileChangeKind kind);
    void CheckActiveDocumentForExternalChange(
        io::FileChangeKind kind = io::FileChangeKind::Modified);
    void StartFileSearch();
    bool HandleSearchPanelCommand(int controlId);
    void JumpToSearchResult();
    void FinishFileSearch(const search::SearchProgress& progress);
    void UpdateSearchProgress();
    void CommandPreferences();
    void CommandManageShortcuts();
    void CommandToggleToolbar();
    void CommandCustomizeToolbar();
    void CommandCustomizeContextMenu();
    // 检查更新（M3 第十块）：手动入口起一次对话框；启动延迟检查走同一条后台路径
    void CommandCheckUpdate();
    void StartBackgroundUpdateCheck();
    void HandleBackgroundUpdateResult(const infra::UpdateCheckReport& report);
    infra::ThreadPool& UpdatePool();
    void ShowEditorContextMenu(POINT screenPoint);
    void CommandToggleSplit();
    void CommandCyclePane();
    void CommandConvertEol(edit::EolMode target);
    void CommandConvertEncoding(edit::UniMode target);
    // 行操作与文本转换（设计文档 2.13）。tool 接收"选区或整篇"的字节与当前代码页。
    void ApplyTextTool(
        const std::function<std::string(std::string_view, std::uint32_t)>& tool,
        const wchar_t* description);
    // 文本解码的三种来源（URL 百分号 / Base64 / Quoted-Printable）：共用一条
    // "先校验、失败讲清楚、成功走 ApplyTextTool"的路径
    enum class TextDecodeKind { Url, Base64, QuotedPrintable };
    void CommandDecodeText(TextDecodeKind kind);
    void CommandToggleComment(bool block);
    // JSON / XML 格式化（设计文档 2.13）。这两件事会失败（内容不合法），
    // 所以不能走 ApplyTextTool 那条"永不失败"的入口：先试一次，失败只弹说明。
    void CommandFormatStructured(bool json, bool minify);
    // 校验 XML（M3 的内置 XML 工具）：只探一次结构树、不改内容；结果（行数 / 节点数
    // 或者出错的行列）进状态栏与日志
    void CommandValidateXml();
    void CommandXPathQuery();
    void CommandExportLanguageDefs();
    void CommandStyleConfigurator();
    void CommandExportUdlSample();
    void CommandToggleDarkMode();
    // 护眼 / 低蓝光（设计文档 2.7 第 7 条）：只换编辑区与标签栏的配色，不改变行为
    void CommandToggleEyeCare();
    edit::EditorPalette CurrentEditorPalette() const;
    // 基线 P0 的三件"编辑器基本功"：全屏（F11）、跳到匹配括号（Ctrl+B）、摘要
    void CommandToggleFullScreen();
    void CommandGotoMatchingBrace();
    void CommandShowSummary();
    // ---- 宏（设计文档 2.9 / 5.7）----
    void CommandToggleMacroRecord();
    void CommandPlayLastMacro();
    void CommandRunMacroMultiple();
    void CommandRunSavedMacro(std::size_t index);
    void RunMacro(const core::Macro& macro, int repeatCount, bool stopOnError);
    void RegisterMacroCommands();
    void RefreshMacroMenuEntries();
    void FlushRecordingToStore();   // 停止录制 → 取名 → 存文件 → 注册菜单
    // ---- 运行命令（设计文档 2.13 第 3 条）----
    void CommandShowRunDialog();                         // F5：填一条命令行
    void CommandRunSaved(std::size_t index);             // 跑一条已保存的命令
    core::RunContext MakeRunContext() const;             // 变量展开的输入
    void ExecuteRunCommand(std::wstring_view command, bool captureOutput);
    void RegisterRunCommands();
    bool PollRunCommand();                               // 后台命令跑完了吗
    void FinishRunCommand();                             // 收尾：读输出 → 开文档
    void CancelPendingRun(bool deleteOutput);            // 退出时清理
    // ---- 脚本插件（M3 第十二块）----
    void CommandRunScriptFile();      // 工具菜单：运行脚本…（选任意 .py 跑一次）
    void CommandOpenScriptsFolder();  // 工具菜单：打开脚本目录
    void CommandRefreshScripts();     // 工具菜单：刷新脚本列表
    void RefreshScriptList();         // 扫描 <配置根>\scripts（IO 这一半）
    void RegisterScriptCommands();    // 把脚本列表变成工具菜单里的命令
    void RunScript(std::wstring_view scriptPath, std::wstring_view displayName);
    bool PollScript();                // 脚本跑完了吗（超时顺带终止）
    void FinishScript();              // 收尾：结果写回文档 / 失败收进新文档
    void CancelPendingScript();       // 退出时清理（终止子进程）
    // 生成一份"只读用途"的输出文档（运行命令与脚本共用）：UTF-8 + CRLF、
    // 不进撤销栈、钉在保存点上。text 按 UTF-8 字节给出
    edit::Document* OpenOutputDocument(std::wstring_view displayName, std::string_view text);
    // 正在跑的那条命令（捕获输出时才有：不捕获就是"丢出去不管"）
    struct PendingRun {
        HANDLE        process = nullptr;
        std::wstring  outputPath;      // 捕获时的临时文件
        std::uint64_t startedMs = 0;
    };
    PendingRun   pendingRun_;
    bool         pendingRunActive_ = false;
    // 命令行里要打开的文件（在建窗口之前就填好）。WM_CREATE 用它判断"要不要顺手
    // 建一个空白文档"——有文件要开就别建，否则用户会看到一个多余的空标签（见附录 B）
    std::vector<std::wstring> startupFiles_;
    std::wstring lastRunCommand_;   // 下次打开 F5 时预填
    int          runOutputCounter_ = 0;
    int          runTempCounter_ = 0;
    // 脚本插件：列表来自 <配置根>\scripts；"刷新脚本列表"把命令整批换掉
    std::vector<core::ScriptInfo> scripts_;
    std::size_t scriptCommandCount_ = 0;   // 上一批注册了几条（撤旧用）
    std::unique_ptr<infra::ScriptRunner> scriptRunner_;
    struct PendingScript {
        std::wstring   name;
        core::BufferId docId = core::kInvalidId;
        int32_t        start = 0;
        int32_t        end = 0;
        std::string    input;   // 喂给脚本的原文：收尾时用它核对文档有没有被改过
    };
    PendingScript pendingScript_;
    int           scriptOutputCounter_ = 0;
    // 十六进制视图：一次只看一个文件（它读的是磁盘字节，不是缓冲区）
    HexView      hexView_;
    bool         hexMode_ = false;
    core::BufferId hexDocument_ = 0;   // hexMode_ 为真时正在看哪个文档
    // Markdown 预览面板（右侧一栏，开关记在配置里）
    MarkdownPreview markdownPreview_;
    bool            markdownPreviewVisible_ = false;
    int             previewFileCounter_ = 0;
    // 结构树面板（JSON 树与 XML 树共用这一块面板；与 Markdown 预览互斥：都用右侧那一栏）
    StructureTreePanel structureTreePanel_;
    bool            jsonTreeVisible_ = false;
    bool            xmlTreeVisible_ = false;
    // ---- 界面语言（设计文档 6.6 / 7.7）----
    void RegisterLanguageCommands();
    void CommandSwitchLanguage(const std::wstring& locale);
    void CommandExportLangPack();
    // 把"当前语言包盖不住哪些键"写进日志（英文界面里冒出中文时，这条日志是唯一线索）
    void ReportMissingLanguageKeys();
    // 按当前语言重设所有内置命令的显示文本并重建菜单（切换语言时不重启走的就是这条）
    void ApplyUiLanguage();
    // ---- 十六进制查看（设计文档 2.13 第 5 条）----
    void CommandToggleHexView();
    void CommandHexGoTo();
    bool HexModeActive() const noexcept { return hexMode_; }
    void LeaveHexView();     // 回到文本视图（不碰菜单勾选态：调用方自己重建）
    void LayoutViews() const;   // 按当前模式把视图区摆好（WM_SIZE 与切换时都走它）
    void UpdateHexStatus() const;   // 状态栏第一段显示十六进制偏移
    // ---- 预览（设计文档 2.13 第 4 条）----
    void CommandPreviewInBrowser();
    void CommandToggleMarkdownPreview();
    void ScheduleMarkdownPreview();     // 内容变了之后节流刷新（300 ms）
    void RefreshMarkdownPreview();      // 立刻按活动文档重排面板
    bool ActiveDocumentIsMarkdown() const;
    // ---- 结构树面板（M3 的内置 JSON 能力与 XML 工具共用）----
    void CommandToggleJsonTree();
    void ScheduleJsonTree();
    void RefreshJsonTree();
    void CommandToggleXmlTree();
    void ScheduleXmlTree();
    void RefreshXmlTree();
    // ---- 比较（设计文档第 10 章 M3 的内置 Compare）----
    void CommandCompareWithDocument();   // 与另一个**已打开**的文档比
    void CommandCompareWithDisk();       // 与这份文档在磁盘上的版本比（"我改了哪些"）
    void ShowComparison(edit::Document& left, const std::string& leftText,
                        const std::string& rightText, const std::wstring& rightName);
    // ---- 文件夹工作区（设计文档 2.11）----
    void CommandOpenWorkspace();      // 选一个目录当工作区
    void CommandCloseWorkspace();
    void CommandRefreshWorkspace();
    void CommandOpenWorkspaceSelection();   // 打开树里选中的文件（双击/回车也走它）
    void ApplyWorkspace(const std::wstring& folder, bool persist);
    void HandleFolderNotify(const FolderNotify& notification);
    // ---- 函数列表（设计文档 2.6 第 5 条）----
    void CommandToggleFunctionList();
    // ---- 打印与导出（设计文档 2.14）----
    void CommandPrint();
    void CommandExportHtml();
    void CommandExportRtf();
    void CommandRemoteSites();
    // ---- 插件（设计文档 2.10）----
    void LoadPlugins();
    void CommandPluginManager();
    // plugin::HostActions
    bool RegisterPluginCommand(const std::wstring& pluginName, const std::wstring& key,
                               const std::wstring& name, void (*execute)(void*),
                               void* pluginCookie) override;
    std::wstring ConfigPath() const override;
    std::string  EditorText() const override;
    bool         ReplaceSelection(std::string_view utf8) override;
    void         ShowMessage(const std::wstring& text) override;
    void         Log(int level, const std::wstring& text) override;
    // 收集"当前文档的带样式文本 + 各样式的外观"，导出与打印共用
    bool CollectStyledDocument(std::vector<core::StyledByte>& bytes,
                               std::vector<core::ExportStyle>& styles,
                               core::ExportOptions& options) const;
    // 与上者同一套读取，但只取 [start, end) 这一段（"复制为富文本"按选区走）
    bool CollectStyledRange(std::vector<core::StyledByte>& bytes,
                            std::vector<core::ExportStyle>& styles,
                            core::ExportOptions& options, int32_t start, int32_t end) const;
    void CommandCopyAsRtf();
    void ScheduleFunctionParse();          // 节流之后的解析请求
    void StartFunctionParse();             // 真正提交后台解析（由 WM_TIMER 触发）
    void HandleFunctionNotify(const FunctionNotify& notification);
    void ApplyFunctionSymbols(std::uint64_t requestId,
                              std::vector<core::FunctionSymbol> symbols);
    // 打开一个具体路径（工作区双击、单实例转交都用它，免得两处各写一遍）
    bool OpenDocumentPath(const std::wstring& path);
    // 打开后留在 loadWarning 里的提示（二进制内容、混合行尾、大文件档）在状态栏露面一次
    void AnnounceActiveLoadWarning();
    void ApplyStyleSchemeToAllViews();
    void CommandAutoComplete();
    const edit::LanguageDef* ActiveLanguage() const;
    void WriteConvertBackup(const std::wstring& path) const;
    void CommandRegisterAssociations(bool registerThem);
    void CommandAbout();

    // ---- 分屏 / 克隆视图（设计文档 6.3）----
    void  SyncSplitView();
    void  FocusActivePane();
    bool  PointInDivider(POINT point) const;
    void  DragDividerTo(int x);
    edit::EditView* SecondaryView() const;

    // ---- 文档操作 ----
    bool SaveDocument(edit::Document& document);
    bool SaveDocumentAs(edit::Document& document);
    bool ConfirmSaveIfModified(edit::Document& document);   // 返回 false = 取消
    bool CloseDocument(core::BufferId id, bool force);
    bool CloseOthers(core::BufferId keep);
    bool CloseAllDocuments();
    bool CloseRightOf(core::BufferId anchor);
    void ActivateDocument(core::BufferId id);
    void EnsureAtLeastOneDocument();
    bool RestoreSession();
    core::SessionSnapshot BuildSessionSnapshot() const;
    edit::Document* DocumentOfView(HWND view) const;
    void HandleTabBarNotify(const TabBarNotify& notification);
    void HandleEditorNotify(const NMHDR& header);
    search::SearchConfig CurrentSearchConfig() const;
    edit::Document* ActiveDocument() const;

    HINSTANCE                  instance_ = nullptr;
    HWND                       hwnd_ = nullptr;
    HMENU                      menu_ = nullptr;
    HACCEL                     accel_ = nullptr;
    core::CommandRegistry      registry_;
    core::ConfigCenter         config_;
    core::SnapshotStore        snapshots_;
    edit::LanguageRegistry     languages_;
    edit::StyleScheme          styles_;
    // 启动时记录的默认快捷键表，"恢复默认"要用（见 ShortcutDialog）
    std::vector<core::ShortcutBinding> defaultShortcuts_;
    // 崩溃抢救的文档槽位。数组必须活到进程结束：崩溃处理器只持有指针。
    std::unique_ptr<infra::crashguard::RescueDocument[]> rescueDocs_;
    // 每个文档上一次快照的内容指纹，用来避免"内容没变也反复写盘"
    struct SnapshotState {
        core::BufferId id = core::kInvalidId;
        std::uint64_t  hash = 0;
        bool           tooLargeWarned = false;
    };
    std::vector<SnapshotState> snapshotStates_;
    bool recoveryPending_ = false;
    // "文件已被外部修改"确认框开着的时候，兜底检查不要再叠第二个：
    // 关框时焦点回到主窗口会同步触发 WM_SETFOCUS → 兜底检查，而那一刻
    // 磁盘身份还没刷新（重新加载/保留内容还没执行），第二次询问就会冒出来
    bool externalChangePromptOpen_ = false;
    bool crashTest_ = false;
    std::unique_ptr<core::DocumentManager> documents_;
    TabBar                     tabBar_;
    FolderPanel                folderPanel_;
    FunctionListPanel          functionListPanel_;
    plugin::PluginHost         plugins_;
    std::unique_ptr<infra::ThreadPool> functionPool_;
    std::uint64_t              functionRequestId_ = 0;
    bool                       functionListVisible_ = false;
    // 更新检查：线程池单独一个（函数列表忙的时候不该把检查也堵住）；
    // updateBusy_ 是"已有一次检查在跑"的共享标志——手动检查与启动自动检查共用
    std::unique_ptr<infra::ThreadPool> updatePool_;
    std::shared_ptr<std::atomic<bool>> updateBusy_ =
        std::make_shared<std::atomic<bool>>(false);
    StatusBar                  statusBar_;
    FindBar                    findBar_;
    SearchPanel                searchPanel_;
    std::unique_ptr<search::CrossFileSearch> crossSearch_;
    std::shared_ptr<search::CrossSearchJob>  searchJob_;
    std::unique_ptr<search::CrossFileReplace> crossReplace_;
    std::shared_ptr<search::CrossSearchJob>   replaceJob_;
    search::SearchConfig                      pendingReplaceConfig_;
    search::ReplaceOptions                    replaceOptions_;
    std::unique_ptr<io::FileWatcher>          fileWatcher_;

    std::wstring lastDirectory_;
    bool showWhitespace_ = false;
    bool showEol_ = false;
    bool showLineNumbers_ = true;
    bool wrapEnabled_ = false;
    bool darkMode_ = false;
    // 护眼模式（低蓝光）：与暗色模式叠加——浅 / 深两种主题下各有一套护眼配色
    bool eyeCare_ = false;
    // 工具栏（设计文档 2.11 第 6 条）：按钮表存的是命令键，见 core::ToolbarItems
    ToolBar             toolbar_;
    core::ToolbarItems  toolbarItems_;
    bool                toolbarVisible_ = true;
    // 右键菜单项表（M3 第九块"右键菜单深度定制"）：与工具栏同一套键表格式，
    // 弹出时按注册表逐条解析（见 ShowEditorContextMenu）
    core::ToolbarItems  contextMenuItems_;
    // 全屏（F11）：进出时把菜单、样式、窗口位置原样存下来，退出时逐项还原——
    // 只记一个"是不是全屏"的布尔量是不够的，还原时要用的东西必须一起留着
    bool            fullScreen_ = false;
    WINDOWPLACEMENT fullScreenPlacement_ = {};
    LONG_PTR        fullScreenStyle_ = 0;
    LONG_PTR        fullScreenExStyle_ = 0;
    // 宏：录到的步骤、上次录制/执行的宏（"播放"与"重复执行"都作用在它上面）、
    // 保存在 shortcuts.xml 里的宏表由 shortcutStore_ 持有（一份文件只留一个写者）
    core::MacroRecorder   macroRecorder_;
    core::Macro           lastMacro_;
    core::ShortcutStore   shortcutStore_;
    std::size_t           macroCommandBase_ = 0;   // 宏命令 ID 的起始序号（8 类里从 128 起）
    // 分屏：左右两个视图共用同一份文档，比例与"在哪一侧编辑"随会话保存
    bool          splitEnabled_ = false;
    int           splitRatioPercent_ = 50;
    bool          splitScrollSync_ = true;
    bool          splitFocusSecondary_ = false;
    bool          splitDragging_ = false;
    mutable RECT  dividerRect_ = {};
    mutable RECT  viewRect_ = {};   // 编辑区（布局时算一次，切换十六进制视图时复用）
};

}  // namespace npcc::shell
