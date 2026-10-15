#pragma once

#include <windows.h>

#include <string>

#include "core/CommandRegistry.h"
#include "edit/DocBuffer.h"
#include "edit/EditView.h"
#include "shell/StatusBar.h"

namespace npcc::shell {

class MainWindow {
public:
    static constexpr const wchar_t* kClassName = L"notepadccMainWindow";

    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool RegisterWindowClass(HINSTANCE instance) const;
    bool Create(HINSTANCE instance, const std::wstring& initialFile);

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
    core::CmdContext Context();

    // ---- 命令实现 ----
    void CommandNew();
    void CommandOpen();
    bool CommandSave();
    bool CommandSaveAs();
    void CommandAbout();

    bool OpenPath(const std::wstring& path);
    bool SaveToPath(const std::wstring& path);
    bool ConfirmDiscardChanges();
    void ResetDocument();

    HINSTANCE             instance_ = nullptr;
    HWND                  hwnd_ = nullptr;
    HMENU                 menu_ = nullptr;
    HACCEL                accel_ = nullptr;
    edit::EditView        view_;
    edit::DocBuffer       doc_;
    StatusBar             statusBar_;
    core::CommandRegistry registry_;

    uint16_t     nextBufferId_ = 0;
    std::wstring lastDirectory_;
    bool         showWhitespace_ = false;
    bool         showEol_ = false;
    bool         showLineNumbers_ = true;
    bool         wrapEnabled_ = false;
};

}  // namespace npcc::shell
