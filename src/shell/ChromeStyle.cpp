#include "shell/ChromeStyle.h"

namespace npcc::shell {
namespace {

ChromeColours MakeLight() {
    ChromeColours c;
    c.strip         = RGB(0xF3, 0xF3, 0xF3);
    c.tabActive     = RGB(0xFF, 0xFF, 0xFF);
    c.tabHover      = RGB(0xE8, 0xE8, 0xE8);
    c.text          = RGB(0x1B, 0x1B, 0x1B);
    c.textMuted     = RGB(0x61, 0x61, 0x61);
    c.line          = RGB(0xD8, 0xD8, 0xD8);
    c.accent        = RGB(0x0F, 0x6C, 0xBD);
    c.accentSoft    = RGB(0xDA, 0xE9, 0xF8);
    c.buttonHot     = RGB(0xE6, 0xE6, 0xE6);
    c.buttonPressed = RGB(0xDB, 0xDB, 0xDB);
    c.iconDisabled  = RGB(0xAC, 0xAC, 0xAC);
    c.danger        = RGB(0xC4, 0x2B, 0x1C);
    c.dangerSoft    = RGB(0xF6, 0xD9, 0xD5);
    c.statusText    = RGB(0x44, 0x44, 0x44);
    return c;
}

ChromeColours MakeDark() {
    ChromeColours c;
    c.strip         = RGB(0x27, 0x27, 0x27);
    c.tabActive     = RGB(0x1E, 0x1E, 0x1E);
    c.tabHover      = RGB(0x33, 0x33, 0x33);
    c.text          = RGB(0xED, 0xED, 0xED);
    c.textMuted     = RGB(0x9D, 0x9D, 0x9D);
    c.line          = RGB(0x3E, 0x3E, 0x3E);
    c.accent        = RGB(0x60, 0xCD, 0xFF);
    c.accentSoft    = RGB(0x1E, 0x3D, 0x57);
    c.buttonHot     = RGB(0x38, 0x38, 0x38);
    c.buttonPressed = RGB(0x2E, 0x2E, 0x2E);
    c.iconDisabled  = RGB(0x7C, 0x7C, 0x7C);
    c.danger        = RGB(0xFF, 0x99, 0xA4);
    c.dangerSoft    = RGB(0x4A, 0x2A, 0x2C);
    c.statusText    = RGB(0xD6, 0xD6, 0xD6);
    return c;
}

// 护眼（低蓝光）：底色照旧"红通道明显高于蓝通道"——还是字面意思。
ChromeColours MakeEyeLight() {
    ChromeColours c;
    c.strip         = RGB(0xEF, 0xE5, 0xD1);
    c.tabActive     = RGB(0xFB, 0xF5, 0xE7);
    c.tabHover      = RGB(0xE6, 0xD9, 0xBF);
    c.text          = RGB(0x3B, 0x36, 0x2C);
    c.textMuted     = RGB(0x77, 0x6C, 0x57);
    c.line          = RGB(0xD5, 0xC5, 0xA6);
    c.accent        = RGB(0xB9, 0x82, 0x2C);
    c.accentSoft    = RGB(0xEA, 0xDC, 0xBE);
    c.buttonHot     = RGB(0xE8, 0xDC, 0xC3);
    c.buttonPressed = RGB(0xDC, 0xCD, 0xAF);
    c.iconDisabled  = RGB(0xA8, 0x9A, 0x80);
    c.danger        = RGB(0xC0, 0x5B, 0x4D);
    c.dangerSoft    = RGB(0xF0, 0xD9, 0xD2);
    c.statusText    = RGB(0x4C, 0x45, 0x36);
    return c;
}

ChromeColours MakeEyeDark() {
    ChromeColours c;
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
    // 35 / 36 / 34 = 标题栏底色 / 标题文字 / 边框（Windows 11 才有；失败无所谓）
    setAttribute(hwnd, 35, &colours.strip, sizeof(colours.strip));
    setAttribute(hwnd, 36, &colours.text, sizeof(colours.text));
    setAttribute(hwnd, 34, &colours.line, sizeof(colours.line));
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
