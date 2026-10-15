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

std::string DocBytesToUtf8(std::string_view bytes, bool utfMode, std::uint32_t codePage) {
    if (bytes.empty()) {
        return {};
    }
    if (utfMode) {
        return std::string(bytes);
    }
    const std::wstring wide = ConvertFromDocBytes(bytes, false, codePage);
    if (wide.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

}  // namespace npcc::search
