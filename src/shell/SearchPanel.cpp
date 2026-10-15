#include "shell/SearchPanel.h"

#include <commctrl.h>

#include <algorithm>

#include "infra/Log.h"
#include "search/TextBytes.h"

namespace npcc::shell {
namespace {

constexpr int kGap = 6;
constexpr int kRowHeight = 25;
constexpr int kListHeight = 170;

void SetListSubItem(HWND list, int item, int subItem, const std::wstring& text) {
    LVITEMW entry{};
    entry.mask = LVIF_TEXT;
    entry.iItem = item;
    entry.iSubItem = subItem;
    entry.pszText = const_cast<LPWSTR>(text.c_str());
    ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(item),
                   reinterpret_cast<LPARAM>(&entry));
}

}  // namespace

bool SearchPanel::Create(HWND parent) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SearchPanel::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = L"notepadccSearchPanel";
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(0, L"notepadccSearchPanel", L"",
                              WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

    // 用 cue banner 当占位提示，比在每个输入框前面摆一个 STATIC 省一半控件
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelFind, L"查找内容");
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelDir, L"搜索目录（留空则用当前文件所在目录）");
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelFilter, L"文件过滤器：*.txt;*.cpp;!node_modules");
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelReplaceEdit, L"替换为");

    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                kSearchPanelCase, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                kSearchPanelWord, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                kSearchPanelRegex, nullptr);
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelCase), L"区分大小写");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelWord), L"全字匹配");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelRegex), L"正则");

    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelStart, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelStop, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelReplacePreview, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelApply, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelClose, nullptr);
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelStart), L"在文件中查找");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelStop), L"停止");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelReplacePreview), L"替换…");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelApply), L"应用");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelClose), L"关闭");

    list_ = MakeControl(WC_LISTVIEWW,
                        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT |
                            LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                        0, kSearchPanelList, nullptr);
    status_ = MakeControl(L"STATIC", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, kSearchPanelStatus,
                          nullptr);

    ApplyFont();
    SetupColumns();
    Layout();
    SetStatus(L"就绪");
    SetApplyEnabled(false);
    return true;
}

void SearchPanel::Destroy() {
    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != 0) {
            ::DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    rows_.clear();
}

HWND SearchPanel::MakeControl(const wchar_t* className, DWORD style, DWORD exStyle, int id,
                              const wchar_t* cue) {
    const HWND control = ::CreateWindowExW(exStyle, className, L"", style, 0, 0, 10, 10, hwnd_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           ::GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && cue != nullptr) {
        ::SendMessageW(control, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    }
    return control;
}

void SearchPanel::ApplyFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    LOGFONTW logFont{};
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0) {
        logFont = metrics.lfMessageFont;
    } else {
        ::wcscpy_s(logFont.lfFaceName, L"Microsoft YaHei UI");
        logFont.lfHeight = -12;
    }
    font_ = ::CreateFontIndirectW(&logFont);
    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    }
}

void SearchPanel::SetupColumns() {
    if (list_ == nullptr) {
        return;
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.cx = Scale(320);
    column.pszText = const_cast<LPWSTR>(L"文件");
    ::SendMessageW(list_, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.cx = Scale(60);
    column.pszText = const_cast<LPWSTR>(L"行");
    ::SendMessageW(list_, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    column.cx = Scale(520);
    column.pszText = const_cast<LPWSTR>(L"内容");
    ::SendMessageW(list_, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
}

int SearchPanel::Scale(int value) const {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr && hwnd_ != nullptr) {
            const UINT actual = getDpi(::GetParent(hwnd_));
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

int SearchPanel::Height() const {
    return Scale(kRowHeight) * 2 + Scale(kListHeight) + Scale(kGap) * 4;
}

void SearchPanel::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int rowHeight = Scale(kRowHeight);
    const int gap = Scale(kGap);
    const int row1 = gap;
    const int row2 = gap * 2 + rowHeight;
    const int listTop = row2 + rowHeight + gap;

    auto place = [&](HWND control, int x, int y, int w, int h) {
        if (control != nullptr) {
            ::MoveWindow(control, x, y, w, h, TRUE);
        }
    };

    const int buttonWidth = Scale(92);
    const int smallButton = Scale(60);
    const int checkWidth = Scale(88);
    const int replaceButtonWidth = Scale(68);
    int x = gap;

    // 第一行：查找内容 + 选项 + 操作按钮
    const int reserved = checkWidth * 3 + buttonWidth + replaceButtonWidth * 2 +
                         smallButton + gap * 10;
    const int findWidth = std::max(Scale(160), width - reserved - gap);
    place(::GetDlgItem(hwnd_, kSearchPanelFind), x, row1, findWidth, rowHeight);
    x += findWidth + gap;
    for (const int id : {kSearchPanelCase, kSearchPanelWord, kSearchPanelRegex}) {
        place(::GetDlgItem(hwnd_, id), x, row1, checkWidth, rowHeight);
        x += checkWidth;
    }
    x += gap;
    place(::GetDlgItem(hwnd_, kSearchPanelStart), x, row1, buttonWidth, rowHeight);
    x += buttonWidth + gap;
    place(::GetDlgItem(hwnd_, kSearchPanelReplacePreview), x, row1, replaceButtonWidth,
          rowHeight);
    x += replaceButtonWidth + gap;
    place(::GetDlgItem(hwnd_, kSearchPanelApply), x, row1, replaceButtonWidth, rowHeight);
    x += replaceButtonWidth + gap;
    place(::GetDlgItem(hwnd_, kSearchPanelStop), x, row1, smallButton, rowHeight);
    x += smallButton + gap;
    place(::GetDlgItem(hwnd_, kSearchPanelClose), x, row1, smallButton, rowHeight);

    // 第二行：目录 + 过滤器 + 替换为 + 状态
    x = gap;
    const int statusWidth = Scale(240);
    const int replaceWidth = Scale(200);
    const int filterWidth = Scale(240);
    const int dirWidth = std::max(
        Scale(140), width - statusWidth - filterWidth - replaceWidth - gap * 6);
    place(::GetDlgItem(hwnd_, kSearchPanelDir), x, row2, dirWidth, rowHeight);
    x += dirWidth + gap;
    place(::GetDlgItem(hwnd_, kSearchPanelFilter), x, row2, filterWidth, rowHeight);
    x += filterWidth + gap;
    place(::GetDlgItem(hwnd_, kSearchPanelReplaceEdit), x, row2, replaceWidth, rowHeight);
    place(status_, width - statusWidth - gap, row2, statusWidth, rowHeight);

    // 结果列表填满剩余空间
    const int listHeight =
        std::max(rowHeight, static_cast<int>(client.bottom) - listTop - gap);
    place(list_, gap, listTop, std::max(10, width - gap * 2), listHeight);

    // 列宽随窗口调整
    if (list_ != nullptr) {
        const int total = std::max(100, width - gap * 2 - Scale(24));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 0, total * 35 / 100);
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 1, Scale(56));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 2, total * 60 / 100);
    }
}

void SearchPanel::Show() {
    Layout();
    ::ShowWindow(hwnd_, SW_SHOW);
}

void SearchPanel::Hide() {
    ::ShowWindow(hwnd_, SW_HIDE);
}

bool SearchPanel::Visible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) != 0;
}

std::wstring SearchPanel::TextOf(int controlId) const {
    const HWND control = ::GetDlgItem(hwnd_, controlId);
    if (control == nullptr) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void SearchPanel::SetTextOf(int controlId, const std::wstring& text) {
    if (HWND control = ::GetDlgItem(hwnd_, controlId); control != nullptr) {
        ::SetWindowTextW(control, text.c_str());
    }
}

search::SearchConfig SearchPanel::Config() const {
    search::SearchConfig config;
    config.pattern = TextOf(kSearchPanelFind);
    config.replaceTo = TextOf(kSearchPanelReplaceEdit);
    config.matchCase = ::IsDlgButtonChecked(hwnd_, kSearchPanelCase) == BST_CHECKED;
    config.wholeWordOnly = ::IsDlgButtonChecked(hwnd_, kSearchPanelWord) == BST_CHECKED;
    config.mode = (::IsDlgButtonChecked(hwnd_, kSearchPanelRegex) == BST_CHECKED)
                      ? search::SearchMode::Regex
                      : search::SearchMode::Literal;
    config.filter = TextOf(kSearchPanelFilter);
    if (config.filter.empty()) {
        config.filter = L"*.*";
    }
    const std::wstring directory = TextOf(kSearchPanelDir);
    if (!directory.empty()) {
        config.directories.push_back(directory);
    }
    return config;
}

std::wstring SearchPanel::Pattern() const {
    return TextOf(kSearchPanelFind);
}

void SearchPanel::SetPattern(const std::wstring& text) {
    SetTextOf(kSearchPanelFind, text);
}

void SearchPanel::SetDirectory(const std::wstring& directory) {
    if (TextOf(kSearchPanelDir).empty()) {
        SetTextOf(kSearchPanelDir, directory);
    }
}

void SearchPanel::SetFilter(const std::wstring& filter) {
    SetTextOf(kSearchPanelFilter, filter);
}

void SearchPanel::SetDefaultFilter() {
    if (TextOf(kSearchPanelFilter).empty()) {
        SetTextOf(kSearchPanelFilter, L"*.*");
    }
}

void SearchPanel::ClearResults() {
    rows_.clear();
    truncated_ = false;
    plans_.clear();
    replacePreview_ = false;
    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        // 回到"查找结果"的列定义
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT;
        column.pszText = const_cast<LPWSTR>(L"文件");
        ::SendMessageW(list_, LVM_SETCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        column.pszText = const_cast<LPWSTR>(L"行");
        ::SendMessageW(list_, LVM_SETCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
        column.pszText = const_cast<LPWSTR>(L"内容");
        ::SendMessageW(list_, LVM_SETCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, 0);
    }
    SetApplyEnabled(false);
}

void SearchPanel::BeginReplacePreview() {
    ClearResults();
    replacePreview_ = true;
    if (list_ == nullptr) {
        return;
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT;
    column.pszText = const_cast<LPWSTR>(L"文件");
    ::SendMessageW(list_, LVM_SETCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.pszText = const_cast<LPWSTR>(L"处数");
    ::SendMessageW(list_, LVM_SETCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    column.pszText = const_cast<LPWSTR>(L"替换预览（改前 → 改后）");
    ::SendMessageW(list_, LVM_SETCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
    ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
}

void SearchPanel::AppendReplacePlan(const search::FileReplacePlan& plan) {
    if (list_ == nullptr || !replacePreview_) {
        return;
    }
    if (plans_.size() >= kMaxRows) {
        if (!truncated_) {
            truncated_ = true;
            SetStatus(L"文件过多，预览已截断");
        }
        return;
    }

    const std::size_t planIndex = plans_.size();
    plans_.push_back(plan);

    // 预览列显示第一处改动的前后对照；出错的文件把原因写在这里
    std::wstring detail;
    if (!plan.error.empty()) {
        detail = L"⚠ " + plan.error;
    } else if (!plan.samples.empty()) {
        detail = search::ConvertFromDocBytes(plan.samples[0].before, true, 936);
        detail += L"  →  ";
        detail += search::ConvertFromDocBytes(plan.samples[0].after, true, 936);
    }
    if (detail.size() > 200) {
        detail.resize(200);
        detail += L"…";
    }

    const std::wstring countText =
        plan.error.empty() ? std::to_wstring(plan.replacements) : std::wstring(L"—");
    const std::wstring displayName = plan.path;

    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(planIndex);
    item.iSubItem = 0;
    item.pszText = const_cast<LPWSTR>(displayName.c_str());
    item.lParam = static_cast<LPARAM>(planIndex);
    const int inserted = static_cast<int>(
        ::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
    if (inserted < 0) {
        plans_.pop_back();
        return;
    }
    SetListSubItem(list_, inserted, 1, countText);
    SetListSubItem(list_, inserted, 2, detail);

    // 默认勾选；出错的文件取消勾选并置灰
    const UINT state = plan.selected ? 2u : 1u;
    LVITEMW stateItem{};
    stateItem.mask = LVIF_STATE;
    stateItem.stateMask = LVIS_STATEIMAGEMASK;
    stateItem.state = INDEXTOSTATEIMAGEMASK(state);
    ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(inserted),
                   reinterpret_cast<LPARAM>(&stateItem));

    rows_.push_back(RowRef{plan.path, 0, planIndex});
}

void SearchPanel::EndReplacePreview() {
    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 0, Scale(320));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 1, Scale(60));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 2, Scale(520));
    }
    SetApplyEnabled(!plans_.empty());
}

std::vector<search::FileReplacePlan> SearchPanel::CheckedPlans() const {
    std::vector<search::FileReplacePlan> selected;
    if (list_ == nullptr || !replacePreview_) {
        return selected;
    }
    const int count = static_cast<int>(::SendMessageW(list_, LVM_GETITEMCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        const LRESULT state = ::SendMessageW(list_, LVM_GETITEMSTATE,
                                             static_cast<WPARAM>(index), LVIS_STATEIMAGEMASK);
        const bool checked = ((state >> 12) & 0xF) == 2;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = index;
        if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == 0) {
            continue;
        }
        const std::size_t planIndex = static_cast<std::size_t>(item.lParam);
        if (planIndex >= plans_.size()) {
            continue;
        }
        search::FileReplacePlan plan = plans_[planIndex];
        plan.selected = checked && plan.error.empty();
        selected.push_back(std::move(plan));
    }
    return selected;
}

std::wstring SearchPanel::ReplacementText() const {
    return TextOf(kSearchPanelReplaceEdit);
}

void SearchPanel::SetApplyEnabled(bool enabled) {
    if (HWND control = ::GetDlgItem(hwnd_, kSearchPanelApply); control != nullptr) {
        ::EnableWindow(control, enabled ? TRUE : FALSE);
    }
}

void SearchPanel::AppendFileResult(const search::FileSearchResult& result) {
    if (list_ == nullptr || result.hits.empty()) {
        return;
    }
    const std::wstring displayName = result.path;

    for (const search::LineHit& hit : result.hits) {
        if (rows_.size() >= kMaxRows) {
            if (!truncated_) {
                truncated_ = true;
                SetStatus(L"结果过多，已停止追加");
            }
            return;
        }

        // 行文本转成宽字符显示；过长时截断，免得列表被单行撑爆
        std::wstring text = search::ConvertFromDocBytes(hit.text, true, 936);
        if (text.size() > 400) {
            text.resize(400);
            text += L"…";
        }
        for (wchar_t& ch : text) {
            if (ch == L'\t') {
                ch = L' ';
            }
        }

        const int index = static_cast<int>(rows_.size());
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = index;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(displayName.c_str());
        item.lParam = index;
        const int inserted = static_cast<int>(::SendMessageW(list_, LVM_INSERTITEMW, 0,
                                                             reinterpret_cast<LPARAM>(&item)));
        if (inserted < 0) {
            return;
        }
        const std::wstring lineText = std::to_wstring(hit.line);
        SetListSubItem(list_, inserted, 1, lineText);
        SetListSubItem(list_, inserted, 2, text);
        rows_.push_back(RowRef{result.path, hit.line});
    }
}

void SearchPanel::SetStatus(const std::wstring& text) {
    if (status_ != nullptr) {
        ::SetWindowTextW(status_, text.c_str());
    }
}

void SearchPanel::SetRunning(bool running) {
    if (HWND control = ::GetDlgItem(hwnd_, kSearchPanelStart); control != nullptr) {
        ::EnableWindow(control, running ? FALSE : TRUE);
    }
    if (HWND control = ::GetDlgItem(hwnd_, kSearchPanelStop); control != nullptr) {
        ::EnableWindow(control, running ? TRUE : FALSE);
    }
    for (const int id : {kSearchPanelReplacePreview, kSearchPanelApply}) {
        if (HWND control = ::GetDlgItem(hwnd_, id); control != nullptr) {
            ::EnableWindow(control, running ? FALSE : TRUE);
        }
    }
    if (!running) {
        SetApplyEnabled(replacePreview_ && !plans_.empty());
    }
}

bool SearchPanel::SelectedTarget(std::wstring& path, int& line) const {
    if (list_ == nullptr) {
        return false;
    }
    const int selected = static_cast<int>(::SendMessageW(list_, LVM_GETNEXTITEM,
                                                         static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (selected < 0) {
        return false;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == 0) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(item.lParam);
    if (index >= rows_.size()) {
        return false;
    }
    path = rows_[index].path;
    line = rows_[index].line;
    return true;
}

void SearchPanel::OnListDoubleClick() {
    // 交给父窗口去打开文件并跳转
    ::SendMessageW(::GetParent(hwnd_), WM_COMMAND,
                   MAKEWPARAM(kSearchPanelList, 0), 0);
}

LRESULT CALLBACK SearchPanel::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    SearchPanel* self = reinterpret_cast<SearchPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SearchPanel*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT SearchPanel::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == kSearchPanelList && header->code == NM_DBLCLK) {
                OnListDoubleClick();
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            ::SendMessageW(::GetParent(hwnd_), WM_COMMAND, wParam, lParam);
            return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
