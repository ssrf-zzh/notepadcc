#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/CommandRegistry.h"
#include "core/DocumentManager.h"
#include "core/Macro.h"
#include "core/RunCommand.h"
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
#include "shell/StatusBar.h"
#include "shell/TabBar.h"
#include "shell/FolderPanel.h"
#include "shell/FunctionListPanel.h"
#include "shell/HexView.h"
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

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void RegisterCommands();
    void RebuildMenu();
    void RebuildAccelerators();
    void LayoutChildren() const;
    void UpdateTitle() const;
    void UpdateStatus() const;
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
    void CommandToggleSplit();
    void CommandCyclePane();
    void CommandConvertEol(edit::EolMode target);
    void CommandConvertEncoding(edit::UniMode target);
    // 行操作与文本转换（设计文档 2.13）。tool 接收"选区或整篇"的字节与当前代码页。
    void ApplyTextTool(
        const std::function<std::string(std::string_view, std::uint32_t)>& tool,
        const wchar_t* description);
    void CommandDecodeText(bool base64);
    void CommandToggleComment(bool block);
    // JSON / XML 格式化（设计文档 2.13）。这两件事会失败（内容不合法），
    // 所以不能走 ApplyTextTool 那条"永不失败"的入口：先试一次，失败只弹说明。
    void CommandFormatStructured(bool json, bool minify);
    void CommandExportLanguageDefs();
    void CommandStyleConfigurator();
    void CommandExportUdlSample();
    void CommandToggleDarkMode();
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
    // 正在跑的那条命令（捕获输出时才有：不捕获就是"丢出去不管"）
    struct PendingRun {
        HANDLE        process = nullptr;
        std::wstring  outputPath;      // 捕获时的临时文件
        std::uint64_t startedMs = 0;
    };
    PendingRun   pendingRun_;
    bool         pendingRunActive_ = false;
    std::wstring lastRunCommand_;   // 下次打开 F5 时预填
    int          runOutputCounter_ = 0;
    int          runTempCounter_ = 0;
    // 十六进制视图：一次只看一个文件（它读的是磁盘字节，不是缓冲区）
    HexView      hexView_;
    bool         hexMode_ = false;
    core::BufferId hexDocument_ = 0;   // hexMode_ 为真时正在看哪个文档
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
    void ScheduleFunctionParse();          // 节流之后的解析请求
    void StartFunctionParse();             // 真正提交后台解析（由 WM_TIMER 触发）
    void HandleFunctionNotify(const FunctionNotify& notification);
    void ApplyFunctionSymbols(std::uint64_t requestId,
                              std::vector<core::FunctionSymbol> symbols);
    // 打开一个具体路径（工作区双击、单实例转交都用它，免得两处各写一遍）
    bool OpenDocumentPath(const std::wstring& path);
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
    bool crashTest_ = false;
    std::unique_ptr<core::DocumentManager> documents_;
    TabBar                     tabBar_;
    FolderPanel                folderPanel_;
    FunctionListPanel          functionListPanel_;
    plugin::PluginHost         plugins_;
    std::unique_ptr<infra::ThreadPool> functionPool_;
    std::uint64_t              functionRequestId_ = 0;
    bool                       functionListVisible_ = false;
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
