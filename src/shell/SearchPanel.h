#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "search/SearchConfig.h"
#include "search/SearchResult.h"

namespace npcc::shell {

// 在文件中查找面板的控件 ID，与查找栏（900 段）分开。
enum SearchPanelControl : int {
    kSearchPanelBase   = 700,
    kSearchPanelFind   = 701,
    kSearchPanelDir    = 702,
    kSearchPanelFilter = 703,
    kSearchPanelCase   = 704,
    kSearchPanelWord   = 705,
    kSearchPanelRegex  = 706,
    kSearchPanelStart  = 707,
    kSearchPanelStop   = 708,
    kSearchPanelClose  = 709,
    kSearchPanelList   = 710,
    kSearchPanelStatus = 711,
    kSearchPanelLimit  = 799,
};

// "在文件中查找"的面板：上方是搜索条件，下方是结果列表。
class SearchPanel {
public:
    // 结果列表最多容纳多少行，防止把界面拖垮
    static constexpr std::size_t kMaxRows = 50000;

    bool Create(HWND parent);
    void Destroy();

    HWND Handle() const noexcept { return hwnd_; }
    int  Height() const;

    void Show();
    void Hide();
    bool Visible() const;

    // 从控件读出搜索条件（不含 pattern 的编码相关字段）
    search::SearchConfig Config() const;
    std::wstring Pattern() const;
    void SetPattern(const std::wstring& text);
    void SetDirectory(const std::wstring& directory);
    void SetFilter(const std::wstring& filter);
    void SetDefaultFilter();

    void ClearResults();
    void AppendFileResult(const search::FileSearchResult& result);
    void SetStatus(const std::wstring& text);
    void SetRunning(bool running);

    // 双击结果行时，父窗口用它取出要跳转的目标
    bool SelectedTarget(std::wstring& path, int& line) const;

    static bool IsSearchPanelControl(int controlId) {
        return controlId >= kSearchPanelBase && controlId <= kSearchPanelLimit;
    }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    void Layout();
    void ApplyFont();
    HWND MakeControl(const wchar_t* className, DWORD style, DWORD exStyle, int id,
                     const wchar_t* cue);
    void SetupColumns();
    std::wstring TextOf(int controlId) const;
    void SetTextOf(int controlId, const std::wstring& text);
    void OnListDoubleClick();

    struct RowRef {
        std::wstring path;
        int          line = 0;
    };

    HWND                  hwnd_ = nullptr;
    HFONT                 font_ = nullptr;
    HWND                  list_ = nullptr;
    HWND                  status_ = nullptr;
    std::vector<RowRef>   rows_;
    bool                  truncated_ = false;
};

}  // namespace npcc::shell
