#include "shell/Clipboard.h"

#include <cstring>

#include "infra/Log.h"

namespace npcc::shell {
namespace {

// 剪贴板是全机共享的：别的程序正开着的时候 OpenClipboard 会失败。
// 这不是错误，等一会儿再试就是（用户按复制的瞬间可能正有个工具在往剪贴板放东西）
bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (::OpenClipboard(owner) != 0) {
            return true;
        }
        ::Sleep(20);
    }
    return false;
}

// 放进剪贴板的内存必须用 GlobalAlloc(GMEM_MOVEABLE) 分配，而且**成功之后不能自己释放**：
// SetClipboardData 成功就是把所有权交出去了，再 GlobalFree 等于把系统要用的内存还回去
HANDLE AllocateClipboardBlock(std::size_t bytes) {
    HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    return handle;
}

}  // namespace

bool CopyRichTextToClipboard(HWND owner, const std::wstring& plain, const std::string& rtf) {
    if (!OpenClipboardWithRetry(owner)) {
        infra::LogWarn(L"clipboard", L"打不开剪贴板（被别的程序占着），复制为富文本失败");
        return false;
    }
    const UINT rtfFormat = ::RegisterClipboardFormatW(L"Rich Text Format");
    bool ok = false;
    HGLOBAL textBlock = nullptr;
    HGLOBAL rtfBlock = nullptr;
    do {
        if (::EmptyClipboard() == 0) {
            infra::LogWarn(L"clipboard", L"清空剪贴板失败，复制为富文本失败");
            break;
        }
        // 纯文本那一份：CF_UNICODETEXT 要求以 NUL 结尾，且算的是字节
        const std::size_t textBytes = (plain.size() + 1) * sizeof(wchar_t);
        textBlock = AllocateClipboardBlock(textBytes);
        if (textBlock == nullptr) {
            break;
        }
        void* textPointer = ::GlobalLock(textBlock);
        if (textPointer == nullptr) {
            break;
        }
        std::memcpy(textPointer, plain.c_str(), textBytes);
        ::GlobalUnlock(textBlock);
        if (::SetClipboardData(CF_UNICODETEXT, textBlock) == nullptr) {
            break;
        }
        textBlock = nullptr;   // 所有权已经交出去了

        if (rtfFormat == 0) {
            // 注册不了这个格式是系统层面的事，极少见；纯文本已经进剪贴板了，
            // 但按上面的约定"要么两种都有、要么都算失败"，这里报失败并说清楚
            infra::LogWarn(L"clipboard", L"注册 \"Rich Text Format\" 失败，只写进了纯文本");
            break;
        }
        const std::size_t rtfBytes = rtf.size() + 1;   // RTF 是 ASCII，字节数即字符数
        rtfBlock = AllocateClipboardBlock(rtfBytes);
        if (rtfBlock == nullptr) {
            break;
        }
        void* rtfPointer = ::GlobalLock(rtfBlock);
        if (rtfPointer == nullptr) {
            break;
        }
        std::memcpy(rtfPointer, rtf.c_str(), rtfBytes);
        ::GlobalUnlock(rtfBlock);
        if (::SetClipboardData(rtfFormat, rtfBlock) == nullptr) {
            break;
        }
        rtfBlock = nullptr;
        ok = true;
    } while (false);

    if (!ok) {
        // 失败时把剪贴板清空：只留下纯文本的话，用户粘到 Word 里拿到的是一份
        // "看起来成功了"的普通文本，而我们已经告诉他失败了——两种说法不能同时存在
        ::EmptyClipboard();
    }
    ::CloseClipboard();
    // 失败时自己收拾没交出去的那块内存（成功了的话这两个句柄都已经是 nullptr）
    if (textBlock != nullptr) {
        ::GlobalFree(textBlock);
    }
    if (rtfBlock != nullptr) {
        ::GlobalFree(rtfBlock);
    }
    return ok;
}

}  // namespace npcc::shell
