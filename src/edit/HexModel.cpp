#include "edit/HexModel.h"

#include <algorithm>
#include <cwctype>

namespace npcc::edit {
namespace {

constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";

void AppendHexByte(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(kHexDigits[(value >> 4) & 0x0F]));
    out.push_back(static_cast<char>(kHexDigits[value & 0x0F]));
}

bool IsPrintableAscii(std::uint8_t value) {
    return value >= 0x20 && value <= 0x7E;
}

}  // namespace

int HexOffsetWidthFor(std::uint64_t fileSize) {
    return fileSize > 0xFFFFFFFFull ? 16 : 8;
}

std::string FormatHexOffset(std::uint64_t offset, int width) {
    if (width < 1) {
        width = 1;
    }
    if (width > 16) {
        width = 16;
    }
    std::string out(static_cast<std::size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = static_cast<char>(kHexDigits[offset & 0x0F]);
        offset >>= 4;
    }
    return out;
}

std::string HexRowText(const std::uint8_t* data, std::size_t length, std::uint64_t offset,
                       int offsetWidth) {
    if (length > static_cast<std::size_t>(kHexBytesPerRow)) {
        length = static_cast<std::size_t>(kHexBytesPerRow);
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(kHexRowWidth));
    out += FormatHexOffset(offset, offsetWidth);
    out.append(static_cast<std::size_t>(std::max(0, kHexTextStart - static_cast<int>(out.size()))),
               ' ');

    // 十六进制一列：**不管本行有几个字节，都排满 16 个位置**，缺的地方填空格——
    // 否则最后一行的 ASCII 列会整体左移，看着像错位
    for (int i = 0; i < kHexBytesPerRow; ++i) {
        if (i < static_cast<int>(length) && data != nullptr) {
            AppendHexByte(out, data[i]);
        } else {
            out.append(2, ' ');
        }
        if (i != kHexBytesPerRow - 1) {
            out.push_back(' ');
        }
        if (i == kHexHalf - 1) {
            out.push_back(' ');
        }
    }
    out.append(2, ' ');
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t value = data != nullptr ? data[i] : 0;
        out.push_back(IsPrintableAscii(value) ? static_cast<char>(value) : '.');
    }
    return out;
}

std::string HexBlockText(const std::uint8_t* data, std::size_t length, std::uint64_t startOffset,
                         std::uint64_t byteCount, int offsetWidth) {
    std::string out;
    if (data == nullptr || byteCount == 0 || startOffset >= length) {
        return out;
    }
    const std::uint64_t available = static_cast<std::uint64_t>(length) - startOffset;
    const std::uint64_t count = std::min(available, byteCount);
    for (std::uint64_t done = 0; done < count; done += kHexBytesPerRow) {
        const std::uint64_t chunk =
            std::min<std::uint64_t>(static_cast<std::uint64_t>(kHexBytesPerRow), count - done);
        if (!out.empty()) {
            out += "\r\n";
        }
        out += HexRowText(data + startOffset + done, static_cast<std::size_t>(chunk),
                          startOffset + done, offsetWidth);
    }
    return out;
}

bool ParseHexJump(std::wstring_view text, std::uint64_t& out) {
    while (!text.empty() && std::iswspace(static_cast<wint_t>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::iswspace(static_cast<wint_t>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }
    int base = 10;
    if (text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const wchar_t ch : text) {
        int digit = -1;
        if (ch >= L'0' && ch <= L'9') {
            digit = ch - L'0';
        } else if (base == 16 && ch >= L'a' && ch <= L'f') {
            digit = ch - L'a' + 10;
        } else if (base == 16 && ch >= L'A' && ch <= L'F') {
            digit = ch - L'A' + 10;
        } else {
            return false;
        }
        if (value > (UINT64_MAX - static_cast<std::uint64_t>(digit)) /
                        static_cast<std::uint64_t>(base)) {
            return false;   // 溢出
        }
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
    }
    out = value;
    return true;
}

bool HexByteAtColumn(int column, int rowBytes, int* indexInRow) {
    if (rowBytes <= 0 || rowBytes > kHexBytesPerRow) {
        return false;
    }
    int index = -1;
    if (column >= kHexTextStart && column < kHexTextStart + kHexTextWidth) {
        const int relative = column - kHexTextStart;
        index = relative / 3;
        // 第 8 个字节后面那个多出来的空档落在 relative 23：23/3 = 7，正好归给左边那个字节
        if (index >= kHexBytesPerRow) {
            index = kHexBytesPerRow - 1;
        }
    } else if (column >= kHexAsciiStart && column < kHexAsciiStart + kHexBytesPerRow) {
        index = column - kHexAsciiStart;
    } else {
        return false;
    }
    if (index < 0 || index >= rowBytes) {
        return false;   // 最后一行补齐的空档不算字节
    }
    if (indexInRow != nullptr) {
        *indexInRow = index;
    }
    return true;
}

}  // namespace npcc::edit
