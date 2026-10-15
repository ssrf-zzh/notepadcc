#include "shell/StatusBar.h"

#include <commctrl.h>

namespace npcc::shell {

bool StatusBar::Create(HWND parent, int controlId) {
    hwnd_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                              0, 0, 0, 0, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd_ == nullptr) {
        return false;
    }
    SetText(0, L"就绪");
    SetText(1, L"");
    SetText(2, L"");
    return true;
}

void StatusBar::Resize(const RECT& client) const {
    if (hwnd_ == nullptr) {
        return;
    }
    ::SendMessageW(hwnd_, WM_SIZE, 0, 0);

    // 分区宽度按客户区宽度比例分配，避免固定像素在高 DPI 下错位
    const int width = client.right - client.left;
    int parts[3] = {width * 45 / 100, width * 75 / 100, -1};
    ::SendMessageW(hwnd_, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
}

int StatusBar::Height() const {
    if (hwnd_ == nullptr) {
        return 0;
    }
    const LRESULT size = ::SendMessageW(hwnd_, WM_SIZE, 0, 0);
    return HIWORD(size);
}

void StatusBar::SetText(int part, const std::wstring& text) const {
    if (hwnd_ != nullptr) {
        ::SendMessageW(hwnd_, SB_SETTEXTW, static_cast<WPARAM>(part),
                       reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void StatusBar::SetPosition(int32_t line, int32_t column, int32_t selectedBytes,
                            bool overwrite) const {
    std::wstring text = L"行 " + std::to_wstring(line + 1) + L", 列 " + std::to_wstring(column + 1);
    if (selectedBytes > 0) {
        text += L"  |  选中 " + std::to_wstring(selectedBytes) + L" 字节";
    }
    text += overwrite ? L"  |  改写" : L"  |  插入";
    SetText(0, text);
}

void StatusBar::SetDocumentInfo(const std::wstring& encoding, const std::wstring& eol,
                                const std::wstring& language, bool modified) const {
    SetText(1, encoding + L"  |  " + eol);
    SetText(2, language + (modified ? L"  |  已修改" : L""));
}

void StatusBar::SetMessage(const std::wstring& text) const {
    SetText(0, text);
}

}  // namespace npcc::shell
