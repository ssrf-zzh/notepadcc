#include "core/PrintText.h"

#include <algorithm>

namespace npcc::core {
namespace {

void ReplaceAll(std::wstring& text, std::wstring_view from, const std::wstring& to) {
    if (from.empty()) {
        return;
    }
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::wstring::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

}  // namespace

std::wstring ExpandPrintTemplate(std::wstring_view pattern, const PrintTextContext& context) {
    std::wstring text(pattern);
    // 长的先换：$(FILE_NAME) 与 $(FILE_PATH) 都以 "$(FILE_" 开头，换错顺序会互相污染
    ReplaceAll(text, L"$(FILE_NAME)", context.fileName);
    ReplaceAll(text, L"$(FILE_PATH)", context.filePath);
    ReplaceAll(text, L"$(PAGES)", std::to_wstring(context.totalPages));
    ReplaceAll(text, L"$(PAGE)", std::to_wstring(context.page));
    ReplaceAll(text, L"$(DATE)", context.date);
    return text;
}

}  // namespace npcc::core
