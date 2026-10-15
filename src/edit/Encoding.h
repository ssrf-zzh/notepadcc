#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace npcc::edit {

// Scintilla 用这个代码页号表示"按 UTF-8 解释缓冲区"
inline constexpr uint32_t kCodePageUtf8 = 65001;

// 文档的原始编码形态。UniMode 记录的是"文件里的字节长什么样"，
// 编辑器缓冲区内部对 UTF-16 会转成 UTF-8，保存时再转回去（见 EncodeForSave）。
enum class UniMode : uint8_t {
    Ansi,      // 以 codePage 指定的代码页解释，无 BOM
    Utf8,      // UTF-8 无 BOM
    Utf8Bom,   // UTF-8 带 BOM
    Utf16LE,
    Utf16BE,
};

enum class EolMode : uint8_t { Crlf, Lf, Cr, Mixed };

struct EncodingState {
    UniMode  uniMode    = UniMode::Ansi;
    uint32_t codePage   = 936;    // 仅 Ansi 模式有意义
    bool     hasBom     = false;
    uint8_t  confidence = 100;    // 探测置信度，低于 80 时应在状态栏提示
};

struct LoadResult {
    EncodingState encoding;
    std::string   editorBytes;    // 交给 Scintilla 的字节流（UTF-8 或 Ansi 原字节）
    EolMode       eol = EolMode::Crlf;
    std::wstring  warning;        // 非空时由调用方提示用户
    bool          editable = true;
};

// 探测编码并准备编辑器字节流。fallbackCodePage 用于无法判定时的 ANSI 代码页。
LoadResult DetectAndPrepare(std::string_view raw, uint32_t fallbackCodePage);

// 只探测编码、不做任何转换。大文件路径用它：为了探测把整份文件复制一遍
// 就违背了"映射装载"的初衷，所以只看开头一段。
EncodingState DetectEncodingFromHead(std::string_view head, uint32_t fallbackCodePage);

// 把编辑器字节流按原始编码写回磁盘格式（含 BOM 处理）。
std::string EncodeForSave(std::string_view editorBytes, const EncodingState& state);

bool IsValidUtf8(std::string_view bytes);
EolMode DetectEol(std::string_view bytes);
std::string NormalizeEol(std::string_view bytes, EolMode target);

const char* EolSequence(EolMode eol);
std::wstring EolName(EolMode eol);
std::wstring EncodingName(const EncodingState& state);

}  // namespace npcc::edit
