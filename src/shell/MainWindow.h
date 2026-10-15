#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/CommandRegistry.h"
#include "core/DocumentManager.h"
#include "core/SessionManager.h"
#include "edit/Document.h"
#include "edit/EditView.h"
#include "shell/StatusBar.h"
#include "shell/TabBar.h"
#include "shell/FindBar.h"

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

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void RegisterCommands();
    void RebuildAccelerators();
    void LayoutChildren() const;
    void UpdateTitle() const;
    void UpdateStatus() const;
    void SyncTabBar();
    void ApplyViewToggles(edit::EditView& view) const;
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
    void CommandAbout();

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
    std::unique_ptr<core::DocumentManager> documents_;
    TabBar                     tabBar_;
    StatusBar                  statusBar_;
    FindBar                    findBar_;

    std::wstring lastDirectory_;
    bool showWhitespace_ = false;
    bool showEol_ = false;
    bool showLineNumbers_ = true;
    bool wrapEnabled_ = false;
};

}  // namespace npcc::shell
