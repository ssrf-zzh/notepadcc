#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "search/InDocSearch.h"
#include "shell/ChromeStyle.h"

namespace npcc::shell {

// 查找栏的控件 ID。统一放在 900 以上，父窗口据此判断 WM_COMMAND 是不是发给查找栏的。
enum FindBarControl : int {
    kFindBarBase     = 900,
    kFindEdit        = 901,
    kReplaceEdit     = 902,
    kModeCombo       = 903,
    kCaseCheck       = 904,
    kWordCheck       = 905,
    kDotAllCheck     = 906,
    kFindPrevButton  = 907,
    kFindNextButton  = 908,
    kCountButton     = 909,
    kMarkButton      = 910,
    kCloseButton     = 911,
    kReplaceButton   = 912,
    kReplaceAllButton= 913,
    kFindLabel       = 914,
    kReplaceLabel    = 915,
    kFindBarLimit    = 999,
};

// 底部查找栏（设计文档 2.5 的交互载体）。
// 用普通控件按 DPI 手工排布，不依赖对话框模板——模板里的中文会牵扯 .rc 的编码问题。
class FindBar {
public:
    bool Create(HWND parent);
    void Destroy();

    // 跟随主窗口的四套主题（浅 / 深 / 护眼浅 / 护眼深）。输入框和下拉列表走
    // uxtheme 的深色主题，按钮 / 复选框自绘——系统按钮在深色底上会留一块浅灰。
    void SetTheme(bool dark, bool eyeCare);

    HWND Handle() const noexcept { return hwnd_; }
    int  Height() const;

    void Show(bool replaceMode);
    void Hide();
    bool Visible() const;
    bool ReplaceMode() const noexcept { return replaceMode_; }

    // 从控件读出当前搜索条件
    search::SearchConfig Config(bool utfMode, std::uint32_t codePage) const;

    void SetStatus(const std::wstring& text);
    void FocusFindField();
    // 注意不要叫 FindText：windows.h 里有 `#define FindText FindTextW`，
    // 那个宏会把成员名悄悄改写掉（踩过一次）。
    std::wstring Pattern() const;
    void SetPattern(const std::wstring& text);

    static bool IsFindBarControl(int controlId) {
        return controlId >= kFindBarBase && controlId <= kFindBarLimit;
    }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int    Scale(int value) const;
    void   Layout();
    struct Placement {
        HWND handle = nullptr;
        RECT rect{};
    };
    std::vector<Placement> BuildPlacements(int width) const;
    void   ApplyFont();
    void   UpdateThemeResources();
    void   ApplyThemeToControls();
    void   InstallButtonSubclasses();
    void   PaintBackground(HDC dc) const;
    void   PaintControl(HWND control, HDC dc) const;
    void   PaintCombo(HDC dc) const;
    ChromeColours Theme() const;
    static LRESULT CALLBACK ButtonSubclass(HWND hwnd, UINT message, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR id, DWORD_PTR ref);
    LRESULT ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ComboSubclass(HWND hwnd, UINT message, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR id, DWORD_PTR ref);
    LRESULT ComboSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    HWND   MakeControl(const wchar_t* className, const wchar_t* text, DWORD style, int id);
    std::wstring TextOf(int controlId) const;
    int    ModeIndex() const;

    HWND         hwnd_ = nullptr;
    HFONT        font_ = nullptr;
    bool         replaceMode_ = false;
    HWND         findEdit_ = nullptr;
    HWND         replaceEdit_ = nullptr;
    HWND         modeCombo_ = nullptr;
    HWND         statusText_ = nullptr;
    bool         dark_ = false;
    bool         eyeCare_ = false;
    HBRUSH       background_ = nullptr;
    HBRUSH       field_ = nullptr;
};

}  // namespace npcc::shell
