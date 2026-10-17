#include "shell/ShortcutDialog.h"

#include <commctrl.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "core/CommandText.h"
#include "core/ShortcutStore.h"
#include "shell/ChromeStyle.h"
#include "shell/MessageDialog.h"

namespace npcc::shell {
namespace {

constexpr int            kGap = 8;
constexpr int            kRow = 24;
constexpr const wchar_t* kWindowClass = L"notepadccShortcutDialog";

enum ShortcutControl : int {
    kShFilterLabel = 1100,
    kShFilterEdit  = 1101,
    kShList        = 1102,
    kShStatus      = 1103,
    kShHint        = 1104,
    kShDelete      = 1110,
    kShRestore     = 1111,
    kShClearAll    = 1112,
    kShOk          = 1120,
    kShCancel      = 1121,
};

enum ShortcutColumn : int {
    kColCategory = 0,
    kColCommand  = 1,
    kColShortcut = 2,
};

int DpiOf(HWND reference) {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr && reference != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr) {
            const UINT actual = getDpi(reference);
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return static_cast<int>(dpi);
}

std::wstring LowerOf(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

class ShortcutDialog {
public:
    bool Show(HWND owner, npcc::core::CommandRegistry& registry,
              const std::vector<npcc::core::ShortcutBinding>& defaults,
              ShortcutEditResult* result);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ListProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT ListProc(HWND list, UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
              DWORD exStyle = 0);
    HWND ListBox() const { return ::GetDlgItem(hwnd_, kShList); }
    std::wstring TextOf(int id) const;

    void BuildControls();
    void Layout() const;
    void RefreshList();
    void InsertRow(int row, const std::wstring& category, const std::wstring& name,
                   const std::wstring& keys, npcc::core::CmdId id);
    void     SelectRow(int row) const;
    npcc::core::CmdId SelectedCommand() const;
    std::wstring      NameOf(npcc::core::CmdId id) const;

    void Capture(uint8_t modifiers, uint16_t keyCode);
    void DeleteSelected();
    void RestoreDefaults();
    void ClearAll();
    void SetStatus(const std::wstring& text) const;

    HWND                                       owner_ = nullptr;
    HWND                                       hwnd_ = nullptr;
    HFONT                                      font_ = nullptr;
    WNDPROC                                    listProc_ = nullptr;
    bool                                       done_ = false;
    bool                                       applied_ = false;
    bool                                       changed_ = false;
    npcc::core::CommandRegistry*               registry_ = nullptr;
    const std::vector<npcc::core::ShortcutBinding>* defaults_ = nullptr;
    std::vector<npcc::core::ShortcutBinding>   snapshot_;
    ShortcutEditResult*                        result_ = nullptr;
};

int ShortcutDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND ShortcutDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                          DWORD exStyle) {
    return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

std::wstring ShortcutDialog::TextOf(int id) const {
    const HWND control = ::GetDlgItem(hwnd_, id);
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

std::wstring ShortcutDialog::NameOf(npcc::core::CmdId id) const {
    const npcc::core::Command* command = registry_->Find(id);
    return command != nullptr ? npcc::core::StripAccelerator(command->text) : std::wstring();
}

void ShortcutDialog::BuildControls() {
    Make(L"STATIC", L"筛选：", WS_VISIBLE, kShFilterLabel);
    Make(L"EDIT", L"", WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kShFilterEdit);

    HWND list = Make(WC_LISTVIEWW, L"",
                     WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                         LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                     kShList);
    if (list != nullptr) {
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        struct Column {
            const wchar_t* title;
            int            width;
        };
        const Column columns[] = {
            {L"类别", 80}, {L"命令", 300}, {L"快捷键", 190},
        };
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(columns[i].title);
            column.cx = Scale(columns[i].width);
            column.iSubItem = i;
            ListView_InsertColumn(list, i, &column);
        }
    }

    Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kShStatus);
    Make(L"STATIC",
         L"选中一条命令后直接按下新的组合键即可改绑；方向键与 Home/End 留给列表导航。"
         L"可绑定的组合需带 Ctrl 或 Alt，或使用 F1~F24 / Insert / Delete。",
         WS_VISIBLE | SS_LEFT, kShHint);

    Make(L"BUTTON", L"删除快捷键", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kShDelete);
    Make(L"BUTTON", L"恢复默认", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kShRestore);
    Make(L"BUTTON", L"全部清除", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kShClearAll);
    Make(L"BUTTON", L"确定", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kShOk);
    Make(L"BUTTON", L"取消", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kShCancel);
    (void)::SendMessageW(hwnd_, DM_SETDEFID, kShOk, 0);
}

void ShortcutDialog::Layout() const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(kGap);
    const int rowHeight = Scale(kRow);
    const int buttonHeight = Scale(30);
    const int buttonWidth = Scale(110);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    int y = gap;
    if (HWND label = ::GetDlgItem(hwnd_, kShFilterLabel); label != nullptr) {
        ::MoveWindow(label, gap, y + Scale(4), Scale(50), rowHeight, TRUE);
    }
    if (HWND edit = ::GetDlgItem(hwnd_, kShFilterEdit); edit != nullptr) {
        ::MoveWindow(edit, gap + Scale(54), y, Scale(240), rowHeight, TRUE);
    }
    y += rowHeight + gap;

    const int statusHeight = rowHeight * 2;
    const int hintHeight = rowHeight * 2;
    const int bottom = gap + buttonHeight + gap + hintHeight + gap + statusHeight;
    const int listHeight = std::max(Scale(120), height - y - bottom);
    if (HWND list = ::GetDlgItem(hwnd_, kShList); list != nullptr) {
        ::MoveWindow(list, gap, y, width - gap * 2, listHeight, TRUE);
    }
    y += listHeight + gap;

    if (HWND status = ::GetDlgItem(hwnd_, kShStatus); status != nullptr) {
        ::MoveWindow(status, gap, y, width - gap * 2, statusHeight, TRUE);
    }
    y += statusHeight + gap;
    if (HWND hint = ::GetDlgItem(hwnd_, kShHint); hint != nullptr) {
        ::MoveWindow(hint, gap, y, width - gap * 2, hintHeight, TRUE);
    }

    const int buttonY = height - buttonHeight - gap;
    int left = gap;
    for (const int id : {kShDelete, kShRestore, kShClearAll}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, left, buttonY, buttonWidth, buttonHeight, TRUE);
            left += buttonWidth + gap / 2;
        }
    }
    int right = width - gap - buttonWidth;
    for (const int id : {kShCancel, kShOk}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, right, buttonY, buttonWidth, buttonHeight, TRUE);
            right -= buttonWidth + gap / 2;
        }
    }
}

void ShortcutDialog::InsertRow(int row, const std::wstring& category, const std::wstring& name,
                               const std::wstring& keys, npcc::core::CmdId id) {
    HWND list = ListBox();
    if (list == nullptr) {
        return;
    }
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row;
    item.iSubItem = kColCategory;
    item.pszText = const_cast<wchar_t*>(category.c_str());
    item.lParam = static_cast<LPARAM>(id);
    const LRESULT index = ::SendMessageW(list, LVM_INSERTITEMW, 0,
                                         reinterpret_cast<LPARAM>(&item));
    if (index < 0) {
        return;
    }
    auto setCell = [&](int subItem, const std::wstring& text) {
        LVITEMW cell{};
        cell.mask = LVIF_TEXT;
        cell.iItem = static_cast<int>(index);
        cell.iSubItem = subItem;
        cell.pszText = const_cast<wchar_t*>(text.c_str());
        ::SendMessageW(list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&cell));
    };
    setCell(kColCommand, name);
    setCell(kColShortcut, keys);
}

void ShortcutDialog::SelectRow(int row) const {
    HWND list = ListBox();
    if (list == nullptr) {
        return;
    }
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, row, FALSE);
}

npcc::core::CmdId ShortcutDialog::SelectedCommand() const {
    HWND list = ListBox();
    if (list == nullptr) {
        return 0;
    }
    const LRESULT row = ::SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                       LVNI_SELECTED);
    if (row < 0) {
        return 0;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = static_cast<int>(row);
    if (::SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == 0) {
        return 0;
    }
    return static_cast<npcc::core::CmdId>(item.lParam);
}

void ShortcutDialog::RefreshList() {
    HWND list = ListBox();
    if (list == nullptr) {
        return;
    }
    const npcc::core::CmdId keep = SelectedCommand();
    ::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);

    const std::wstring filter = LowerOf(TextOf(kShFilterEdit));
    int row = 0;
    int selectRow = -1;
    for (const npcc::core::Command& command : registry_->All()) {
        if ((command.flags & npcc::core::kCmdHidden) != 0) {
            continue;   // 隐藏命令不给用户绑
        }
        const std::wstring name = npcc::core::StripAccelerator(command.text);
        const std::wstring category = npcc::core::CategoryText(command.category);
        std::wstring keys;
        for (const npcc::core::ShortcutBinding& binding : registry_->BindingsOf(command.id)) {
            if (!keys.empty()) {
                keys += L" / ";
            }
            keys += npcc::core::ShortcutText(binding.modifiers, binding.keyCode);
        }
        if (!filter.empty() &&
            LowerOf(category + L"\n" + name + L"\n" + keys).find(filter) == std::wstring::npos) {
            continue;
        }
        InsertRow(row, category, name, keys, command.id);
        if (command.id == keep) {
            selectRow = row;
        }
        ++row;
    }
    if (selectRow < 0 && row > 0) {
        selectRow = 0;   // 筛选之后原来选中的行没了，就落到第一行
    }
    if (selectRow >= 0) {
        SelectRow(selectRow);
    }
}

void ShortcutDialog::SetStatus(const std::wstring& text) const {
    if (HWND status = ::GetDlgItem(hwnd_, kShStatus); status != nullptr) {
        ::SetWindowTextW(status, text.c_str());
    }
}

void ShortcutDialog::Capture(uint8_t modifiers, uint16_t keyCode) {
    const npcc::core::CmdId target = SelectedCommand();
    if (target == 0) {
        SetStatus(L"请先在列表里选中一条命令。");
        return;
    }
    const std::wstring name = NameOf(target);
    const std::wstring combo = npcc::core::ShortcutText(modifiers, keyCode);
    if (!npcc::core::IsBindableShortcut(modifiers, keyCode)) {
        SetStatus(combo + L" 不能用作快捷键：需要带 Ctrl 或 Alt，或者使用 F1~F24 / Insert / Delete。");
        return;
    }

    const std::vector<npcc::core::ShortcutBinding> own = registry_->BindingsOf(target);
    if (own.size() == 1 && own[0].modifiers == modifiers && own[0].keyCode == keyCode) {
        SetStatus(L"「" + name + L"」的快捷键已经是 " + combo + L"，没有变化。");
        return;
    }

    for (const npcc::core::CmdId owner : registry_->FindConflicts(modifiers, keyCode)) {
        if (owner == target) {
            continue;
        }
        const std::wstring otherName = NameOf(owner);
        const std::wstring question = combo + L" 已经分配给了「" + otherName + L"」。\n\n" +
                                      L"改给「" + name + L"」吗？（「" + otherName +
                                      L"」会失去这个快捷键）";
        if (ShowMessageBox(hwnd_, question.c_str(), L"快捷键冲突", MB_YESNO | MB_ICONWARNING) !=
            IDYES) {
            SetStatus(L"已取消：" + combo + L" 仍然属于「" + otherName + L"」。");
            return;
        }
        registry_->UnbindShortcut(owner, modifiers, keyCode);
    }

    registry_->AssignShortcut(target, modifiers, keyCode);
    changed_ = true;
    SetStatus(L"已把「" + name + L"」设为 " + combo + L"。");
    RefreshList();
}

void ShortcutDialog::DeleteSelected() {
    const npcc::core::CmdId target = SelectedCommand();
    if (target == 0) {
        SetStatus(L"请先在列表里选中一条命令。");
        return;
    }
    const std::wstring name = NameOf(target);
    if (registry_->BindingsOf(target).empty()) {
        SetStatus(L"「" + name + L"」本来就没有快捷键。");
        return;
    }
    registry_->ClearShortcutsOf(target);
    changed_ = true;
    SetStatus(L"已删除「" + name + L"」的快捷键。");
    RefreshList();
}

void ShortcutDialog::RestoreDefaults() {
    registry_->ClearShortcuts();
    int restored = 0;
    for (const npcc::core::ShortcutBinding& binding : *defaults_) {
        if (registry_->Find(binding.cmdId) == nullptr) {
            continue;   // 默认表里有、但现在没注册的命令（版本差异），跳过
        }
        if (registry_->BindShortcut(binding.cmdId, binding.modifiers, binding.keyCode, true)) {
            ++restored;
        }
    }
    changed_ = true;
    SetStatus(L"已恢复默认方案，共 " + std::to_wstring(restored) + L" 条绑定。");
    RefreshList();
}

void ShortcutDialog::ClearAll() {
    registry_->ClearShortcuts();
    changed_ = true;
    SetStatus(L"已清除全部快捷键绑定。");
    RefreshList();
}

bool ShortcutDialog::Show(HWND owner, npcc::core::CommandRegistry& registry,
                          const std::vector<npcc::core::ShortcutBinding>& defaults,
                          ShortcutEditResult* result) {
    owner_ = owner;
    registry_ = &registry;
    defaults_ = &defaults;
    result_ = result;
    snapshot_ = registry.AllBindings();   // 取消时按这份完整回滚

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ShortcutDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(660);
    const int height = Scale(540);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"管理快捷键",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                              nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

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

    BuildControls();
    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    ApplyDialogChrome(hwnd_);
    // 列表要自己截键盘输入，所以挂一层子类化
    if (HWND list = ListBox(); list != nullptr) {
        listProc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
            list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ShortcutDialog::ListProcThunk)));
        ::SetWindowLongPtrW(list, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    Layout();
    RefreshList();
    std::size_t bindable = 0;
    for (const npcc::core::Command& command : registry_->All()) {
        if ((command.flags & npcc::core::kCmdHidden) == 0) {
            ++bindable;
        }
    }
    SetStatus(L"共 " + std::to_wstring(bindable) + L" 条可绑定命令；确定后会写入 " +
              npcc::core::ShortcutStore::Path().wstring());

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    if (HWND list = ListBox(); list != nullptr) {
        ::SetFocus(list);
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        if (::IsDialogMessageW(hwnd_, &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    if (!applied_) {
        registry_->SetShortcuts(snapshot_);   // 取消 = 完整回滚，不留半截改动
    }
    if (result_ != nullptr) {
        result_->changed = applied_ && changed_;
    }

    ::EnableWindow(owner_, TRUE);
    ::SetForegroundWindow(owner_);
    if (HWND list = ListBox(); list != nullptr) {
        if (listProc_ != nullptr) {
            ::SetWindowLongPtrW(list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(listProc_));
            listProc_ = nullptr;
        }
        ::SetWindowLongPtrW(list, GWLP_USERDATA, 0);
    }
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    return applied_;
}

LRESULT CALLBACK ShortcutDialog::ListProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam) {
    auto* self = reinterpret_cast<ShortcutDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->ListProc(hwnd, message, wParam, lParam);
}

LRESULT ShortcutDialog::ListProc(HWND list, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        const uint16_t key = static_cast<uint16_t>(wParam);
        uint8_t modifiers = npcc::core::kModNone;
        if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            modifiers |= npcc::core::kModCtrl;
        }
        if ((::GetKeyState(VK_MENU) & 0x8000) != 0) {
            modifiers |= npcc::core::kModAlt;
        }
        if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            modifiers |= npcc::core::kModShift;
        }
        if (key == VK_ESCAPE) {
            ::PostMessageW(hwnd_, WM_COMMAND, kShCancel, 0);
            return 0;
        }
        if (key == VK_RETURN) {
            ::PostMessageW(hwnd_, WM_COMMAND, kShOk, 0);
            return 0;
        }
        // Alt+F4 是关窗口，不能被当成快捷键抢走
        const bool altF4 = (key == VK_F4) && ((modifiers & npcc::core::kModAlt) != 0);
        if (!altF4 && npcc::core::IsBindableShortcut(modifiers, key)) {
            Capture(modifiers, key);
            return 0;
        }
        // 剩下的（方向键、Home/End/PgUp/PgDn 等）留给列表自己做导航
    }
    return ::CallWindowProcW(listProc_, list, message, wParam, lParam);
}

LRESULT CALLBACK ShortcutDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                              LPARAM lParam) {
    auto* self = reinterpret_cast<ShortcutDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ShortcutDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT ShortcutDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            switch (id) {
                case kShOk:
                    applied_ = true;
                    done_ = true;
                    return 0;
                case kShCancel:
                    done_ = true;
                    return 0;
                case kShDelete:
                    DeleteSelected();
                    return 0;
                case kShRestore:
                    RestoreDefaults();
                    return 0;
                case kShClearAll:
                    ClearAll();
                    return 0;
                case kShFilterEdit:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        RefreshList();
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;   // 焦点在按钮上时也能用 Esc 取消
                return 0;
            }
            break;
        case WM_CLOSE:
            done_ = true;
            return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace

bool ShowShortcutDialog(HWND owner, npcc::core::CommandRegistry& registry,
                        const std::vector<npcc::core::ShortcutBinding>& defaults,
                        ShortcutEditResult* result) {
    ShortcutDialog dialog;
    return dialog.Show(owner, registry, defaults, result);
}

}  // namespace npcc::shell
