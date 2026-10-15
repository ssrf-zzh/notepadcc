#include "infra/CrashGuard.h"

#include <windows.h>

#include <cstddef>

#include <dbghelp.h>

// 这个文件里的所有代码都可能在"堆已经坏了"的状态下运行：
// 不用 CRT（strlen/wsprintf/std::string 一概不用），不做堆分配，
// 需要缓冲区就放栈上，需要大块内存就 VirtualAlloc。
// 多写几十行，换的是"崩溃时不会把本来能救的内容再弄丢一次"。

namespace npcc::infra::crashguard {
namespace {

constexpr std::size_t kPathChars = 700;

// dbghelp.h 只声明了函数，没给函数指针类型，自己写一个：
// 我们用 LoadLibrary + GetProcAddress 取地址，不链接 dbghelp.lib。
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE process, DWORD processId, HANDLE file,
                                          MINIDUMP_TYPE dumpType,
                                          const MINIDUMP_EXCEPTION_INFORMATION* exceptionParam,
                                          const MINIDUMP_USER_STREAM_INFORMATION* userStreamParam,
                                          const MINIDUMP_CALLBACK_INFORMATION* callbackParam);

wchar_t            g_backupRoot[512] = {};
MiniDumpWriteDumpFn g_writeDump = nullptr;
RescueDocument*    g_documents = nullptr;
int                g_documentCount = 0;
RescueProvider     g_provider;
volatile LONG      g_entered = 0;

// ---- 无堆的定长文本拼接 ----
struct WideText {
    wchar_t*    data;
    std::size_t capacity;
    std::size_t used;

    void Append(const wchar_t* text) {
        if (text == nullptr) {
            return;
        }
        while (*text != L'\0' && used + 1 < capacity) {
            data[used++] = *text++;
        }
        data[used] = L'\0';
    }
    void AppendNumber(unsigned long long value, int digits = 0) {
        wchar_t digitsBuffer[24] = {};
        int count = 0;
        do {
            digitsBuffer[count++] = static_cast<wchar_t>(L'0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count < digits) {
            digitsBuffer[count++] = L'0';
        }
        while (count > 0 && used + 1 < capacity) {
            data[used++] = digitsBuffer[--count];
        }
        data[used] = L'\0';
    }
    void AppendHex(unsigned long long value) {
        static const wchar_t* digits = L"0123456789ABCDEF";
        wchar_t digitsBuffer[18] = {};
        int count = 0;
        do {
            digitsBuffer[count++] = digits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        while (count > 0 && used + 1 < capacity) {
            data[used++] = digitsBuffer[--count];
        }
        data[used] = L'\0';
    }
    std::size_t Length() const { return used; }
};

struct Utf8Text {
    char*       data;
    std::size_t capacity;
    std::size_t used;

    void Append(const char* text) {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0' && used + 1 < capacity) {
            data[used++] = *text++;
        }
        data[used] = '\0';
    }
    void AppendWide(const wchar_t* text) {
        if (text == nullptr || *text == L'\0') {
            return;
        }
        if (used + 1 >= capacity) {
            return;
        }
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, data + used,
                                                  static_cast<int>(capacity - used - 1), nullptr,
                                                  nullptr);
        if (written <= 1) {
            return;
        }
        used += static_cast<std::size_t>(written - 1);
        data[used] = '\0';
    }
    void AppendNumber(unsigned long long value) {
        char digitsBuffer[24] = {};
        int count = 0;
        do {
            digitsBuffer[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0 && used + 1 < capacity) {
            data[used++] = digitsBuffer[--count];
        }
        data[used] = '\0';
    }
    void AppendHex(unsigned long long value) {
        static const char* digits = "0123456789ABCDEF";
        char digitsBuffer[18] = {};
        int count = 0;
        do {
            digitsBuffer[count++] = digits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        while (count > 0 && used + 1 < capacity) {
            data[used++] = digitsBuffer[--count];
        }
        data[used] = '\0';
    }
    std::size_t Length() const { return used; }
};

bool WriteAll(HANDLE file, const void* data, std::size_t size) {
    const char* cursor = static_cast<const char*>(data);
    std::size_t done = 0;
    while (done < size) {
        DWORD written = 0;
        const std::size_t remaining = size - done;
        const DWORD want = static_cast<DWORD>(remaining < (1u << 20) ? remaining : (1u << 20));
        if (::WriteFile(file, cursor + done, want, &written, nullptr) == 0 || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

void StampNow(wchar_t* out, std::size_t capacity) {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    WideText text{out, capacity, 0};
    text.AppendNumber(now.wYear, 4);
    text.AppendNumber(now.wMonth, 2);
    text.AppendNumber(now.wDay, 2);
    text.Append(L"-");
    text.AppendNumber(now.wHour, 2);
    text.AppendNumber(now.wMinute, 2);
    text.AppendNumber(now.wSecond, 2);
}

void ReadableNow(wchar_t* out, std::size_t capacity) {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    WideText text{out, capacity, 0};
    text.AppendNumber(now.wYear, 4);
    text.Append(L"-");
    text.AppendNumber(now.wMonth, 2);
    text.Append(L"-");
    text.AppendNumber(now.wDay, 2);
    text.Append(L" ");
    text.AppendNumber(now.wHour, 2);
    text.Append(L":");
    text.AppendNumber(now.wMinute, 2);
    text.Append(L":");
    text.AppendNumber(now.wSecond, 2);
}

// 抢救一个文档：写成与 core::SnapshotStore 完全相同的"文本头 + 原始字节"格式，
// 这样下次启动用同一套解析代码就能读出来。两边各写一遍格式是有风险的，
// 所以界面上有一段专门校验"崩溃目录里的快照能被恢复"的回归测试兜着。
bool RescueOne(const wchar_t* dir, int slot, int* outLength) {
    *outLength = 0;
    const RescueDocument& document = g_documents[slot];
    if (document.view == nullptr || !document.modified) {
        return false;
    }
    if (g_provider.lengthOf == nullptr || g_provider.copyInto == nullptr) {
        return false;
    }
    const int length = g_provider.lengthOf(g_provider.context, slot);
    if (length <= 0) {
        return false;
    }
    char* buffer = static_cast<char*>(::VirtualAlloc(
        nullptr, static_cast<SIZE_T>(length) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer == nullptr) {
        return false;
    }
    const int copied = g_provider.copyInto(g_provider.context, slot, buffer, length + 1);
    if (copied <= 0) {
        ::VirtualFree(buffer, 0, MEM_RELEASE);
        return false;
    }

    wchar_t path[kPathChars] = {};
    WideText pathText{path, kPathChars, 0};
    pathText.Append(dir);
    pathText.Append(L"\\");
    pathText.AppendNumber(static_cast<unsigned long long>(slot));
    pathText.Append(L".snap");

    bool ok = false;
    const HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char header[2048] = {};
        Utf8Text text{header, sizeof(header), 0};
        wchar_t stamp[40] = {};
        ReadableNow(stamp, sizeof(stamp) / sizeof(stamp[0]));
        text.Append("notepadcc-snapshot 1\r\n");
        text.Append("name=");
        text.AppendWide(document.name);
        text.Append("\r\npath=");
        text.AppendWide(document.path);
        text.Append("\r\nuntitled=");
        text.Append(document.path[0] == L'\0' ? "yes" : "no");
        text.Append("\r\nuniMode=");
        text.Append(document.uniMode);
        text.Append("\r\ncodePage=");
        text.AppendNumber(document.codePage);
        text.Append("\r\nhasBom=");
        text.Append(document.hasBom ? "yes" : "no");
        text.Append("\r\neol=");
        text.Append(document.eol);
        text.Append("\r\nsavedAt=");
        text.AppendWide(stamp);
        text.Append("\r\nlength=");
        text.AppendNumber(static_cast<unsigned long long>(copied));
        text.Append("\r\n\r\n");
        ok = WriteAll(file, header, text.Length()) &&
             WriteAll(file, buffer, static_cast<std::size_t>(copied));
        ::CloseHandle(file);
    }
    ::VirtualFree(buffer, 0, MEM_RELEASE);
    if (ok) {
        *outLength = copied;
    }
    return ok;
}

void WriteMiniDump(const wchar_t* dir, const wchar_t* stamp, EXCEPTION_POINTERS* info) {
    if (g_writeDump == nullptr) {
        return;
    }
    wchar_t path[kPathChars] = {};
    WideText text{path, kPathChars, 0};
    text.Append(dir);
    text.Append(L"\\notepadcc-");
    text.Append(stamp);
    text.Append(L".dmp");
    const HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION dumpInfo{};
    dumpInfo.ThreadId = ::GetCurrentThreadId();
    dumpInfo.ExceptionPointers = info;
    dumpInfo.ClientPointers = FALSE;
    const MINIDUMP_TYPE type =
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo);
    g_writeDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type, &dumpInfo, nullptr,
                nullptr);
    ::CloseHandle(file);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    if (::InterlockedExchange(&g_entered, 1) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;   // 处理器自己又崩了：立刻收手，别再递归
    }

    const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
                           ? info->ExceptionRecord->ExceptionCode
                           : 0;
    const void* address = (info != nullptr && info->ExceptionRecord != nullptr)
                              ? info->ExceptionRecord->ExceptionAddress
                              : nullptr;

    wchar_t stamp[40] = {};
    StampNow(stamp, sizeof(stamp) / sizeof(stamp[0]));
    wchar_t dir[kPathChars] = {};
    {
        WideText text{dir, kPathChars, 0};
        text.Append(g_backupRoot);
        text.Append(L"\\crash_");
        text.Append(stamp);
    }
    ::CreateDirectoryW(g_backupRoot, nullptr);
    ::CreateDirectoryW(dir, nullptr);

    // 先把未保存的内容抢救出来，再生成 minidump：万一 dmp 那步出问题，内容已经在盘上了
    char report[8192] = {};
    Utf8Text text{report, sizeof(report), 0};
    wchar_t readable[40] = {};
    ReadableNow(readable, sizeof(readable) / sizeof(readable[0]));
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    text.Append("notepadcc 崩溃报告\r\n时间: ");
    text.AppendWide(readable);
    text.Append("\r\n异常代码: 0x");
    text.AppendHex(code);
    text.Append("\r\n异常地址: 0x");
    text.AppendHex(reinterpret_cast<unsigned long long>(address));
    text.Append("\r\n进程 ID: ");
    text.AppendNumber(::GetCurrentProcessId());
    text.Append("\r\n程序: ");
    text.AppendWide(exePath);
    text.Append("\r\n未保存内容已抢救为同目录下的 <序号>.snap，下次启动可恢复。\r\n\r\n");

    int rescued = 0;
    for (int i = 0; i < g_documentCount && g_documents != nullptr; ++i) {
        const RescueDocument& document = g_documents[i];
        char slotText[16] = {};
        Utf8Text slot{slotText, sizeof(slotText), 0};
        slot.Append("[");
        slot.AppendNumber(static_cast<unsigned long long>(i));
        slot.Append("] ");
        text.Append(slotText);

        if (document.view == nullptr) {
            text.Append("(空槽位)\r\n");
            continue;
        }
        if (!document.modified) {
            text.Append("无未保存改动: ");
            text.AppendWide(document.path[0] != L'\0' ? document.path : document.name);
            text.Append("\r\n");
            continue;
        }
        int length = 0;
        if (RescueOne(dir, i, &length)) {
            ++rescued;
            text.Append("已抢救 ");
            text.AppendWide(document.path[0] != L'\0' ? document.path : document.name);
            text.Append(" (");
            text.AppendNumber(static_cast<unsigned long long>(length));
            text.Append(" 字节)\r\n");
        } else {
            text.Append("抢救失败: ");
            text.AppendWide(document.path[0] != L'\0' ? document.path : document.name);
            text.Append("\r\n");
        }
    }
    text.Append("\r\n抢救成功 ");
    text.AppendNumber(static_cast<unsigned long long>(rescued));
    text.Append(" 份。\r\n");

    wchar_t reportPath[kPathChars] = {};
    {
        WideText pathText{reportPath, kPathChars, 0};
        pathText.Append(dir);
        pathText.Append(L"\\crash.txt");
    }
    const HANDLE reportFile = ::CreateFileW(reportPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reportFile != INVALID_HANDLE_VALUE) {
        WriteAll(reportFile, report, text.Length());
        ::CloseHandle(reportFile);
    }

    WriteMiniDump(dir, stamp, info);

    // 提示放在最后：前面该落盘的东西都已经落盘了，这里崩了也无所谓
    wchar_t message[2048] = {};
    {
        WideText prompt{message, sizeof(message) / sizeof(message[0]), 0};
        prompt.Append(L"notepadcc 遇到问题需要关闭。\n\n崩溃信息、诊断转储与未保存的内容"
                      L"已写入：\n");
        prompt.Append(dir);
        prompt.Append(L"\n\n下次启动时会提示恢复未保存的内容。\n异常代码：0x");
        prompt.AppendHex(code);
    }
    ::MessageBoxW(nullptr, message, L"notepadcc 崩溃", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void Install(const wchar_t* backupRoot) {
    if (backupRoot != nullptr) {
        std::size_t index = 0;
        for (; backupRoot[index] != L'\0' && index + 1 < 512; ++index) {
            g_backupRoot[index] = backupRoot[index];
        }
        g_backupRoot[index] = L'\0';
    }
    // dbghelp 在启动时载入并把函数地址取好：崩溃路径上不再 LoadLibrary，
    // 那一步会分配内存、加锁，正是最不该在崩溃时做的事。
    const HMODULE module = ::LoadLibraryW(L"dbghelp.dll");
    if (module != nullptr) {
        // 先转 void* 再转函数指针：直接 reinterpret_cast 会触发
        // -Wcast-function-type（FARPROC 与具体签名不兼容）
        g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, "MiniDumpWriteDump")));
    }
    ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    ::SetUnhandledExceptionFilter(&CrashFilter);
}

void SetDocuments(RescueDocument* documents, int count, const RescueProvider& provider) {
    g_documents = documents;
    g_documentCount = count < kMaxRescueDocuments ? count : kMaxRescueDocuments;
    g_provider = provider;
}

void RaiseTestCrash() {
    volatile int* nullPointer = nullptr;
    *nullPointer = 42;   // 真正的空指针写：走的是 Windows 的访问违例，不是伪造的异常码
}

}  // namespace npcc::infra::crashguard
