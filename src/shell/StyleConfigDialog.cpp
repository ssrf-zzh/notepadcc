#include "shell/StyleConfigDialog.h"

#include <commctrl.h>
#include <commdlg.h>   // ChooseColorW（系统颜色选择器）

#include <array>
#include <string>
#include <vector>

#include "edit/LanguageRegistry.h"
#include "edit/StyleCatalogue.h"
#include "edit/StyleScheme.h"

namespace npcc::shell {
namespace {

constexpr int            kGap = 8;
constexpr int            kRow = 26;
constexpr const wchar_t* kWindowClass = L"notepadccStyleConfig";

enum StyleControl : int {
    kStyleLanguageLabel = 1200,
    kStyleLanguageCombo = 1201,
    // 角色名标签。原先这些标签是用 id = -1 建的，而 Layout() 按 id 找控件摆位置——
    // 找不到就留在 (0,0) 那个 7x7 的默认尺寸上，于是**名字一直没显示出来**，
    // 界面上只剩六个色块（自动测试只看"对话框能打开"，看不见这种问题；截图才看出来）。
    kStyleRoleLabelBase = 1290,   // +role
    kStyleRowBase       = 1210,   // +role: 颜色按钮
    kStyleBoldBase      = 1230,   // +role: 粗体
    kStyleItalicBase    = 1250,   // +role: 斜体
    kStyleItemLabel     = 1260,
    kStyleList          = 1261,
    kStyleItemColour    = 1262,
    kStyleItemBold      = 1263,
    kStyleItemItalic    = 1264,
    kStyleItemReset     = 1265,
    kStyleReset         = 1270,
    kStyleOk            = 1271,
    kStyleCancel        = 1272,
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

class StyleConfigurator {
public:
    bool Show(HWND owner, const npcc::edit::LanguageRegistry& languages,
              npcc::edit::StyleScheme& scheme);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);
    void Layout();
    void LoadForLanguage(int comboIndex);
    void RefreshControls();
    void RefreshItemControls();
    void DrawColourButton(const DRAWITEMSTRUCT& item) const;
    void PickColour(int role);
    void PickItemColour();
    // 把界面上改过的值收进方案（换语言与点确定时都要做）
    void FlushToScheme();
    npcc::edit::RoleStyle DefaultRole(npcc::edit::StyleRole role) const;
    std::wstring CurrentLanguage() const;

    HWND                                       owner_ = nullptr;
    HWND                                       hwnd_ = nullptr;
    HFONT                                      font_ = nullptr;
    const npcc::edit::LanguageRegistry*        languages_ = nullptr;
    npcc::edit::StyleScheme*                   scheme_ = nullptr;
    std::array<npcc::edit::RoleStyle, npcc::edit::kStyleRoleCount> scratch_{};
    std::vector<npcc::edit::StyleEntry> entries_;      // 当前语言可逐项调整的样式号
    std::vector<npcc::edit::RoleStyle>   itemScratch_;  // 与 entries_ 一一对应
    int                                        languageIndex_ = 0;
    int                                        itemIndex_ = 0;
    bool                                       clearAll_ = false;
    bool                                       done_ = false;
    bool                                       accepted_ = false;
};

int StyleConfigurator::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND StyleConfigurator::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                             DWORD exStyle) {
    return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

std::wstring StyleConfigurator::CurrentLanguage() const {
    if (languages_ == nullptr || languageIndex_ < 0 ||
        languageIndex_ >= static_cast<int>(languages_->All().size())) {
        return L"null";
    }
    return languages_->All()[static_cast<std::size_t>(languageIndex_)]->languageName;
}

void StyleConfigurator::LoadForLanguage(int comboIndex) {
    // 换语言时先把上一个语言改过的值收进方案，免得来回切一下就丢了
    RefreshControls();
    languageIndex_ = comboIndex;
    const std::wstring language = CurrentLanguage();
    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
        scratch_[static_cast<std::size_t>(role)] =
            scheme_->Role(language, static_cast<npcc::edit::StyleRole>(role));
    }

    // 可逐项调整的样式号：当前值 = 有单独覆盖就用覆盖，否则用它所属角色的颜色。
    // 这样"没动过的项"在界面上显示的就是它此刻真正的样子。
    entries_.clear();
    itemScratch_.clear();
    if (languageIndex_ >= 0 && languageIndex_ < static_cast<int>(languages_->All().size())) {
        entries_ = npcc::edit::StylesFor(
            *languages_->All()[static_cast<std::size_t>(languageIndex_)]);
    }
    itemScratch_.reserve(entries_.size());
    for (const npcc::edit::StyleEntry& entry : entries_) {
        npcc::edit::RoleStyle value = scheme_->Role(language, entry.role);
        for (const npcc::edit::StyleOverride& item : scheme_->Styles(language)) {
            if (item.number == entry.number) {
                value = item.value;
                break;
            }
        }
        itemScratch_.push_back(value);
    }
    itemIndex_ = 0;
    RefreshItemControls();
    RefreshControls();
}

npcc::edit::RoleStyle StyleConfigurator::DefaultRole(npcc::edit::StyleRole role) const {
    // 默认色：向一个不存在的语言问角色色，拿到的就是内置默认（见 StyleScheme::Role）
    return npcc::edit::StyleScheme::Defaults().Role(L"__default__", role);
}

void StyleConfigurator::RefreshItemControls() {
    HWND list = ::GetDlgItem(hwnd_, kStyleList);
    if (list != nullptr) {
        // 列表内容随语言变，也可能因为"恢复默认"整片改回角色色，所以每次重建
        ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            const npcc::edit::StyleEntry& entry = entries_[index];
            const npcc::edit::RoleStyle& value = itemScratch_[index];
            const std::wstring text =
                std::to_wstring(entry.number) + L"    " + entry.name + L"    " +
                npcc::edit::StyleScheme::ColourText(value.fore) +
                (value.bold ? L"  粗" : L"") + (value.italic ? L"  斜" : L"");
            ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        if (!entries_.empty()) {
            if (itemIndex_ < 0 || itemIndex_ >= static_cast<int>(entries_.size())) {
                itemIndex_ = 0;
            }
            ::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(itemIndex_), 0);
        }
    }
    const bool hasItem = !entries_.empty();
    for (const int id : {kStyleItemColour, kStyleItemBold, kStyleItemItalic, kStyleItemReset}) {
        if (HWND control = ::GetDlgItem(hwnd_, id); control != nullptr) {
            ::EnableWindow(control, hasItem ? TRUE : FALSE);
        }
    }
    if (!hasItem) {
        return;
    }
    const npcc::edit::RoleStyle& value = itemScratch_[static_cast<std::size_t>(itemIndex_)];
    if (HWND bold = ::GetDlgItem(hwnd_, kStyleItemBold); bold != nullptr) {
        ::SendMessageW(bold, BM_SETCHECK, value.bold ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (HWND italic = ::GetDlgItem(hwnd_, kStyleItemItalic); italic != nullptr) {
        ::SendMessageW(italic, BM_SETCHECK, value.italic ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (HWND swatch = ::GetDlgItem(hwnd_, kStyleItemColour); swatch != nullptr) {
        ::InvalidateRect(swatch, nullptr, TRUE);
    }
}

void StyleConfigurator::FlushToScheme() {
    const std::wstring language = CurrentLanguage();
    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
        scheme_->SetRole(language, static_cast<npcc::edit::StyleRole>(role),
                         scratch_[static_cast<std::size_t>(role)]);
    }
    // 逐项覆盖只在"确实与角色色不同"时写：在界面上把某一项调回角色色，文件里那条覆盖
    // 就该消失，而不是留下一条等于角色色的死记录（下次再调角色色时它就会显得"没生效"）
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const npcc::edit::StyleEntry& entry = entries_[index];
        const npcc::edit::RoleStyle& value = itemScratch_[index];
        if (value != scheme_->Role(language, entry.role)) {
            scheme_->SetStyle(language, entry.number, entry.name, value);
        } else {
            scheme_->ClearStyle(language, entry.number);
        }
    }
}

void StyleConfigurator::RefreshControls() {
    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
        const npcc::edit::RoleStyle& value = scratch_[static_cast<std::size_t>(role)];
        if (HWND bold = ::GetDlgItem(hwnd_, kStyleBoldBase + role); bold != nullptr) {
            ::SendMessageW(bold, BM_SETCHECK, value.bold ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (HWND italic = ::GetDlgItem(hwnd_, kStyleItalicBase + role); italic != nullptr) {
            ::SendMessageW(italic, BM_SETCHECK, value.italic ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (HWND swatch = ::GetDlgItem(hwnd_, kStyleRowBase + role); swatch != nullptr) {
            ::InvalidateRect(swatch, nullptr, TRUE);
        }
    }
}

// 颜色块的自绘（角色行与"具体样式"那一行都用它——两处各画一遍迟早会画得不一样）
void DrawSwatch(const DRAWITEMSTRUCT& item, std::uint32_t fore) {
    HBRUSH brush = ::CreateSolidBrush(
        RGB((fore >> 16) & 0xFF, (fore >> 8) & 0xFF, fore & 0xFF));
    RECT rect = item.rcItem;
    ::InflateRect(&rect, -2, -2);
    ::FillRect(item.hDC, &rect, brush);
    ::DeleteObject(brush);
    ::FrameRect(item.hDC, &rect, reinterpret_cast<HBRUSH>(COLOR_BTNSHADOW + 1));
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        ::InflateRect(&focus, -1, -1);
        ::DrawFocusRect(item.hDC, &focus);
    }
}

void StyleConfigurator::DrawColourButton(const DRAWITEMSTRUCT& item) const {
    if (item.CtlID == kStyleItemColour) {
        if (itemIndex_ < 0 || itemIndex_ >= static_cast<int>(itemScratch_.size())) {
            return;
        }
        DrawSwatch(item, itemScratch_[static_cast<std::size_t>(itemIndex_)].fore);
        return;
    }
    const int role = item.CtlID - kStyleRowBase;
    if (role < 0 || role >= npcc::edit::kStyleRoleCount) {
        return;
    }
    DrawSwatch(item, scratch_[static_cast<std::size_t>(role)].fore);
}

void StyleConfigurator::PickColour(int role) {
    if (role < 0 || role >= npcc::edit::kStyleRoleCount) {
        return;
    }
    const npcc::edit::RoleStyle& current = scratch_[static_cast<std::size_t>(role)];
    COLORREF custom[16] = {};
    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = hwnd_;
    choose.rgbResult = RGB((current.fore >> 16) & 0xFF, (current.fore >> 8) & 0xFF,
                           current.fore & 0xFF);
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (::ChooseColorW(&choose) == 0) {
        return;   // 用户取消
    }
    scratch_[static_cast<std::size_t>(role)].fore =
        (static_cast<std::uint32_t>(GetRValue(choose.rgbResult)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(choose.rgbResult)) << 8) |
        static_cast<std::uint32_t>(GetBValue(choose.rgbResult));
    RefreshControls();
}

void StyleConfigurator::PickItemColour() {
    if (itemIndex_ < 0 || itemIndex_ >= static_cast<int>(itemScratch_.size())) {
        return;
    }
    const npcc::edit::RoleStyle& current = itemScratch_[static_cast<std::size_t>(itemIndex_)];
    COLORREF custom[16] = {};
    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = hwnd_;
    choose.rgbResult = RGB((current.fore >> 16) & 0xFF, (current.fore >> 8) & 0xFF,
                           current.fore & 0xFF);
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (::ChooseColorW(&choose) == 0) {
        return;   // 用户取消
    }
    itemScratch_[static_cast<std::size_t>(itemIndex_)].fore =
        (static_cast<std::uint32_t>(GetRValue(choose.rgbResult)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(choose.rgbResult)) << 8) |
        static_cast<std::uint32_t>(GetBValue(choose.rgbResult));
    RefreshItemControls();
}

void StyleConfigurator::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(kGap);
    const int rowHeight = Scale(kRow);
    const int buttonHeight = Scale(30);
    const int buttonWidth = Scale(110);
    const int width = client.right - client.left;
    const int labelWidth = Scale(90);

    int y = gap;
    if (HWND label = ::GetDlgItem(hwnd_, kStyleLanguageLabel); label != nullptr) {
        ::MoveWindow(label, gap, y + Scale(4), labelWidth, rowHeight, TRUE);
    }
    if (HWND combo = ::GetDlgItem(hwnd_, kStyleLanguageCombo); combo != nullptr) {
        ::MoveWindow(combo, gap + labelWidth, y, Scale(220), rowHeight * 10, TRUE);
    }
    y += rowHeight + gap;

    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
        // 角色名画在颜色按钮左边：标签与色块各有各的 id，本来就不该混用一个
        if (HWND label = ::GetDlgItem(hwnd_, kStyleRoleLabelBase + role); label != nullptr) {
            ::MoveWindow(label, gap, y + Scale(3), labelWidth - Scale(6), rowHeight - Scale(6),
                         TRUE);
        }
        if (HWND swatch = ::GetDlgItem(hwnd_, kStyleRowBase + role); swatch != nullptr) {
            ::MoveWindow(swatch, gap + labelWidth, y, Scale(64), rowHeight - Scale(6), TRUE);
        }
        if (HWND bold = ::GetDlgItem(hwnd_, kStyleBoldBase + role); bold != nullptr) {
            ::MoveWindow(bold, gap + labelWidth + Scale(72), y + Scale(3), Scale(48),
                         rowHeight - Scale(6), TRUE);
        }
        if (HWND italic = ::GetDlgItem(hwnd_, kStyleItalicBase + role); italic != nullptr) {
            ::MoveWindow(italic, gap + labelWidth + Scale(122), y + Scale(3), Scale(48),
                         rowHeight - Scale(6), TRUE);
        }
        y += rowHeight;
    }
    y += gap;

    // 逐样式号那一节
    if (HWND label = ::GetDlgItem(hwnd_, kStyleItemLabel); label != nullptr) {
        ::MoveWindow(label, gap, y, width - gap * 2, rowHeight - Scale(4), TRUE);
    }
    y += rowHeight;
    const int buttonY2 = client.bottom - buttonHeight - gap;
    const int itemRow = Scale(28);
    if (HWND list = ::GetDlgItem(hwnd_, kStyleList); list != nullptr) {
        // 列表占满这一节剩下的高度，最后一行留给"颜色/粗/斜/恢复"
        const int listHeight = std::max(itemRow * 3, buttonY2 - gap - itemRow - y);
        ::MoveWindow(list, gap, y, width - gap * 2, listHeight, TRUE);
        y += listHeight + Scale(6);
    }
    if (HWND colour = ::GetDlgItem(hwnd_, kStyleItemColour); colour != nullptr) {
        ::MoveWindow(colour, gap, y, Scale(64), itemRow - Scale(6), TRUE);
    }
    if (HWND bold = ::GetDlgItem(hwnd_, kStyleItemBold); bold != nullptr) {
        ::MoveWindow(bold, gap + Scale(72), y + Scale(3), Scale(48), itemRow - Scale(6), TRUE);
    }
    if (HWND italic = ::GetDlgItem(hwnd_, kStyleItemItalic); italic != nullptr) {
        ::MoveWindow(italic, gap + Scale(122), y + Scale(3), Scale(48), itemRow - Scale(6), TRUE);
    }
    if (HWND reset = ::GetDlgItem(hwnd_, kStyleItemReset); reset != nullptr) {
        ::MoveWindow(reset, gap + Scale(178), y, Scale(110), itemRow - Scale(6), TRUE);
    }

    const int buttonY = client.bottom - buttonHeight - gap;
    int right = width - gap - buttonWidth;
    for (const int id : {kStyleCancel, kStyleOk}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, right, buttonY, buttonWidth, buttonHeight, TRUE);
            right -= buttonWidth + gap / 2;
        }
    }
    if (HWND reset = ::GetDlgItem(hwnd_, kStyleReset); reset != nullptr) {
        ::MoveWindow(reset, gap, buttonY, buttonWidth, buttonHeight, TRUE);
    }
    (void)y;
}

bool StyleConfigurator::Show(HWND owner, const npcc::edit::LanguageRegistry& languages,
                             npcc::edit::StyleScheme& scheme) {
    owner_ = owner;
    languages_ = &languages;
    scheme_ = &scheme;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StyleConfigurator::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(520);
    const int height = Scale(560);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"样式配置器",
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

    Make(L"STATIC", L"语言：", WS_VISIBLE, kStyleLanguageLabel);
    HWND combo = Make(WC_COMBOBOXW, L"", WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                      kStyleLanguageCombo);
    if (combo != nullptr) {
        for (const npcc::edit::LanguageDef* language : languages_->All()) {
            ::SendMessageW(combo, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(language->displayName.c_str()));
        }
        ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
        const auto kind = static_cast<npcc::edit::StyleRole>(role);
        const std::wstring name = npcc::edit::StyleScheme::RoleDisplayName(kind);
        Make(L"STATIC", name.c_str(), WS_VISIBLE | SS_LEFT, kStyleRoleLabelBase + role);
        Make(L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kStyleRowBase + role);
        Make(L"BUTTON", L"粗", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kStyleBoldBase + role);
        Make(L"BUTTON", L"斜", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
             kStyleItalicBase + role);
    }
    Make(L"BUTTON", L"恢复默认", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kStyleReset);
    // 逐样式号那一节：列表 + 选中项的颜色/粗体/斜体（控件与角色行分开，免得两套状态打架）
    Make(L"STATIC", L"具体样式（按样式号，只列本程序上了色的那些）：", WS_VISIBLE | SS_LEFT,
         kStyleItemLabel);
    Make(WC_LISTBOXW, L"",
         WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY, kStyleList);
    Make(L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kStyleItemColour);
    Make(L"BUTTON", L"粗", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kStyleItemBold);
    Make(L"BUTTON", L"斜", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kStyleItemItalic);
    Make(L"BUTTON", L"该项恢复默认", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kStyleItemReset);
    Make(L"BUTTON", L"确定", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kStyleOk);
    Make(L"BUTTON", L"取消", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kStyleCancel);
    (void)::SendMessageW(hwnd_, DM_SETDEFID, kStyleOk, 0);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    LoadForLanguage(0);
    Layout();

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    // 窗口显示之后再摆一次位置：创建过程中的客户区尺寸未必是最终值（WM_SIZE 也是在这期间
    // 到达的）。这一步是防御性的——真正抓到过的布局问题见 kStyleRoleLabelBase 那里的注释。
    Layout();
    ::SetFocus(combo);

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
    ::EnableWindow(owner_, TRUE);
    ::SetForegroundWindow(owner_);
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    return accepted_;
}

LRESULT CALLBACK StyleConfigurator::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam) {
    auto* self = reinterpret_cast<StyleConfigurator*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<StyleConfigurator*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT StyleConfigurator::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item != nullptr &&
                ((item->CtlID >= kStyleRowBase &&
                  item->CtlID < kStyleRowBase + npcc::edit::kStyleRoleCount) ||
                 item->CtlID == kStyleItemColour)) {
                DrawColourButton(*item);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kStyleList && HIWORD(wParam) == LBN_SELCHANGE) {
                const LRESULT index =
                    ::SendMessageW(::GetDlgItem(hwnd_, kStyleList), LB_GETCURSEL, 0, 0);
                if (index >= 0 && index < static_cast<LRESULT>(entries_.size())) {
                    itemIndex_ = static_cast<int>(index);
                    RefreshItemControls();
                }
                return 0;
            }
            if (id == kStyleItemColour) {
                PickItemColour();
                return 0;
            }
            if (id == kStyleItemBold || id == kStyleItemItalic) {
                if (itemIndex_ >= 0 && itemIndex_ < static_cast<int>(itemScratch_.size())) {
                    npcc::edit::RoleStyle& value =
                        itemScratch_[static_cast<std::size_t>(itemIndex_)];
                    const bool checked =
                        ::SendMessageW(::GetDlgItem(hwnd_, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (id == kStyleItemBold) {
                        value.bold = checked;
                    } else {
                        value.italic = checked;
                    }
                    RefreshItemControls();
                }
                return 0;
            }
            if (id == kStyleItemReset) {
                // "该项恢复默认" = 把它拉回它所属角色的颜色；确定时那条覆盖会被删掉
                if (itemIndex_ >= 0 && itemIndex_ < static_cast<int>(itemScratch_.size())) {
                    itemScratch_[static_cast<std::size_t>(itemIndex_)] =
                        scratch_[static_cast<std::size_t>(
                            entries_[static_cast<std::size_t>(itemIndex_)].role)];
                    RefreshItemControls();
                }
                return 0;
            }
            if (id >= kStyleRowBase && id < kStyleRowBase + npcc::edit::kStyleRoleCount) {
                PickColour(id - kStyleRowBase);
                return 0;
            }
            if (id >= kStyleBoldBase && id < kStyleBoldBase + npcc::edit::kStyleRoleCount) {
                scratch_[static_cast<std::size_t>(id - kStyleBoldBase)].bold =
                    ::SendMessageW(::GetDlgItem(hwnd_, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
                return 0;
            }
            if (id >= kStyleItalicBase && id < kStyleItalicBase + npcc::edit::kStyleRoleCount) {
                scratch_[static_cast<std::size_t>(id - kStyleItalicBase)].italic =
                    ::SendMessageW(::GetDlgItem(hwnd_, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
                return 0;
            }
            switch (id) {
                case kStyleLanguageCombo:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        const LRESULT index =
                            ::SendMessageW(::GetDlgItem(hwnd_, kStyleLanguageCombo),
                                           CB_GETCURSEL, 0, 0);
                        // 先收起当前语言改过的值，再换语言
                        FlushToScheme();
                        LoadForLanguage(static_cast<int>(index));
                    }
                    return 0;
                case kStyleReset: {
                    // "全部恢复默认"：角色回默认色、逐项覆盖也清掉。真正的清除放到点确定
                    // 的时候做（clearAll_），这样点"取消"仍然是什么都不动。
                    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
                        const auto kind = static_cast<npcc::edit::StyleRole>(role);
                        scratch_[static_cast<std::size_t>(role)] = DefaultRole(kind);
                    }
                    for (std::size_t index = 0; index < entries_.size(); ++index) {
                        itemScratch_[index] =
                            scratch_[static_cast<std::size_t>(entries_[index].role)];
                    }
                    clearAll_ = true;
                    RefreshItemControls();
                    RefreshControls();
                    return 0;
                }
                case kStyleOk: {
                    if (clearAll_) {
                        scheme_->ResetLanguage(CurrentLanguage());
                    }
                    FlushToScheme();
                    accepted_ = true;
                    done_ = true;
                    return 0;
                }
                case kStyleCancel:
                    done_ = true;
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                return 0;
            }
            break;
        // 尺寸变了（含 DPI 变化引起的重排）就按真实客户区重摆一遍，免得有控件落在外面
        case WM_SIZE:
            Layout();
            return 0;
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

bool ShowStyleConfigurator(HWND owner, const npcc::edit::LanguageRegistry& languages,
                           npcc::edit::StyleScheme& scheme) {
    StyleConfigurator dialog;
    return dialog.Show(owner, languages, scheme);
}

}  // namespace npcc::shell
