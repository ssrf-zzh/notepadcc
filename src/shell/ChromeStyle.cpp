#include "shell/ChromeStyle.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace npcc::shell {
namespace {

ChromeColours MakeLight() {
    ChromeColours c;
    c.menuBar       = RGB(0xFF, 0xFF, 0xFF);
    c.strip         = RGB(0xF5, 0xF6, 0xF8);
    c.tabActive     = RGB(0xFF, 0xFF, 0xFF);
    c.tabHover      = RGB(0xED, 0xF0, 0xF4);
    c.text          = RGB(0x1F, 0x23, 0x29);
    c.textMuted     = RGB(0x61, 0x61, 0x61);
    c.line          = RGB(0xE3, 0xE6, 0xEB);
    c.accent        = RGB(0x25, 0x63, 0xEB);
    c.accentSoft    = RGB(0xE8, 0xEF, 0xFD);
    c.buttonHot     = RGB(0xEE, 0xF1, 0xF5);
    c.buttonPressed = RGB(0xE1, 0xE5, 0xEB);
    c.iconDisabled  = RGB(0xB5, 0xBB, 0xC5);
    c.danger        = RGB(0xD9, 0x2D, 0x20);
    c.dangerSoft    = RGB(0xFE, 0xE4, 0xE2);
    c.statusText    = RGB(0x3F, 0x46, 0x52);
    return c;
}

ChromeColours MakeDark() {
    ChromeColours c;
    c.menuBar       = RGB(0x1B, 0x1D, 0x21);
    c.strip         = RGB(0x20, 0x22, 0x26);
    c.tabActive     = RGB(0x17, 0x19, 0x1C);
    c.tabHover      = RGB(0x2A, 0x2D, 0x32);
    c.text          = RGB(0xF0, 0xF2, 0xF5);
    c.textMuted     = RGB(0x9A, 0xA1, 0xAC);
    c.line          = RGB(0x35, 0x38, 0x3F);
    c.accent        = RGB(0x60, 0xA5, 0xFA);
    c.accentSoft    = RGB(0x23, 0x3A, 0x5C);
    c.buttonHot     = RGB(0x2B, 0x2F, 0x36);
    c.buttonPressed = RGB(0x25, 0x28, 0x2E);
    c.iconDisabled  = RGB(0x6B, 0x72, 0x80);
    c.danger        = RGB(0xFF, 0x8A, 0x80);
    c.dangerSoft    = RGB(0x4A, 0x29, 0x28);
    c.statusText    = RGB(0xD6, 0xDA, 0xE1);
    return c;
}

// 护眼（低蓝光）：底色照旧"红通道明显高于蓝通道"——还是字面意思。
ChromeColours MakeEyeLight() {
    ChromeColours c;
    c.menuBar       = RGB(0xFB, 0xF6, 0xEC);
    c.strip         = RGB(0xF2, 0xEA, 0xDB);
    c.tabActive     = RGB(0xFB, 0xF7, 0xF0);
    c.tabHover      = RGB(0xE9, 0xDD, 0xC7);
    c.text          = RGB(0x3B, 0x36, 0x2C);
    c.textMuted     = RGB(0x77, 0x6C, 0x57);
    c.line          = RGB(0xD8, 0xC9, 0xAC);
    c.accent        = RGB(0xB7, 0x79, 0x1F);
    c.accentSoft    = RGB(0xF0, 0xE3, 0xC8);
    c.buttonHot     = RGB(0xED, 0xE2, 0xCC);
    c.buttonPressed = RGB(0xE0, 0xD2, 0xB5);
    c.iconDisabled  = RGB(0xA8, 0x9A, 0x80);
    c.danger        = RGB(0xC0, 0x5B, 0x4D);
    c.dangerSoft    = RGB(0xF0, 0xD9, 0xD2);
    c.statusText    = RGB(0x4C, 0x45, 0x36);
    return c;
}

ChromeColours MakeEyeDark() {
    ChromeColours c;
    c.menuBar       = RGB(0x1D, 0x18, 0x14);
    c.strip         = RGB(0x2A, 0x23, 0x20);
    c.tabActive     = RGB(0x20, 0x1A, 0x15);
    c.tabHover      = RGB(0x35, 0x2C, 0x25);
    c.text          = RGB(0xE8, 0xDC, 0xC6);
    c.textMuted     = RGB(0x9A, 0x8C, 0x74);
    c.line          = RGB(0x46, 0x3B, 0x2E);
    c.accent        = RGB(0xE0, 0xA3, 0x4C);
    c.accentSoft    = RGB(0x4A, 0x3A, 0x26);
    c.buttonHot     = RGB(0x34, 0x2B, 0x24);
    c.buttonPressed = RGB(0x2B, 0x23, 0x1D);
    c.iconDisabled  = RGB(0x7E, 0x73, 0x60);
    c.danger        = RGB(0xD9, 0x7A, 0x6A);
    c.dangerSoft    = RGB(0x4A, 0x2C, 0x29);
    c.statusText    = RGB(0xDA, 0xCD, 0xB4);
    return c;
}

// dwmapi / uxtheme 都是**用到才加载**：老系统上没有对应导出时静默降级，
// 不能为了一个标题栏颜色让程序在启动时依赖一个可能不存在的库。
using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

DwmSetWindowAttributeFn LoadDwmSetWindowAttribute() {
    static const DwmSetWindowAttributeFn fn = []() -> DwmSetWindowAttributeFn {
        const HMODULE module = ::LoadLibraryW(L"dwmapi.dll");
        if (module == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<DwmSetWindowAttributeFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, "DwmSetWindowAttribute")));
    }();
    return fn;
}

using AllowDarkModeForWindowFn = BOOL (WINAPI*)(HWND, BOOL);

AllowDarkModeForWindowFn LoadAllowDarkModeForWindow() {
    static const AllowDarkModeForWindowFn fn = []() -> AllowDarkModeForWindowFn {
        const HMODULE module = ::LoadLibraryW(L"uxtheme.dll");
        if (module == nullptr) {
            return nullptr;
        }
        // 132 = AllowDarkModeForWindow（未公开序号，Win10 1809+ 有效）
        return reinterpret_cast<AllowDarkModeForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, MAKEINTRESOURCEA(132))));
    }();
    return fn;
}

using SetWindowThemeFn = HRESULT (WINAPI*)(HWND, LPCWSTR, LPCWSTR);

SetWindowThemeFn LoadSetWindowTheme() {
    static const SetWindowThemeFn fn = []() -> SetWindowThemeFn {
        const HMODULE module = ::LoadLibraryW(L"uxtheme.dll");
        if (module == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<SetWindowThemeFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, "SetWindowTheme")));
    }();
    return fn;
}

}  // namespace

namespace {

bool FontPresent(HDC dc, const wchar_t* face) {
    LOGFONTW logFont{};
    logFont.lfCharSet = DEFAULT_CHARSET;
    ::wcscpy_s(logFont.lfFaceName, face);
    bool found = false;
    ::EnumFontFamiliesExW(
        dc, &logFont,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    return found;
}

const wchar_t* ChromeFace() {
    static const std::wstring face = []() {
        const HDC dc = ::GetDC(nullptr);
        std::wstring chosen = L"Microsoft YaHei UI";
        if (dc != nullptr) {
            if (FontPresent(dc, L"Segoe UI Variable Text")) {
                chosen = L"Segoe UI Variable Text";
            } else if (FontPresent(dc, L"Segoe UI")) {
                chosen = L"Segoe UI";
            }
            ::ReleaseDC(nullptr, dc);
        }
        return chosen;
    }();
    return face.c_str();
}

}  // namespace

ChromeColours ChromeColoursFor(bool dark, bool eyeCare) {
    static const ChromeColours light = MakeLight();
    static const ChromeColours darkSet = MakeDark();
    static const ChromeColours eyeLight = MakeEyeLight();
    static const ChromeColours eyeDark = MakeEyeDark();
    if (eyeCare) {
        return dark ? eyeDark : eyeLight;
    }
    return dark ? darkSet : light;
}

void ApplyControlTheme(HWND control, bool dark, const wchar_t* darkTheme) {
    if (control == nullptr) {
        return;
    }
    if (const AllowDarkModeForWindowFn allowDark = LoadAllowDarkModeForWindow();
        allowDark != nullptr) {
        allowDark(control, dark ? TRUE : FALSE);
    }
    if (const SetWindowThemeFn setTheme = LoadSetWindowTheme(); setTheme != nullptr) {
        if (dark) {
            setTheme(control, darkTheme != nullptr ? darkTheme : L"DarkMode_Explorer", nullptr);
        } else {
            setTheme(control, L"Explorer", nullptr);
        }
    }
}

HFONT CreateChromeFont(int dpi, int pointTenths, bool semibold) {
    LOGFONTW logFont{};
    logFont.lfHeight = -::MulDiv(pointTenths, dpi, 720);
    logFont.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfCharSet = DEFAULT_CHARSET;
    ::wcscpy_s(logFont.lfFaceName, ChromeFace());
    return ::CreateFontIndirectW(&logFont);
}

std::wstring ChromeMenuLabel(std::wstring_view title, wchar_t* mnemonic) {
    if (mnemonic != nullptr) {
        *mnemonic = L'\0';
    }
    std::wstring text(title);
    if (text.size() >= 4 && text.back() == L')') {
        const wchar_t letter = text[text.size() - 2];
        const bool letterOk = (letter >= L'0' && letter <= L'9') ||
                              (letter >= L'A' && letter <= L'Z') ||
                              (letter >= L'a' && letter <= L'z');
        if (letterOk) {
            if (text[text.size() - 3] == L'(') {
                if (mnemonic != nullptr) {
                    *mnemonic = letter;
                }
                text.resize(text.size() - 3);
            } else if (text.size() >= 5 && text[text.size() - 4] == L'(' &&
                       text[text.size() - 3] == L'&') {
                if (mnemonic != nullptr) {
                    *mnemonic = letter;
                }
                text.resize(text.size() - 4);
            }
        }
    }
    return text;
}

void FillRoundedRect(HDC dc, const RECT& rc, int radius, COLORREF fill) {
    if (dc == nullptr || rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    const HBRUSH brush = ::CreateSolidBrush(fill);
    const HPEN pen = ::CreatePen(PS_SOLID, 1, fill);
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(brush);
    ::DeleteObject(pen);
}

namespace {

int ScaleForDpi(int value, int dpi) {
    return ::MulDiv(value, dpi > 0 ? dpi : 96, 96);
}

void FillControlBackground(HDC dc, const RECT& rc, COLORREF colour) {
    const HBRUSH brush = ::CreateSolidBrush(colour);
    ::FillRect(dc, &rc, brush);
    ::DeleteObject(brush);
}

void DrawRoundedOutline(HDC dc, const RECT& rc, int radius, COLORREF border, COLORREF fill) {
    FillRoundedRect(dc, rc, radius, border);
    RECT inner = rc;
    ::InflateRect(&inner, -1, -1);
    if (inner.right > inner.left && inner.bottom > inner.top) {
        FillRoundedRect(dc, inner, std::max(0, radius - 1), fill);
    }
}

}  // namespace

void DrawChromePushButton(HDC dc, const RECT& rc, std::wstring_view text, HFONT font,
                          const ChromeColours& colours, int dpi, bool dark, bool primary,
                          bool hovered, bool pressed, bool focused, bool enabled) {
    if (dc == nullptr || rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    FillControlBackground(dc, rc, colours.strip);
    ::SetBkMode(dc, TRANSPARENT);
    if (font != nullptr) {
        ::SelectObject(dc, font);
    }

    RECT body = rc;
    ::InflateRect(&body, -1, -1);
    COLORREF fill = colours.strip;
    COLORREF border = colours.line;
    if (!enabled) {
        fill = colours.strip;
        border = colours.line;
    } else if (primary) {
        fill = pressed ? colours.accentSoft : colours.accent;
        border = fill;
    } else if (pressed) {
        fill = colours.buttonPressed;
        border = colours.accent;
    } else if (hovered) {
        fill = colours.buttonHot;
        border = colours.line;
    } else {
        fill = colours.strip;
    }

    const int radius = ScaleForDpi(6, dpi);
    if (primary) {
        FillRoundedRect(dc, body, radius, fill);
    } else {
        DrawRoundedOutline(dc, body, radius, border, fill);
    }

    COLORREF textColour = enabled ? colours.text : colours.iconDisabled;
    if (primary && enabled) {
        textColour = pressed ? colours.text : (dark ? RGB(0x0B, 0x12, 0x20) : RGB(0xFF, 0xFF, 0xFF));
    }
    ::SetTextColor(dc, textColour);
    RECT textRect = body;
    ::InflateRect(&textRect, -ScaleForDpi(4, dpi), 0);
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (focused) {
        RECT focusRect = body;
        ::InflateRect(&focusRect, -ScaleForDpi(3, dpi), -ScaleForDpi(3, dpi));
        ::DrawFocusRect(dc, &focusRect);
    }
}

void DrawChromeCheckBox(HDC dc, const RECT& rc, std::wstring_view text, HFONT font,
                        const ChromeColours& colours, int dpi, bool dark, bool checked,
                        bool hovered, bool focused, bool enabled) {
    if (dc == nullptr || rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    FillControlBackground(dc, rc, colours.strip);
    ::SetBkMode(dc, TRANSPARENT);
    if (font != nullptr) {
        ::SelectObject(dc, font);
    }

    const int boxSize = ScaleForDpi(16, dpi);
    const int boxLeft = rc.left + ScaleForDpi(2, dpi);
    RECT box{boxLeft, rc.top + (rc.bottom - rc.top - boxSize) / 2, boxLeft + boxSize, 0};
    box.bottom = box.top + boxSize;
    const COLORREF border = (hovered || focused) ? colours.accent : colours.line;
    if (checked) {
        FillRoundedRect(dc, box, ScaleForDpi(4, dpi), enabled ? colours.accent : colours.iconDisabled);
        LOGBRUSH spec{};
        spec.lbStyle = BS_SOLID;
        spec.lbColor = dark ? RGB(0x0B, 0x12, 0x20) : RGB(0xFF, 0xFF, 0xFF);
        const HPEN pen = ::ExtCreatePen(
            PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
            static_cast<DWORD>(std::max(1, ScaleForDpi(2, dpi))), &spec, 0, nullptr);
        const HGDIOBJ oldPen = ::SelectObject(dc, pen);
        ::MoveToEx(dc, box.left + ScaleForDpi(3, dpi), box.top + boxSize / 2, nullptr);
        ::LineTo(dc, box.left + ScaleForDpi(6, dpi), box.bottom - ScaleForDpi(4, dpi));
        ::LineTo(dc, box.right - ScaleForDpi(3, dpi), box.top + ScaleForDpi(4, dpi));
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
    } else {
        DrawRoundedOutline(dc, box, ScaleForDpi(4, dpi), enabled ? border : colours.line,
                           colours.strip);
    }

    ::SetTextColor(dc, enabled ? colours.text : colours.iconDisabled);
    RECT textRect{box.right + ScaleForDpi(7, dpi), rc.top, rc.right, rc.bottom};
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (focused) {
        RECT focusRect = rc;
        ::InflateRect(&focusRect, -ScaleForDpi(1, dpi), -ScaleForDpi(1, dpi));
        ::DrawFocusRect(dc, &focusRect);
    }
}

void ApplyModernWindowChrome(HWND hwnd, bool dark, bool eyeCare) {
    if (hwnd == nullptr) {
        return;
    }
    const ChromeColours colours = ChromeColoursFor(dark, eyeCare);

    const AllowDarkModeForWindowFn allowDark = LoadAllowDarkModeForWindow();
    if (allowDark != nullptr) {
        allowDark(hwnd, dark ? TRUE : FALSE);
    }

    const DwmSetWindowAttributeFn setAttribute = LoadDwmSetWindowAttribute();
    if (setAttribute == nullptr) {
        return;
    }

    BOOL darkFlag = dark ? TRUE : FALSE;
    // 20 是现 SDK 的 DWMWA_USE_IMMERSIVE_DARK_MODE，19 是 20H1 之前的老编号
    if (FAILED(setAttribute(hwnd, 20, &darkFlag, sizeof(darkFlag)))) {
        setAttribute(hwnd, 19, &darkFlag, sizeof(darkFlag));
    }
    // 33 = DWMWA_WINDOW_CORNER_PREFERENCE，2 = DWMWCP_ROUND（Windows 11）
    const DWORD corner = 2;
    setAttribute(hwnd, 33, &corner, sizeof(corner));
    // 38 = DWMWA_SYSTEMBACKDROP_TYPE，2 = DWMSBT_MAINWINDOW（Win11 22H2 的 Mica）。
    // Mica 可用时让标题栏透出桌面材质；老系统失败再退回纯色标题栏。
    const DWORD backdrop = 2;
    setAttribute(hwnd, 38, &backdrop, sizeof(backdrop));
    // Mica 只负责材质；标题栏底色仍显式跟随主题。否则部分 Win11 配置会
    // 在 PrintWindow / 远程桌面上退回系统灰，和自绘菜单栏对不上。
    setAttribute(hwnd, 35, &colours.menuBar, sizeof(colours.menuBar));
    // 34 / 36 = 边框 / 标题文字（Windows 11 才有；失败无所谓）
    setAttribute(hwnd, 34, &colours.line, sizeof(colours.line));
    setAttribute(hwnd, 36, &colours.text, sizeof(colours.text));
}

namespace {

struct ChromeThemeState {
    bool dark = false;
    bool eyeCare = false;
};

ChromeThemeState g_chromeTheme;

int DpiOfWindow(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const GetDpiForWindowFn getDpi = []() -> GetDpiForWindowFn {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
    }();
    if (getDpi != nullptr && hwnd != nullptr) {
        const UINT dpi = getDpi(hwnd);
        if (dpi != 0) {
            return static_cast<int>(dpi);
        }
    }
    return 96;
}

std::wstring ControlText(HWND control) {
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

constexpr const wchar_t* kDialogBrushesProp = L"notepadcc.dialogBrushes";
constexpr UINT_PTR kDialogRootSubclass = 0x4E504344;
constexpr UINT_PTR kDialogButtonSubclass = 0x4E504345;

struct DialogBrushes {
    HBRUSH background = nullptr;
    HBRUSH field = nullptr;
};

DialogBrushes* BrushesOf(HWND hwnd) {
    return reinterpret_cast<DialogBrushes*>(::GetPropW(hwnd, kDialogBrushesProp));
}

void PaintDialogBackground(HWND hwnd, HDC dc) {
    if (hwnd == nullptr || dc == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd, &client);
    const DialogBrushes* brushes = BrushesOf(hwnd);
    const HBRUSH brush = brushes != nullptr && brushes->background != nullptr
                             ? brushes->background
                             : ::GetSysColorBrush(COLOR_BTNFACE);
    ::FillRect(dc, &client, brush);
}

void PaintDialogButton(HWND hwnd, HDC dc) {
    if (hwnd == nullptr || dc == nullptr) {
        return;
    }
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const UINT type = static_cast<UINT>(style) & BS_TYPEMASK;
    if (type == BS_OWNERDRAW || type == BS_GROUPBOX || type == BS_RADIOBUTTON ||
        type == BS_AUTORADIOBUTTON) {
        return;   // 这些按钮各有自己的绘制/交互，不抢
    }

    RECT client{};
    ::GetClientRect(hwnd, &client);
    const std::wstring text = ControlText(hwnd);
    const ChromeColours colours = CurrentChromeColours();
    const int dpi = DpiOfWindow(hwnd);
    const HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
    const bool enabled = ::IsWindowEnabled(hwnd) != 0;
    const bool focused = ::GetFocus() == hwnd;
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const bool hovered = ::WindowFromPoint(cursor) == hwnd;
    const DWORD state = static_cast<DWORD>(::SendMessageW(hwnd, BM_GETSTATE, 0, 0));
    const bool pressed = (state & BST_PUSHED) != 0;

    const bool checkbox = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
                          type == BS_AUTO3STATE || type == BS_3STATE;
    if (checkbox) {
        const bool checked = ::SendMessageW(hwnd, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
        DrawChromeCheckBox(dc, client, text, font, colours, dpi, g_chromeTheme.dark,
                           checked, hovered, focused, enabled);
        return;
    }

    bool primary = type == BS_DEFPUSHBUTTON;
    if (!primary) {
        const LRESULT def = ::SendMessageW(::GetParent(hwnd), DM_GETDEFID, 0, 0);
        primary = HIWORD(def) == DC_HASDEFID &&
                  static_cast<int>(LOWORD(def)) == ::GetDlgCtrlID(hwnd);
    }
    DrawChromePushButton(dc, client, text, font, colours, dpi, g_chromeTheme.dark, primary,
                         hovered, pressed, focused, enabled);
}

LRESULT CALLBACK DialogButtonSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            PaintDialogButton(hwnd, dc);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintDialogButton(hwnd, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            ::TrackMouseEvent(&track);
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case WM_MOUSELEAVE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case WM_SETTEXT:
        case WM_UPDATEUISTATE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void ApplyDialogControlThemes(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    const ChromeColours colours = CurrentChromeColours();
    struct Context {
        bool dark;
        ChromeColours colours;
    } context{g_chromeTheme.dark, colours};

    ::EnumChildWindows(
        hwnd,
        [](const HWND child, const LPARAM param) -> BOOL {
            const auto* context = reinterpret_cast<const Context*>(param);
            wchar_t className[32]{};
            ::GetClassNameW(child, className, 32);
            if (::_wcsicmp(className, L"Edit") == 0) {
                ApplyControlTheme(child, context->dark, L"DarkMode_CFD");
            } else if (::_wcsicmp(className, L"ComboBox") == 0) {
                ApplyControlTheme(child, context->dark, L"DarkMode_CFD");
            } else if (::_wcsicmp(className, L"ListBox") == 0) {
                ApplyControlTheme(child, context->dark, L"DarkMode_Explorer");
            } else if (::_wcsicmp(className, L"SysListView32") == 0) {
                ApplyControlTheme(child, context->dark, L"DarkMode_Explorer");
                ::SendMessageW(child, LVM_SETBKCOLOR, 0, context->colours.tabActive);
                ::SendMessageW(child, LVM_SETTEXTBKCOLOR, 0, context->colours.tabActive);
                ::SendMessageW(child, LVM_SETTEXTCOLOR, 0, context->colours.text);
                const HWND header = ListView_GetHeader(child);
                if (header != nullptr) {
                    ApplyControlTheme(header, context->dark, L"DarkMode_Explorer");
                    ::InvalidateRect(header, nullptr, TRUE);
                }
            } else if (::_wcsicmp(className, L"SysTreeView32") == 0) {
                ApplyControlTheme(child, context->dark, L"DarkMode_Explorer");
                ::SendMessageW(child, TVM_SETBKCOLOR, 0, context->colours.tabActive);
                ::SendMessageW(child, TVM_SETTEXTCOLOR, 0, context->colours.text);
            } else if (::_wcsicmp(className, WC_TABCONTROLW) == 0) {
                ApplyControlTheme(child, context->dark, L"DarkMode_Explorer");
            } else if (::_wcsicmp(className, L"Button") == 0) {
                const LONG_PTR style = ::GetWindowLongPtrW(child, GWL_STYLE);
                const UINT type = static_cast<UINT>(style) & BS_TYPEMASK;
                if (type != BS_OWNERDRAW && type != BS_GROUPBOX &&
                    type != BS_RADIOBUTTON && type != BS_AUTORADIOBUTTON) {
                    ::RemoveWindowSubclass(child, &DialogButtonSubclass, kDialogButtonSubclass);
                    ::SetWindowSubclass(child, &DialogButtonSubclass, kDialogButtonSubclass, 0);
                }
            }
            ::InvalidateRect(child, nullptr, TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
}

LRESULT CALLBACK DialogRootSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    DialogBrushes* brushes = BrushesOf(hwnd);
    switch (message) {
        case WM_ERASEBKGND:
            PaintDialogBackground(hwnd, reinterpret_cast<HDC>(wParam));
            return 1;
        case WM_PRINTCLIENT:
            PaintDialogBackground(hwnd, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_CTLCOLORDLG:
            return reinterpret_cast<LRESULT>(brushes != nullptr ? brushes->background
                                                                : nullptr);
        case WM_CTLCOLORSTATIC: {
            const HWND control = reinterpret_cast<HWND>(lParam);
            wchar_t className[32]{};
            ::GetClassNameW(control, className, 32);
            const bool readOnlyEdit = ::_wcsicmp(className, L"Edit") == 0;
            ::SetBkMode(reinterpret_cast<HDC>(wParam), readOnlyEdit ? OPAQUE : TRANSPARENT);
            ::SetTextColor(reinterpret_cast<HDC>(wParam),
                           ::IsWindowEnabled(control) ? CurrentChromeColours().text
                                                      : CurrentChromeColours().textMuted);
            if (readOnlyEdit) {
                ::SetBkColor(reinterpret_cast<HDC>(wParam), CurrentChromeColours().tabActive);
                return reinterpret_cast<LRESULT>(brushes != nullptr ? brushes->field : nullptr);
            }
            return reinterpret_cast<LRESULT>(brushes != nullptr ? brushes->background : nullptr);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            ::SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
            ::SetTextColor(reinterpret_cast<HDC>(wParam), CurrentChromeColours().text);
            ::SetBkColor(reinterpret_cast<HDC>(wParam), CurrentChromeColours().tabActive);
            return reinterpret_cast<LRESULT>(brushes != nullptr ? brushes->field : nullptr);
        case WM_CTLCOLORBTN:
            ::SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(brushes != nullptr ? brushes->background : nullptr);
        case WM_CTLCOLORSCROLLBAR:
            return reinterpret_cast<LRESULT>(brushes != nullptr ? brushes->background : nullptr);
        case WM_THEMECHANGED:
            ApplyDialogControlThemes(hwnd);
            break;
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, &DialogRootSubclass, kDialogRootSubclass);
            if (brushes != nullptr) {
                if (brushes->background != nullptr) {
                    ::DeleteObject(brushes->background);
                }
                if (brushes->field != nullptr) {
                    ::DeleteObject(brushes->field);
                }
                ::RemovePropW(hwnd, kDialogBrushesProp);
                delete brushes;
            }
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

}  // namespace

void SetChromeThemeState(bool dark, bool eyeCare) {
    g_chromeTheme.dark = dark;
    g_chromeTheme.eyeCare = eyeCare;
}

ChromeColours CurrentChromeColours() {
    return ChromeColoursFor(g_chromeTheme.dark, g_chromeTheme.eyeCare);
}

void ApplyDialogChrome(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    ApplyModernWindowChrome(hwnd, g_chromeTheme.dark, g_chromeTheme.eyeCare);
    DialogBrushes* brushes = BrushesOf(hwnd);
    if (brushes == nullptr) {
        brushes = new DialogBrushes();
        ::SetPropW(hwnd, kDialogBrushesProp, reinterpret_cast<HANDLE>(brushes));
        ::SetWindowSubclass(hwnd, &DialogRootSubclass, kDialogRootSubclass, 0);
    } else {
        if (brushes->background != nullptr) {
            ::DeleteObject(brushes->background);
        }
        if (brushes->field != nullptr) {
            ::DeleteObject(brushes->field);
        }
    }
    const ChromeColours colours = CurrentChromeColours();
    brushes->background = ::CreateSolidBrush(colours.strip);
    brushes->field = ::CreateSolidBrush(colours.tabActive);
    ApplyDialogControlThemes(hwnd);
    ::InvalidateRect(hwnd, nullptr, TRUE);
}

void ApplyDarkModePreference(bool dark) {
    using SetPreferredAppModeFn = int (WINAPI*)(int);
    static const SetPreferredAppModeFn setPreferred = []() -> SetPreferredAppModeFn {
        const HMODULE module = ::LoadLibraryW(L"uxtheme.dll");
        if (module == nullptr) {
            return nullptr;
        }
        // 135 = SetPreferredAppMode（未公开序号）
        return reinterpret_cast<SetPreferredAppModeFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, MAKEINTRESOURCEA(135))));
    }();
    if (setPreferred != nullptr) {
        setPreferred(dark ? 2 : 3);   // 2 = ForceDark，3 = ForceLight
    }
}

}  // namespace npcc::shell
