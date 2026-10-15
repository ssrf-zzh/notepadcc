#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/CommandRegistry.h"
#include "core/DocumentManager.h"
#include "core/ConfigCenter.h"
#include "core/SessionManager.h"
#include "core/SnapshotStore.h"
#include "edit/Document.h"
#include "edit/EditView.h"
#include "edit/LanguageRegistry.h"
#include "infra/CrashGuard.h"
#include "shell/StatusBar.h"
#include "shell/TabBar.h"
#include "shell/FindBar.h"
#include "shell/SearchPanel.h"
#include "search/CrossFileSearch.h"
#include "search/CrossFileReplace.h"
#include "io/FileWatcher.h"

namespace npcc::shell {

class MainWindow {
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
    void CommandExportLanguageDefs();
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
    // 分屏：左右两个视图共用同一份文档，比例与"在哪一侧编辑"随会话保存
    bool          splitEnabled_ = false;
    int           splitRatioPercent_ = 50;
    bool          splitScrollSync_ = true;
    bool          splitFocusSecondary_ = false;
    bool          splitDragging_ = false;
    mutable RECT  dividerRect_ = {};
};

}  // namespace npcc::shell
