#include "shell/StyleConfigDialog.h"

#include <commctrl.h>
#include <commdlg.h>   // ChooseColorW（系统颜色选择器）

#include <array>
#include <string>
#include <vector>

#include "edit/LanguageRegistry.h"
#include "edit/StyleScheme.h"

namespace npcc::shell {
namespace {

constexpr int            kGap = 8;
constexpr int            kRow = 26;
constexpr const wchar_t* kWindowClass = L"notepadccStyleConfig";

enum StyleControl : int {
    kStyleLanguageLabel = 1200,
    kStyleLanguageCombo = 1201,
    kStyleRowBase       = 1210,   // +role: 颜色按钮
    kStyleBoldBase      = 1230,   // +role: 粗体
    kStyleItalicBase    = 1250,   // +role: 斜体
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
    void DrawColourButton(const DRAWITEMSTRUCT& item) const;
    void PickColour(int role);
    std::wstring CurrentLanguage() const;

    HWND                                       owner_ = nullptr;
    HWND                                       hwnd_ = nullptr;
    HFONT                                      font_ = nullptr;
    const npcc::edit::LanguageRegistry*        languages_ = nullptr;
    npcc::edit::StyleScheme*                   scheme_ = nullptr;
    std::array<npcc::edit::RoleStyle, npcc::edit::kStyleRoleCount> scratch_{};
    int                                        languageIndex_ = 0;
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
    const npcc::edit::StyleScheme defaults = npcc::edit::StyleScheme::Defaults();
    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
        scratch_[static_cast<std::size_t>(role)] =
            scheme_->Role(CurrentLanguage(), static_cast<npcc::edit::StyleRole>(role));
        (void)defaults;
    }
    RefreshControls();
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

void StyleConfigurator::DrawColourButton(const DRAWITEMSTRUCT& item) const {
    const int role = item.CtlID - kStyleRowBase;
    if (role < 0 || role >= npcc::edit::kStyleRoleCount) {
        return;
    }
    const npcc::edit::RoleStyle& value = scratch_[static_cast<std::size_t>(role)];
    HBRUSH brush = ::CreateSolidBrush(RGB((value.fore >> 16) & 0xFF, (value.fore >> 8) & 0xFF,
                                          value.fore & 0xFF));
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
        // 角色名用 STATIC 画在颜色按钮左边（沿用创建顺序，见 Show()）
        if (HWND label = ::GetDlgItem(hwnd_, kStyleRowBase + role); label != nullptr) {
            ::MoveWindow(::GetDlgItem(hwnd_, kStyleRowBase + role), gap + labelWidth, y, Scale(64),
                         rowHeight - Scale(6), TRUE);
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
    const int width = Scale(460);
    const int height = Scale(330);
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
        // 角色名当 STATIC 用，创建顺序与颜色按钮一一对应（Layout 里按 id 摆）
        const std::wstring name = npcc::edit::StyleScheme::RoleDisplayName(kind);
        HWND label = Make(L"STATIC", name.c_str(), WS_VISIBLE | SS_LEFT, -1);
        (void)label;
        Make(L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kStyleRowBase + role);
        Make(L"BUTTON", L"粗", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kStyleBoldBase + role);
        Make(L"BUTTON", L"斜", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
             kStyleItalicBase + role);
    }
    Make(L"BUTTON", L"恢复默认", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kStyleReset);
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
            if (item != nullptr && item->CtlID >= kStyleRowBase &&
                item->CtlID < kStyleRowBase + npcc::edit::kStyleRoleCount) {
                DrawColourButton(*item);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
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
                        for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
                            scheme_->SetRole(CurrentLanguage(),
                                             static_cast<npcc::edit::StyleRole>(role),
                                             scratch_[static_cast<std::size_t>(role)]);
                        }
                        LoadForLanguage(static_cast<int>(index));
                    }
                    return 0;
                case kStyleReset: {
                    const npcc::edit::StyleScheme defaults = npcc::edit::StyleScheme::Defaults();
                    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
                        scratch_[static_cast<std::size_t>(role)] =
                            defaults.Role(CurrentLanguage(),
                                          static_cast<npcc::edit::StyleRole>(role));
                    }
                    RefreshControls();
                    return 0;
                }
                case kStyleOk: {
                    for (int role = 0; role < npcc::edit::kStyleRoleCount; ++role) {
                        scheme_->SetRole(CurrentLanguage(),
                                         static_cast<npcc::edit::StyleRole>(role),
                                         scratch_[static_cast<std::size_t>(role)]);
                    }
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
