#include "edit/Encoding.h"

#include <windows.h>

namespace npcc::edit {
namespace {

bool HasPrefix(std::string_view bytes, std::initializer_list<unsigned char> prefix) {
    if (bytes.size() < prefix.size()) {
        return false;
    }
    size_t i = 0;
    for (const unsigned char c : prefix) {
        if (static_cast<unsigned char>(bytes[i++]) != c) {
            return false;
        }
    }
    return true;
}

std::wstring ToWide(std::string_view utf16Bytes, bool bigEndian) {
    size_t unitCount = utf16Bytes.size() / 2;
    std::wstring wide(unitCount, L'\0');
    for (size_t i = 0; i < unitCount; ++i) {
        const unsigned int lo = static_cast<unsigned char>(utf16Bytes[2 * i]);
        const unsigned int hi = static_cast<unsigned char>(utf16Bytes[2 * i + 1]);
        wide[i] = static_cast<wchar_t>(bigEndian ? ((lo << 8) | hi) : ((hi << 8) | lo));
    }
    return wide;
}

std::string Utf16ToUtf8(std::string_view utf16Bytes, bool bigEndian) {
    const std::wstring wide = ToWide(utf16Bytes, bigEndian);
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

std::string Utf8ToUtf16(std::string_view utf8Bytes, bool bigEndian) {
    if (utf8Bytes.empty()) {
        std::string bomOnly;
        bomOnly.push_back(bigEndian ? static_cast<char>(0xFE) : static_cast<char>(0xFF));
        bomOnly.push_back(bigEndian ? static_cast<char>(0xFF) : static_cast<char>(0xFE));
        return bomOnly;
    }
    const int wideCount = ::MultiByteToWideChar(CP_UTF8, 0, utf8Bytes.data(),
                                                static_cast<int>(utf8Bytes.size()), nullptr, 0);
    if (wideCount <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(wideCount), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8Bytes.data(), static_cast<int>(utf8Bytes.size()),
                          wide.data(), wideCount);

    std::string out;
    out.reserve(2 + wide.size() * 2);
    out.push_back(bigEndian ? static_cast<char>(0xFE) : static_cast<char>(0xFF));
    out.push_back(bigEndian ? static_cast<char>(0xFF) : static_cast<char>(0xFE));
    for (const wchar_t wc : wide) {
        const unsigned int value = static_cast<unsigned int>(static_cast<uint16_t>(wc));
        if (bigEndian) {
            out.push_back(static_cast<char>((value >> 8) & 0xFF));
            out.push_back(static_cast<char>(value & 0xFF));
        } else {
            out.push_back(static_cast<char>(value & 0xFF));
            out.push_back(static_cast<char>((value >> 8) & 0xFF));
        }
    }
    return out;
}

const wchar_t* CodePageShortName(uint32_t codePage) {
    switch (codePage) {
        case 936:  return L"GBK";
        case 950:  return L"Big5";
        case 932:  return L"Shift-JIS";
        case 949:  return L"Korean";
        case 1252: return L"Latin-1";
        default:   return L"OEM";
    }
}

}  // namespace

bool IsValidUtf8(std::string_view bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned int c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t extra = 0;
        unsigned int codePoint = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            codePoint = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            codePoint = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            codePoint = c & 0x07u;
        } else {
            return false;   // 非法前导字节（含孤立续字节 0x80~0xBF）
        }
        if (i + extra >= bytes.size()) {
            return false;   // 尾部被截断
        }
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned int cc = static_cast<unsigned char>(bytes[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6) | (cc & 0x3Fu);
        }
        // 拒绝过长编码（overlong）与 UTF-16 代理区码点
        if ((extra == 1 && codePoint < 0x80) ||
            (extra == 2 && codePoint < 0x800) ||
            (extra == 3 && codePoint < 0x10000)) {
            return false;
        }
        if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

EolMode DetectEol(std::string_view bytes) {
    size_t crlf = 0;
    size_t lf = 0;
    size_t cr = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r') {
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (bytes[i] == '\n') {
            ++lf;
        }
    }
    const size_t total = crlf + lf + cr;
    if (total == 0 || crlf == total) {
        return EolMode::Crlf;
    }
    if (lf == total) {
        return EolMode::Lf;
    }
    if (cr == total) {
        return EolMode::Cr;
    }
    return EolMode::Mixed;   // 混合行尾：保存时保持原样，由用户显式统一
}

const char* EolSequence(EolMode eol) {
    switch (eol) {
        case EolMode::Lf: return "\n";
        case EolMode::Cr: return "\r";
        default:          return "\r\n";
    }
}

std::string NormalizeEol(std::string_view bytes, EolMode target) {
    if (target == EolMode::Mixed) {
        return std::string(bytes);
    }
    const std::string sequence = EolSequence(target);
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 16 + 16);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '\r') {
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') {
                ++i;
            }
            out += sequence;
        } else if (c == '\n') {
            out += sequence;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::wstring EolName(EolMode eol) {
    switch (eol) {
        case EolMode::Crlf:  return L"CRLF";
        case EolMode::Lf:    return L"LF";
        case EolMode::Cr:    return L"CR";
        case EolMode::Mixed: return L"Mixed";
    }
    return L"?";
}

std::wstring EncodingName(const EncodingState& state) {
    switch (state.uniMode) {
        case UniMode::Utf8:    return L"UTF-8";
        case UniMode::Utf8Bom: return L"UTF-8-BOM";
        case UniMode::Utf16LE: return L"UCS-2 LE";
        case UniMode::Utf16BE: return L"UCS-2 BE";
        case UniMode::Ansi:    break;
    }
    return std::wstring(L"ANSI(") + CodePageShortName(state.codePage) + L")";
}

LoadResult DetectAndPrepare(std::string_view raw, uint32_t fallbackCodePage) {
    LoadResult result;
    result.encoding.codePage = fallbackCodePage;

    if (HasPrefix(raw, {0xEF, 0xBB, 0xBF})) {
        result.encoding.uniMode = UniMode::Utf8Bom;
        result.encoding.hasBom = true;
        result.encoding.confidence = 100;
        result.editorBytes.assign(raw.begin() + 3, raw.end());
    } else if (HasPrefix(raw, {0xFF, 0xFE})) {
        result.encoding.uniMode = UniMode::Utf16LE;
        result.encoding.hasBom = true;
        result.encoding.confidence = 100;
        result.editorBytes = Utf16ToUtf8(raw.substr(2), false);
    } else if (HasPrefix(raw, {0xFE, 0xFF})) {
        result.encoding.uniMode = UniMode::Utf16BE;
        result.encoding.hasBom = true;
        result.encoding.confidence = 100;
        result.editorBytes = Utf16ToUtf8(raw.substr(2), true);
    } else if (raw.empty()) {
        result.encoding.uniMode = UniMode::Utf8;
        result.encoding.confidence = 100;
    } else if (IsValidUtf8(raw)) {
        result.encoding.uniMode = UniMode::Utf8;
        result.encoding.confidence = 95;
        result.editorBytes.assign(raw.begin(), raw.end());
    } else {
        // 严格 UTF-8 校验失败：按配置的候选代码页当作 ANSI 处理（字节原样交给 Scintilla）。
        result.encoding.uniMode = UniMode::Ansi;
        result.encoding.confidence = 80;
        result.editorBytes.assign(raw.begin(), raw.end());
    }

    result.eol = DetectEol(result.editorBytes);
    if (result.eol == EolMode::Mixed) {
        result.warning = L"检测到混合行尾，保存时将保持原样。";
    }
    return result;
}

EncodingState DetectEncodingFromHead(std::string_view head, uint32_t fallbackCodePage) {
    EncodingState state;
    state.codePage = fallbackCodePage;

    if (HasPrefix(head, {0xEF, 0xBB, 0xBF})) {
        state.uniMode = UniMode::Utf8Bom;
        state.hasBom = true;
        state.confidence = 100;
    } else if (HasPrefix(head, {0xFF, 0xFE})) {
        state.uniMode = UniMode::Utf16LE;
        state.hasBom = true;
        state.confidence = 100;
    } else if (HasPrefix(head, {0xFE, 0xFF})) {
        state.uniMode = UniMode::Utf16BE;
        state.hasBom = true;
        state.confidence = 100;
    } else if (head.empty() || IsValidUtf8(head)) {
        state.uniMode = UniMode::Utf8;
        state.confidence = head.empty() ? 100 : 95;
    } else {
        // 开头不是合法 UTF-8，按配置的代码页当作 ANSI
        state.uniMode = UniMode::Ansi;
        state.confidence = 80;
    }
    return state;
}

const char* UniModeToken(UniMode mode) {
    switch (mode) {
        case UniMode::Ansi:    return "Ansi";
        case UniMode::Utf8Bom: return "Utf8Bom";
        case UniMode::Utf16LE: return "Utf16LE";
        case UniMode::Utf16BE: return "Utf16BE";
        case UniMode::Utf8:
        default:               return "Utf8";
    }
}

const char* EolToken(EolMode eol) {
    switch (eol) {
        case EolMode::Lf:    return "Lf";
        case EolMode::Cr:    return "Cr";
        case EolMode::Mixed: return "Mixed";
        case EolMode::Crlf:
        default:             return "Crlf";
    }
}

bool ParseUniModeToken(std::string_view token, UniMode& out) {
    if (token == "Utf8") {
        out = UniMode::Utf8;
    } else if (token == "Utf8Bom") {
        out = UniMode::Utf8Bom;
    } else if (token == "Ansi") {
        out = UniMode::Ansi;
    } else if (token == "Utf16LE") {
        out = UniMode::Utf16LE;
    } else if (token == "Utf16BE") {
        out = UniMode::Utf16BE;
    } else {
        return false;
    }
    return true;
}

bool ParseEolToken(std::string_view token, EolMode& out) {
    if (token == "Crlf") {
        out = EolMode::Crlf;
    } else if (token == "Lf") {
        out = EolMode::Lf;
    } else if (token == "Cr") {
        out = EolMode::Cr;
    } else if (token == "Mixed") {
        out = EolMode::Mixed;
    } else {
        return false;
    }
    return true;
}

namespace {

// 编辑器字节 → 宽字符。ANSI 走代码页，其余都走 UTF-8。
bool BufferToWide(std::string_view bytes, const EncodingState& state, std::wstring& wide,
                  std::wstring& problem) {
    const bool isUtf8 = state.uniMode != UniMode::Ansi;
    const UINT codePage = isUtf8 ? CP_UTF8 : state.codePage;
    // MB_ERR_INVALID_CHARS：不加这个标志的话，非法 UTF-8 会被静默换成 U+FFFD
    // 并返回成功——我们要的是"发现并报出来"，而不是把坏字节悄悄改写掉。
    // 这个标志只对 UTF-8/UTF-16 有效，代码页转换传它会被直接判为无效参数。
    const DWORD flags = isUtf8 ? MB_ERR_INVALID_CHARS : 0;
    if (bytes.empty()) {
        wide.clear();
        return true;
    }
    const int count = ::MultiByteToWideChar(codePage, flags, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) {
        problem = L"当前内容无法按声明的编码解释（字节序列不合法）";
        return false;
    }
    wide.assign(static_cast<std::size_t>(count), L'\0');
    ::MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()),
                          wide.data(),
                          count);
    return true;
}

std::string WideToUtf8Bytes(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int count = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                            static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                            nullptr);
    if (count <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(count), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                          count, nullptr, nullptr);
    return out;
}

// 宽字符 → 代码页字节，遇到无法表示的字符就失败。
// 必须用 WC_NO_BEST_FIT_CHARS 并检查 usedDefaultChar：不这样做的话
// WideCharToMultiByte 会把无法表示的字符**静默替换成 '?'** 还返回成功，
// 用户的文档就在无声无息中被改坏了。
bool WideToAnsiStrict(const std::wstring& wide, uint32_t codePage, std::string& bytes,
                      std::wstring& problem) {
    if (wide.empty()) {
        bytes.clear();
        return true;
    }
    BOOL usedDefault = FALSE;
    const int count = ::WideCharToMultiByte(static_cast<UINT>(codePage), WC_NO_BEST_FIT_CHARS,
                                            wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                            nullptr, &usedDefault);
    if (count <= 0) {
        problem = L"代码页 " + std::to_wstring(codePage) + L" 不可用";
        return false;
    }
    std::string converted(static_cast<std::size_t>(count), '\0');
    usedDefault = FALSE;
    ::WideCharToMultiByte(static_cast<UINT>(codePage), WC_NO_BEST_FIT_CHARS, wide.data(),
                          static_cast<int>(wide.size()), converted.data(), count, nullptr,
                          &usedDefault);
    if (usedDefault != FALSE) {
        // 把表示不了的字符列出来给用户看：只说"有字符表示不了"没用，用户得知道是哪个
        std::wstring samples;
        for (const wchar_t ch : wide) {
            BOOL lossy = FALSE;
            const std::wstring one(1, ch);
            if (::WideCharToMultiByte(static_cast<UINT>(codePage), WC_NO_BEST_FIT_CHARS, one.data(),
                                      1, nullptr, 0, nullptr, &lossy) <= 0 ||
                lossy != FALSE) {
                if (samples.find(ch) == std::wstring::npos) {
                    if (!samples.empty()) {
                        samples += L"、";
                    }
                    samples += std::wstring(1, ch);
                }
                if (samples.size() >= 24) {
                    break;
                }
            }
        }
        problem = L"以下字符无法用代码页 " + std::to_wstring(codePage) + L" 表示：" + samples;
        return false;
    }
    bytes = std::move(converted);
    return true;
}

}  // namespace

ConvertResult ConvertTo(std::string_view editorBytes, const EncodingState& from,
                        UniMode targetMode, uint32_t targetCodePage) {
    ConvertResult result;
    std::wstring wide;
    if (!BufferToWide(editorBytes, from, wide, result.problem)) {
        return result;
    }
    if (targetMode == UniMode::Ansi) {
        if (!WideToAnsiStrict(wide, targetCodePage, result.editorBytes, result.problem)) {
            return result;
        }
    } else {
        result.editorBytes = WideToUtf8Bytes(wide);
    }
    result.encoding.uniMode = targetMode;
    result.encoding.codePage = targetMode == UniMode::Ansi ? targetCodePage : kCodePageUtf8;
    result.encoding.hasBom = targetMode == UniMode::Utf8Bom;
    result.encoding.confidence = 100;
    result.ok = true;
    return result;
}

std::string EncodeForSave(std::string_view editorBytes, const EncodingState& state) {
    switch (state.uniMode) {
        case UniMode::Ansi:
        case UniMode::Utf8:
            return std::string(editorBytes);
        case UniMode::Utf8Bom: {
            std::string out;
            out.reserve(editorBytes.size() + 3);
            out.append("\xEF\xBB\xBF", 3);
            out.append(editorBytes);
            return out;
        }
        case UniMode::Utf16LE:
            return Utf8ToUtf16(editorBytes, false);
        case UniMode::Utf16BE:
            return Utf8ToUtf16(editorBytes, true);
    }
    return std::string(editorBytes);
}

}  // namespace npcc::edit
