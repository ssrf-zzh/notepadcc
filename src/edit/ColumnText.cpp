#include "edit/ColumnText.h"

#include <algorithm>
#include <cstdlib>

namespace npcc::edit {
namespace {

std::wstring FormatBase16(long long value) {
    const bool negative = value < 0;
    unsigned long long magnitude =
        negative ? static_cast<unsigned long long>(-value) : static_cast<unsigned long long>(value);
    std::wstring digits;
    if (magnitude == 0) {
        digits = L"0";
    }
    const wchar_t* hex = L"0123456789ABCDEF";
    while (magnitude != 0) {
        digits.insert(digits.begin(), hex[magnitude & 0xF]);
        magnitude >>= 4;
    }
    return negative ? L"-" + digits : digits;
}

}  // namespace

std::wstring FormatSequenceValue(long long value, int zeroPad, int base) {
    std::wstring text = (base == 16) ? FormatBase16(value) : std::to_wstring(value);

    if (zeroPad <= 0) {
        return text;
    }
    // 补零只补数字部分，负号留在最前面
    const bool negative = !text.empty() && text[0] == L'-';
    const std::wstring body = negative ? text.substr(1) : text;
    if (static_cast<int>(body.size()) >= zeroPad) {
        return text;
    }
    const std::wstring padded(static_cast<std::size_t>(zeroPad) - body.size(), L'0');
    return negative ? L"-" + padded + body : padded + body;
}

std::vector<std::wstring> BuildColumnInsertTexts(const ColumnInsertOptions& options) {
    std::vector<std::wstring> texts;
    if (options.lineCount <= 0) {
        return texts;
    }
    texts.reserve(static_cast<std::size_t>(options.lineCount));

    const std::wstring placeholder(kColumnIndexPlaceholder);
    const bool usesSequence = options.templateText.find(placeholder) != std::wstring::npos;

    for (int i = 0; i < options.lineCount; ++i) {
        if (!usesSequence) {
            texts.push_back(options.templateText);
            continue;
        }
        const int index = (options.resetEvery > 0) ? (i % options.resetEvery) : i;
        const long long value = options.start + options.step * static_cast<long long>(index);
        const std::wstring number =
            FormatSequenceValue(value, options.zeroPad, options.base);

        // 替换全部占位符，方便 "0x\i\_\i\" 这类模板
        std::wstring line = options.templateText;
        for (std::size_t pos = line.find(placeholder); pos != std::wstring::npos;
             pos = line.find(placeholder, pos + number.size())) {
            line.replace(pos, placeholder.size(), number);
        }
        texts.push_back(std::move(line));
    }
    return texts;
}

}  // namespace npcc::edit
