#include "search/TextBytes.h"

#include <windows.h>

namespace npcc::search {

bool ConvertToDocBytes(std::wstring_view text, bool utfMode, std::uint32_t codePage,
                       std::string& out) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    const UINT page = utfMode ? CP_UTF8 : codePage;
    const int need = ::WideCharToMultiByte(page, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return true;   // 转换不出内容，按空处理
    }
    out.resize(static_cast<size_t>(need));
    return ::WideCharToMultiByte(page, 0, text.data(), static_cast<int>(text.size()),
                                 out.data(), need, nullptr, nullptr) != 0;
}

std::wstring ConvertFromDocBytes(std::string_view bytes, bool utfMode, std::uint32_t codePage) {
    if (bytes.empty()) {
        return {};
    }
    const UINT page = utfMode ? CP_UTF8 : codePage;
    const int need = ::MultiByteToWideChar(page, 0, bytes.data(), static_cast<int>(bytes.size()),
                                           nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(page, 0, bytes.data(), static_cast<int>(bytes.size()),
                          wide.data(), need);
    return wide;
}

}  // namespace npcc::search
