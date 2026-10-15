#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "search/CrossFileReplace.h"
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
    kSearchPanelReplaceEdit = 712,
    kSearchPanelReplacePreview = 713,
    kSearchPanelApply = 714,
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

    // ---- 替换预览模式 ----
    // 预览时列表变成"一行一个文件 + 勾选框"，用户可逐条取消。
    void BeginReplacePreview();
    void AppendReplacePlan(const search::FileReplacePlan& plan);
    void EndReplacePreview();
    bool ReplacePreviewActive() const noexcept { return replacePreview_; }
    std::vector<search::FileReplacePlan> CheckedPlans() const;

    std::wstring ReplacementText() const;
    void SetStatus(const std::wstring& text);
    void SetRunning(bool running);
    void SetApplyEnabled(bool enabled);

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
        std::size_t  planIndex = 0;
    };

    HWND                  hwnd_ = nullptr;
    HFONT                 font_ = nullptr;
    HWND                  list_ = nullptr;
    HWND                  status_ = nullptr;
    std::vector<RowRef>   rows_;
    bool                  truncated_ = false;
    bool                  replacePreview_ = false;
    std::vector<search::FileReplacePlan> plans_;
};

}  // namespace npcc::shell
