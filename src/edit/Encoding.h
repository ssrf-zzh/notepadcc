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

// ---- 持久化用的稳定标识 ----
// config.xml 与快照文件都写这几个词，别再各写一份映射：抄成几份之后，
// 早晚会出现"某一份把 Utf8 与 Utf8Bom 的顺序记反了"这种安静的错误。
// 改动这些字符串等于改文件格式（老配置、老快照会读不懂），不要随手改。
const char* UniModeToken(UniMode mode);
const char* EolToken(EolMode eol);
// 不认识时返回 false，且不改动 out（调用方据此记警告并回退默认值）
bool ParseUniModeToken(std::string_view token, UniMode& out);
bool ParseEolToken(std::string_view token, EolMode& out);

// ---- 编码转换（设计文档 6.2）----
// 把编辑器缓冲区从一种编码形态转到另一种。纯函数：给字节与状态，返回新字节与新状态，
// 不做任何 IO，也不碰窗口——转换是不可逆的高风险操作，逻辑要能单独测。
//
// 注意编辑器缓冲区的表示：UTF-8 / UTF-8-BOM / UTF-16LE / UTF-16BE 这几种在缓冲区里
// 一律是 UTF-8（保存时才按 uniMode 编回去），只有 ANSI 是原始代码页字节。所以
// "转为 UTF-16" 只需要改状态，而 "转为 ANSI" 必须真的把缓冲区字节换成代码页字节。
struct ConvertResult {
    bool         ok = false;
    std::string  editorBytes;    // 转换后的缓冲区字节
    EncodingState encoding;      // 转换后的编码状态
    std::wstring problem;        // ok=false 时说明原因（含无法表示的字符样例）
};

ConvertResult ConvertTo(std::string_view editorBytes, const EncodingState& from, UniMode targetMode,
                        uint32_t targetCodePage);

}  // namespace npcc::edit
