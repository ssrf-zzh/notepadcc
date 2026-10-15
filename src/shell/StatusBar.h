#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace npcc::shell {

// 三段式状态栏：① 光标位置与选中字节数 ② 编码与行尾 ③ 语言与修改状态
class StatusBar {
public:
    bool Create(HWND parent, int controlId);
    void Resize(const RECT& client) const;

    void SetPosition(int32_t line, int32_t column, int32_t selectedBytes) const;
    void SetDocumentInfo(const std::wstring& encoding, const std::wstring& eol,
                         const std::wstring& language, bool modified) const;

    HWND Handle() const noexcept { return hwnd_; }
    int  Height() const;

private:
    void SetText(int part, const std::wstring& text) const;
    HWND hwnd_ = nullptr;
};

}  // namespace npcc::shell
