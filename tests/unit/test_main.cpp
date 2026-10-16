// notepadcc 单元测试：只覆盖不依赖窗口的纯逻辑（设计文档 9.1 的单元测试层）。
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "core/Command.h"
#include "core/CommandRegistry.h"
#include "core/CommandText.h"
#include "core/ToolbarItems.h"
#include "core/ConfigCenter.h"
#include "core/DocumentManager.h"
#include "core/SessionManager.h"
#include "core/ShortcutStore.h"
#include "core/Macro.h"
#include "core/FolderTree.h"
#include "core/FunctionList.h"
#include "core/ExportHtml.h"
#include "core/ExportRtf.h"
#include "core/MarkdownRender.h"
#include "core/Diff.h"
#include "core/JsonTree.h"
#include "core/PrintText.h"
#include "core/PrintPaging.h"
#include "plugin/PluginHost.h"
#include "core/JsonFlat.h"
#include "core/JsonFormat.h"
#include "core/XmlFormat.h"
#include "core/Localization.h"
#include "core/ExportRtf.h"
#include "shell/FolderPanel.h"
#include "shell/FunctionListPanel.h"
#include "core/SnapshotStore.h"
#include "infra/SingleInstance.h"
#include "infra/FileAssociation.h"
#include "edit/DocBuffer.h"
#include "edit/ColumnEdit.h"
#include "edit/ColumnText.h"
#include "edit/Encoding.h"
#include "edit/HexModel.h"
#include "edit/FileTier.h"
#include "edit/TextTools.h"
#include "edit/StyleScheme.h"
#include "edit/StyleCatalogue.h"
#include "edit/EditView.h"
#include "edit/LanguageRegistry.h"
#include "edit/UdlDefinition.h"
#include "edit/UdlStyles.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"
#include "io/FileWatcher.h"
#include "io/MappedFile.h"
#include "io/XmlFile.h"
#include "search/EscapeSequence.h"
#include "search/CrossFileSearch.h"
#include "search/CrossFileReplace.h"
#include "search/InDocSearch.h"
#include "search/RegexEngine.h"
#include "search/Wildcard.h"
#include "infra/Paths.h"
#include "infra/Xml.h"

// 只用它拿风格号常量（STYLE_DEFAULT / STYLE_LINENUMBER），不碰任何窗口逻辑
#include <Scintilla.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("    [FAIL] %s\n", what);
    }
}

// 把字符串转成一行可读的调试文本：换行显示为 \n，过长截断
std::string QuoteForLog(const std::string& text, std::size_t limit = 120) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && out.size() < limit; ++i) {
        const char ch = text[i];
        if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else {
            out += ch;
        }
    }
    if (text.size() > limit) {
        out += "...";
    }
    return out;
}

void CheckEqual(const std::string& actual, const std::string& expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        // 失败时把两边的内容也打出来——只看字节数很难判断差在哪里
        std::printf("    [FAIL] %s\n", what);
        std::printf("           实际(%u): %s\n", static_cast<unsigned>(actual.size()),
                    QuoteForLog(actual).c_str());
        std::printf("           期望(%u): %s\n", static_cast<unsigned>(expected.size()),
                    QuoteForLog(expected).c_str());
    }
}

// 每段报出"本段多少项检查"：文档里那些"这一段占 N 项"的说法才有人能核对，
// 也不至于总数一涨就变成谁也说不清对不对的旧数字。
void PrintSection(const char* name, int count) {
    std::printf("  %s（%d 项）\n", name, count);
}

const char* g_sectionName = nullptr;
int g_sectionStart = 0;

void Section(const char* name) {
    // 段名在下一段开始（或收尾）时才打印，此时才知道这一段跑了多少项
    if (g_sectionName != nullptr) {
        PrintSection(g_sectionName, g_checks - g_sectionStart);
    }
    g_sectionName = name;
    g_sectionStart = g_checks;
}

void FinishSections() {
    if (g_sectionName != nullptr) {
        PrintSection(g_sectionName, g_checks - g_sectionStart);
        g_sectionName = nullptr;
    }
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          size, nullptr, nullptr);
    return out;
}

void CheckWide(const std::wstring& actual, const std::wstring& expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("    [FAIL] %s\n", what);
        std::printf("           实际: %s\n", QuoteForLog(ToUtf8(actual)).c_str());
        std::printf("           期望: %s\n", QuoteForLog(ToUtf8(expected)).c_str());
    }
}

npcc::core::CmdId AddTestCommand(npcc::core::CommandRegistry& registry,
                                 npcc::core::CmdCategory category, const wchar_t* key,
                                 const wchar_t* text) {
    npcc::core::Command command;
    command.category = category;
    command.key = key;
    command.text = text;
    command.execute = [](const npcc::core::CmdContext&) {};
    return registry.Register(std::move(command));
}

LRESULT CALLBACK TestHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// 文件夹工作区面板向外壳发的那种通知，测试里需要一个能把它收下来的宿主窗口
std::wstring g_lastFolderNotifyPath;
int          g_folderNotifyCount = 0;
int          g_functionNotifyLine = -1;
int          g_functionNotifyCount = 0;

LRESULT CALLBACK NotifyHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header != nullptr && header->code == npcc::shell::kFolderNotifyCode) {
            const auto* notification = reinterpret_cast<const npcc::shell::FolderNotify*>(lParam);
            g_lastFolderNotifyPath = notification->path;
            ++g_folderNotifyCount;
        } else if (header != nullptr && header->code == npcc::shell::kFunctionNotifyCode) {
            const auto* notification =
                reinterpret_cast<const npcc::shell::FunctionNotify*>(lParam);
            g_functionNotifyLine = notification->line;
            ++g_functionNotifyCount;
        }
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateNotifyHost() {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &NotifyHostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"notepadccNotifyHost";
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, L"notepadccNotifyHost", L"", 0, 0, 0, 200, 200, nullptr, nullptr,
                             instance, nullptr);
}

// 直接写文件。不要用 WriteFileAtomic——它走"临时文件 + 改名替换"，
// 系统上报的是替换/改名而不是修改，测不出 Modified 通知。
bool WriteFileDirect(const std::filesystem::path& path, std::string_view bytes) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ::CloseHandle(file);
    return true;
}

// 建立一个隐藏宿主窗口。多文档逻辑要创建真实的 Scintilla 控件，
// 所以这部分测试需要窗口，但不需要显示出来。
HWND CreateHiddenHost() {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TestHostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"notepadccTestHost";
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, L"notepadccTestHost", L"", 0, 0, 0, 200, 200,
                             nullptr, nullptr, instance, nullptr);
}

std::string ToUtf16Bytes(const std::wstring& text, bool bigEndian) {
    std::string out;
    out.push_back(bigEndian ? static_cast<char>(0xFE) : static_cast<char>(0xFF));
    out.push_back(bigEndian ? static_cast<char>(0xFF) : static_cast<char>(0xFE));
    for (const wchar_t ch : text) {
        const unsigned int value = static_cast<unsigned int>(static_cast<uint16_t>(ch));
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

}  // namespace

int main() {
    std::printf("notepadcc 单元测试\n");

    // 用例一律按便携模式跑，配置写在 <测试程序目录>/config 下。
    // 这里必须自己把这个目录建出来：各段虽然都会调 InitializePaths(true)，但那是幂等的 no-op，
    // 而 config 目录本身没人负责创建——真外壳靠 InitLogging() 顺手建出来，单元测试不初始化日志，
    // 于是"在全新构建目录里第一次跑"会有一批保存类用例失败（实测 54 项，第二次跑才正常）。
    npcc::infra::InitializePaths(true);
    {
        std::error_code ec;
        std::filesystem::create_directories(npcc::infra::ConfigRoot(), ec);
    }

    Section("IsValidUtf8");
    Check(npcc::edit::IsValidUtf8("plain ascii"), "纯 ASCII 合法");
    Check(npcc::edit::IsValidUtf8("中文内容"), "UTF-8 中文合法");
    Check(npcc::edit::IsValidUtf8("emoji \xF0\x9F\x98\x80"), "4 字节码点合法");
    Check(!npcc::edit::IsValidUtf8("\xB5\xDA\xD2\xBB"), "GBK 字节序列应判为非法");
    Check(!npcc::edit::IsValidUtf8("\xC0\xAF"), "overlong 编码应判为非法");
    Check(!npcc::edit::IsValidUtf8("\xE4\xB8"), "截断的多字节序列应判为非法");
    Check(!npcc::edit::IsValidUtf8("\xED\xA0\x80"), "UTF-16 代理区码点应判为非法");

    Section("DetectEol / NormalizeEol");
    Check(npcc::edit::DetectEol("a\r\nb\r\n") == npcc::edit::EolMode::Crlf, "CRLF 识别");
    Check(npcc::edit::DetectEol("a\nb\n") == npcc::edit::EolMode::Lf, "LF 识别");
    Check(npcc::edit::DetectEol("a\rb\r") == npcc::edit::EolMode::Cr, "CR 识别");
    Check(npcc::edit::DetectEol("a\r\nb\n") == npcc::edit::EolMode::Mixed, "混合行尾识别");
    Check(npcc::edit::DetectEol("no newline") == npcc::edit::EolMode::Crlf, "无换行时取默认");
    CheckEqual(npcc::edit::NormalizeEol("a\r\nb\nc\rd", npcc::edit::EolMode::Lf), "a\nb\nc\nd",
               "统一为 LF");
    CheckEqual(npcc::edit::NormalizeEol("a\nb", npcc::edit::EolMode::Crlf), "a\r\nb",
               "统一为 CRLF");
    CheckEqual(npcc::edit::NormalizeEol("a\r\nb", npcc::edit::EolMode::Mixed), "a\r\nb",
               "Mixed 时保持原样");

    Section("DetectAndPrepare");
    {
        const auto plain = npcc::edit::DetectAndPrepare("hello world", 936);
        Check(plain.encoding.uniMode == npcc::edit::UniMode::Utf8, "纯 ASCII 判为 UTF-8");
        CheckEqual(plain.editorBytes, "hello world", "纯 ASCII 内容不变");
    }
    {
        const std::string raw = std::string("\xEF\xBB\xBF") + "中文";
        const auto withBom = npcc::edit::DetectAndPrepare(raw, 936);
        Check(withBom.encoding.uniMode == npcc::edit::UniMode::Utf8Bom, "识别 UTF-8 BOM");
        Check(withBom.encoding.hasBom, "BOM 标志置位");
        CheckEqual(withBom.editorBytes, "中文", "交给编辑器时剥离 BOM");
    }
    {
        const std::string gbk("\xB5\xDA\xD2\xBB\xD0\xD0", 6);
        const auto ansi = npcc::edit::DetectAndPrepare(gbk, 936);
        Check(ansi.encoding.uniMode == npcc::edit::UniMode::Ansi, "非 UTF-8 判为 ANSI");
        Check(ansi.encoding.codePage == 936, "沿用候选代码页");
        CheckEqual(ansi.editorBytes, gbk, "ANSI 字节原样交给编辑器");
    }
    {
        const std::string utf16 = ToUtf16Bytes(L"中文", false);
        const auto le = npcc::edit::DetectAndPrepare(utf16, 936);
        Check(le.encoding.uniMode == npcc::edit::UniMode::Utf16LE, "识别 UTF-16LE");
        CheckEqual(le.editorBytes, "中文", "UTF-16LE 转成 UTF-8 交给编辑器");
    }
    {
        const std::string utf16 = ToUtf16Bytes(L"中文", true);
        const auto be = npcc::edit::DetectAndPrepare(utf16, 936);
        Check(be.encoding.uniMode == npcc::edit::UniMode::Utf16BE, "识别 UTF-16BE");
        CheckEqual(be.editorBytes, "中文", "UTF-16BE 转成 UTF-8 交给编辑器");
    }
    {
        const auto empty = npcc::edit::DetectAndPrepare("", 936);
        Check(empty.encoding.uniMode == npcc::edit::UniMode::Utf8, "空文件按 UTF-8 处理");
    }

    Section("EncodeForSave");
    {
        npcc::edit::EncodingState state;
        state.uniMode = npcc::edit::UniMode::Utf8Bom;
        CheckEqual(npcc::edit::EncodeForSave("中文", state),
                   std::string("\xEF\xBB\xBF") + "中文", "UTF-8-BOM 写出带 BOM");
    }
    {
        npcc::edit::EncodingState state;
        state.uniMode = npcc::edit::UniMode::Utf8;
        CheckEqual(npcc::edit::EncodeForSave("中文", state), "中文", "UTF-8 写出不带 BOM");
    }
    {
        npcc::edit::EncodingState state;
        state.uniMode = npcc::edit::UniMode::Ansi;
        const std::string gbk("\xB5\xDA\xD2\xBB", 4);
        CheckEqual(npcc::edit::EncodeForSave(gbk, state), gbk, "ANSI 原样写出");
    }
    for (const bool bigEndian : {false, true}) {
        npcc::edit::EncodingState state;
        state.uniMode = bigEndian ? npcc::edit::UniMode::Utf16BE : npcc::edit::UniMode::Utf16LE;
        const std::string bytes = npcc::edit::EncodeForSave("中文往返", state);
        const auto again = npcc::edit::DetectAndPrepare(bytes, 936);
        CheckEqual(again.editorBytes, "中文往返", "UTF-16 保存后再打开内容一致");
    }

    Section("WriteFileAtomic / ReadWholeFile");
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "notepadcc_unit_test";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path target = dir / "sample.txt";

        const std::string first = "第一版内容\r\n";
        Check(npcc::io::WriteFileAtomic(target.wstring(), first).ok, "首次写入成功");
        const auto read1 = npcc::io::ReadWholeFile(target.wstring());
        Check(read1.ok, "读回成功");
        CheckEqual(read1.bytes, first, "首次写入内容一致");

        const std::string second = "第二版内容，覆盖写入\r\n";
        Check(npcc::io::WriteFileAtomic(target.wstring(), second).ok, "覆盖写入成功");
        const auto read2 = npcc::io::ReadWholeFile(target.wstring());
        CheckEqual(read2.bytes, second, "覆盖后内容一致");

        const std::filesystem::path badTarget = dir / "no_such_dir" / "x.txt";
        const auto bad = npcc::io::WriteFileAtomic(badTarget.wstring(), "x");
        Check(!bad.ok, "目录不存在时写入应失败");
        Check(!bad.message.empty(), "失败时给出可读原因");

        int leftovers = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".cpptmp") {
                ++leftovers;
            }
        }
        Check(leftovers == 0, "不残留 .cpptmp 临时文件");
        std::filesystem::remove_all(dir, ec);
    }

    Section("CommandRegistry");
    {
        npcc::core::CommandRegistry registry;
        int calls = 0;

        npcc::core::Command command;
        command.id = npcc::core::MakeCmdId(1, 1);
        command.category = npcc::core::CmdCategory::File;
        command.text = L"测试命令";
        command.flags = npcc::core::kCmdNeedsBuffer;
        command.execute = [&calls](const npcc::core::CmdContext&) { ++calls; };
        const npcc::core::CmdId id = registry.Register(std::move(command));

        const npcc::core::CmdContext empty;
        Check(registry.Execute(id, empty) == npcc::core::ExecResult::Disabled,
              "缺少缓冲区时命令被禁用");
        Check(calls == 0, "禁用状态下不执行回调");

        npcc::edit::DocBuffer buffer;
        npcc::core::CmdContext ready;
        ready.buffer = &buffer;
        Check(registry.Execute(id, ready) == npcc::core::ExecResult::Ok, "条件满足时执行成功");
        Check(calls == 1, "回调被调用一次");

        buffer.isReadOnly = true;
        npcc::core::Command writeCommand;
        writeCommand.id = npcc::core::MakeCmdId(2, 1);
        writeCommand.category = npcc::core::CmdCategory::Edit;
        writeCommand.text = L"写命令";
        writeCommand.flags = npcc::core::kCmdNeedsBuffer | npcc::core::kCmdNeedsWritable;
        writeCommand.execute = [](const npcc::core::CmdContext&) {};
        const npcc::core::CmdId writeId = registry.Register(std::move(writeCommand));
        Check(!registry.IsEnabled(writeId, ready), "只读缓冲区下写命令被禁用");
        buffer.isReadOnly = false;
        Check(registry.IsEnabled(writeId, ready), "可写缓冲区下写命令可用");

        registry.BindShortcut(id, npcc::core::kModCtrl, 'K', true);
        Check(registry.ResolveShortcut(npcc::core::kModCtrl, 'K') == id, "快捷键解析");
        Check(registry.ResolveShortcut(npcc::core::kModCtrl, 'Q') == 0, "未绑定的组合返回 0");
        Check(registry.FindConflicts(npcc::core::kModCtrl, 'K').size() == 1, "冲突检测命中 1 条");
        Check(npcc::core::MakeCmdId(1, 1) <= 0xFFFF, "命令 ID 落在 16 位内（菜单与加速键要求）");
        Check(npcc::core::MakeCmdId(1, 1) != npcc::core::MakeCmdId(2, 1), "不同类别不冲突");

        // 按**命令键**查（工具栏按钮表记的是键）：与 ID 查到的是同一条；
        // 认不出的键与空键都返回空
        {
            npcc::core::Command keyed;
            keyed.id = npcc::core::MakeCmdId(1, 2);
            keyed.category = npcc::core::CmdCategory::File;
            keyed.key = L"cmd.file.sample";
            keyed.text = L"带键的命令";
            keyed.execute = [](const npcc::core::CmdContext&) {};
            const npcc::core::CmdId keyedId = registry.Register(std::move(keyed));
            const npcc::core::Command* byKey = registry.FindByKey(L"cmd.file.sample");
            Check(byKey != nullptr && byKey->id == keyedId, "按键查得到，且是 ID 查到的那一条");
            Check(registry.FindByKey(L"cmd.file.missing") == nullptr, "认不出的键返回空");
            Check(registry.FindByKey(L"") == nullptr, "空键返回空");
        }

        // 自动分配 ID 必须绕开用过的号。曾经踩过：菜单分隔线用自动 ID，
        // 而它正好等于紧随其后的那条命令的显式 ID，于是那条命令被静默丢弃——
        // 表现是"菜单里有这一项，点了没反应"。
        {
            npcc::core::CommandRegistry autoIds;
            npcc::core::Command explicitOne;
            explicitOne.id = 4097;                  // 让 nextAutoId_ 走到 4098
            explicitOne.category = npcc::core::CmdCategory::Edit;
            explicitOne.text = L"显式命令";
            explicitOne.execute = [](const npcc::core::CmdContext&) {};
            autoIds.Register(std::move(explicitOne));

            npcc::core::Command later;
            later.id = 4098;                        // 正好是自动分配会先挑的那个号
            later.category = npcc::core::CmdCategory::Tools;
            later.text = L"后来的命令";
            later.execute = [](const npcc::core::CmdContext&) {};
            const npcc::core::CmdId laterId = autoIds.Register(std::move(later));

            auto addAuto = [&autoIds](const wchar_t* text) {
                npcc::core::Command autoCommand;
                autoCommand.category = npcc::core::CmdCategory::Tools;
                autoCommand.text = text;
                autoCommand.execute = [](const npcc::core::CmdContext&) {};
                return autoIds.Register(std::move(autoCommand));
            };
            const npcc::core::CmdId auto1 = addAuto(L"自动一");
            const npcc::core::CmdId auto2 = addAuto(L"自动二");

            Check(laterId == 4098, "显式 ID 不被自动分配抢走");
            Check(auto1 != laterId && auto2 != laterId && auto1 != auto2,
                  "自动分配绕开已用号且彼此不同");
            Check(autoIds.All().size() == 4, "四条命令都注册上了（没有被静默丢弃）");
            Check(autoIds.Find(laterId) != nullptr && autoIds.Find(auto1) != nullptr,
                  "注册过的命令都查得到");
        }
    }

    // ---------------------------------------------------------------- 工具栏按钮表
    // 纯函数：默认表长什么样、用户给的表怎么整理。工具栏控件本身要窗口，
    // 这一段只盯"按钮表"这层逻辑；控件与配置的联动由界面回归盯
    Section("工具栏按钮表（默认表 / 整理规则）");
    {
        const npcc::core::ToolbarItems& defaults = npcc::core::DefaultToolbarItems();
        Check(!defaults.empty(), "默认表不是空的");
        Check(defaults.front() != npcc::core::kToolbarSeparator &&
                  defaults.back() != npcc::core::kToolbarSeparator,
              "默认表首尾都不是分隔符");
        int buttons = 0;
        bool clean = true;
        for (std::size_t i = 0; i < defaults.size(); ++i) {
            if (defaults[i] == npcc::core::kToolbarSeparator) {
                if (i == 0 || defaults[i - 1] == npcc::core::kToolbarSeparator) {
                    clean = false;
                }
            } else {
                ++buttons;
            }
        }
        Check(clean, "默认表里没有连排的分隔符");
        Check(buttons == 14, "默认表里有 14 个可点按钮");
        bool unique = true;
        for (std::size_t i = 0; i < defaults.size(); ++i) {
            for (std::size_t j = i + 1; j < defaults.size(); ++j) {
                if (defaults[i] != npcc::core::kToolbarSeparator && defaults[i] == defaults[j]) {
                    unique = false;
                }
            }
        }
        Check(unique, "默认表里没有重复按钮");

        const auto known = [](const std::wstring& key) {
            return key == L"a" || key == L"b";
        };
        // 认不出的键丢掉（插件没装、宏被删都会走到这儿）
        Check(npcc::core::NormalizeToolbarItems({L"a", L"ghost", L"b"}, known) ==
                  npcc::core::ToolbarItems({L"a", L"b"}),
              "认不出的键被丢掉，其余按原顺序保留");
        // 同一个按钮放两次没有意义，只留第一次
        Check(npcc::core::NormalizeToolbarItems({L"b", L"a", L"b"}, known) ==
                  npcc::core::ToolbarItems({L"b", L"a"}),
              "重复的按钮只保留第一次");
        // 分隔符：站头的、连排的、站尾的都吃掉
        Check(npcc::core::NormalizeToolbarItems(
                  {npcc::core::kToolbarSeparator, L"a", npcc::core::kToolbarSeparator,
                   npcc::core::kToolbarSeparator, L"b", npcc::core::kToolbarSeparator},
                  known) == npcc::core::ToolbarItems({L"a", npcc::core::kToolbarSeparator, L"b"}),
              "站头 / 连排 / 站尾的分隔符都被吃掉");
        // 空表是合法结果：工具栏上一个按钮都不放
        Check(npcc::core::NormalizeToolbarItems({}, known).empty(), "空表整理后仍然是空表");
        Check(npcc::core::NormalizeToolbarItems({L"ghost"}, known).empty(),
              "全是认不出的键时结果是空表");
        Check(npcc::core::NormalizeToolbarItems({L""}, known).empty(), "空字符串键被丢掉");
        // 没有给的 known 时一个键都不敢放（怕摆上去是个死按钮），只剩下分隔符也会被清掉
        Check(npcc::core::NormalizeToolbarItems({L"a", L"b"}, nullptr).empty(),
              "没有 known 时不放任何命令键");
    }

    // ---------------------------------------------------------------- 多文档管理
    // 这一段会创建真实的 Scintilla 控件，但宿主窗口不显示
    Section("DocumentManager（多标签）");
    if (!npcc::edit::RegisterEditorClasses(::GetModuleHandleW(nullptr))) {
        Check(false, "注册 Scintilla 窗口类");
    } else if (HWND host = CreateHiddenHost(); host == nullptr) {
        Check(false, "创建隐藏宿主窗口");
    } else {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "notepadcc_doc_test";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path fileA = dir / "a.txt";
        const std::filesystem::path fileB = dir / "b.txt";
        npcc::io::WriteFileAtomic(fileA.wstring(), "文件 A 的内容\r\n");
        npcc::io::WriteFileAtomic(fileB.wstring(),
                                  std::string("\xEF\xBB\xBF") + "文件 B 的内容\r\n");

        {
            npcc::core::DocumentManager manager(host);

            npcc::edit::Document* first = manager.CreateUntitled();
            Check(first != nullptr, "新建文档成功");
            Check(manager.Count() == 1, "标签数为 1");
            Check(manager.Active() == first, "新建后成为活动文档");
            Check(first->buffer.isUntitled, "新建文档标记为未保存");
            Check(!first->buffer.displayName.empty(), "新建文档有默认标题");

            npcc::edit::Document* second = manager.CreateUntitled();
            Check(manager.Count() == 2, "第二个标签加入");
            Check(manager.TabOrder().size() == 2, "标签顺序表同步");
            Check(manager.TabOrder()[0] == first->buffer.id &&
                      manager.TabOrder()[1] == second->buffer.id,
                  "标签顺序与创建顺序一致");

            // 打开文件
            const auto openA = manager.Open(fileA.wstring());
            Check(openA.status == npcc::core::OpenStatus::Ok, "打开文件成功");
            Check(openA.document != nullptr && !openA.document->buffer.isUntitled,
                  "打开后不再是未保存文档");
            Check(openA.document->buffer.displayName == L"a.txt", "标题取自文件名");
            Check(openA.document->buffer.encoding.uniMode == npcc::edit::UniMode::Utf8,
                  "ANSI 范围内的内容判为 UTF-8");
            Check(manager.Count() == 3, "打开后标签数为 3");

            const auto openB = manager.Open(fileB.wstring());
            Check(openB.status == npcc::core::OpenStatus::Ok, "打开第二个文件成功");
            Check(openB.document->buffer.encoding.uniMode == npcc::edit::UniMode::Utf8Bom,
                  "识别出 UTF-8 BOM");

            // 重复打开不应产生新标签
            const auto again = manager.Open(fileA.wstring());
            Check(again.status == npcc::core::OpenStatus::AlreadyOpen, "重复打开被识别");
            Check(manager.Count() == 4, "重复打开不新增标签");
            Check(manager.Active() == openA.document, "重复打开会激活已有标签");

            // 同一文件的不同路径写法也必须识别为同一个文档
            const std::wstring alias = (dir / "sub" / ".." / "a.txt").wstring();
            const auto viaAlias = manager.Open(alias);
            Check(viaAlias.status == npcc::core::OpenStatus::AlreadyOpen,
                  "带 .. 的等价路径被视为同一文件");
            Check(manager.Count() == 4, "等价路径不新增标签");
            Check(openA.document->buffer.filePath.find(L"sub") == std::wstring::npos,
                  "存储的是规范化后的路径");

            // 打开不存在的文件
            const auto missing = manager.Open((dir / "no_such_file.txt").wstring());
            Check(missing.status == npcc::core::OpenStatus::NotFound, "不存在的文件返回 NotFound");
            Check(manager.Count() == 4, "失败不产生标签");

            // 保存：内容改动后写回，修改标记复位
            npcc::edit::Document* target = openA.document;
            target->view->SetText("改写后的内容\r\n");
            Check(target->view->IsModified(), "改写后 Scintilla 修改标记置位");
            const auto saved = manager.SaveAs(*target, (dir / "a2.txt").wstring());
            Check(saved.ok, "另存为成功");
            Check(!target->view->IsModified(), "保存后修改标记复位");
            Check(target->buffer.displayName == L"a2.txt", "另存为后标题更新");
            const auto reread = npcc::io::ReadWholeFile((dir / "a2.txt").wstring());
            CheckEqual(reread.bytes, "改写后的内容\r\n", "磁盘内容与编辑器一致");
            Check(std::filesystem::exists(fileA), "原文件仍然存在（另存为不删除源文件）");

            // 标签重排
            const npcc::core::BufferId a2Id = target->buffer.id;
            const npcc::core::BufferId firstId = first->buffer.id;
            manager.MoveBefore(a2Id, firstId);
            Check(manager.TabOrder().front() == a2Id, "MoveBefore 把标签移到目标之前");
            manager.MoveBefore(a2Id, npcc::core::kInvalidId);
            Check(manager.TabOrder().back() == a2Id, "target 为 kInvalidId 时移到末尾");
            Check(npcc::core::kInvalidId != 0, "无效 ID 不是 0（0 会与首个缓冲区 ID 冲突）");

            // 关闭活动标签后应自动激活相邻标签
            manager.Activate(a2Id);
            const size_t before = manager.Count();
            Check(manager.Close(target), "关闭文档成功");
            Check(manager.Count() == before - 1, "标签数减少");
            Check(manager.Active() != nullptr, "关闭后仍有活动文档");
            Check(manager.Find(a2Id) == nullptr, "已关闭的文档不可再查到");

            // 关闭到为空
            for (npcc::edit::Document* document : manager.All()) {
                manager.Close(document);
            }
            Check(manager.Count() == 0, "可以关闭到空");
            Check(manager.Active() == nullptr, "空状态下没有活动文档");
        }

        std::filesystem::remove_all(dir, ec);
        ::DestroyWindow(host);
    }

    // ---------------------------------------------------------------- 列编辑序列
    Section("列块编辑的序列生成");
    {
        Check(npcc::edit::FormatSequenceValue(7, 0, 10) == L"7", "十进制不带补零");
        Check(npcc::edit::FormatSequenceValue(7, 4, 10) == L"0007", "十进制补零");
        Check(npcc::edit::FormatSequenceValue(-7, 4, 10) == L"-0007", "负数补零时负号在前");
        Check(npcc::edit::FormatSequenceValue(12345, 4, 10) == L"12345", "超出补零宽度时不截断");
        Check(npcc::edit::FormatSequenceValue(255, 0, 16) == L"FF", "十六进制大写");
        Check(npcc::edit::FormatSequenceValue(255, 4, 16) == L"00FF", "十六进制补零");
        Check(npcc::edit::FormatSequenceValue(0, 2, 16) == L"00", "零值补零");

        npcc::edit::ColumnInsertOptions options;
        options.templateText = L"item_\\i\\";
        options.start = 1;
        options.step = 1;
        options.zeroPad = 3;
        options.lineCount = 3;
        {
            const auto texts = npcc::edit::BuildColumnInsertTexts(options);
            Check(texts.size() == 3, "生成 3 行文本");
            Check(texts.size() == 3 && texts[0] == L"item_001" && texts[1] == L"item_002" &&
                      texts[2] == L"item_003",
                  "序号从 1 起、按 3 位补零");
        }

        options.resetEvery = 2;
        options.lineCount = 4;
        {
            const auto texts = npcc::edit::BuildColumnInsertTexts(options);
            Check(texts.size() == 4 && texts[0] == L"item_001" && texts[1] == L"item_002" &&
                      texts[2] == L"item_001" && texts[3] == L"item_002",
                  "每 2 行重置序号");
        }

        options.resetEvery = 0;
        options.start = 100;
        options.step = 5;
        options.zeroPad = 0;
        options.lineCount = 3;
        {
            const auto texts = npcc::edit::BuildColumnInsertTexts(options);
            Check(texts.size() == 3 && texts[0] == L"item_100" && texts[1] == L"item_105" &&
                      texts[2] == L"item_110",
                  "自定义起始值与步长");
        }

        options.templateText = L"0x\\i\\";
        options.start = 10;
        options.step = 1;
        options.base = 16;
        options.zeroPad = 2;
        options.lineCount = 2;
        {
            const auto texts = npcc::edit::BuildColumnInsertTexts(options);
            Check(texts.size() == 2 && texts[0] == L"0x0A" && texts[1] == L"0x0B",
                  "十六进制序列");
        }

        options.templateText = L"固定文本";
        options.lineCount = 2;
        {
            const auto texts = npcc::edit::BuildColumnInsertTexts(options);
            Check(texts.size() == 2 && texts[0] == L"固定文本" && texts[1] == L"固定文本",
                  "模板没有占位符时按字面量插入每一行");
        }

        options.templateText = L"\\i\\_\\i\\";
        options.base = 10;
        options.zeroPad = 0;
        options.start = 1;
        options.step = 1;
        options.lineCount = 2;
        {
            const auto texts = npcc::edit::BuildColumnInsertTexts(options);
            Check(texts.size() == 2 && texts[0] == L"1_1" && texts[1] == L"2_2",
                  "模板里的多个占位符都被替换");
        }

        options.lineCount = 0;
        Check(npcc::edit::BuildColumnInsertTexts(options).empty(), "零行时返回空");
    }

    // ---------------------------------------------------------------- 列编辑
    Section("列编辑（矩形选区上的批量操作）");
    {
        HWND host = CreateHiddenHost();
        Check(host != nullptr, "创建隐藏宿主窗口");
        if (host != nullptr) {
            auto view = std::make_unique<npcc::edit::EditView>();
            Check(view->Create(host, 701), "创建编辑视图");
            view->ConfigureColumnEditing();
            view->ApplyLightTheme();

            // 三行等长：aaa / bbb / ccc，字节偏移 0-2 / 4-6 / 8-10
            auto reset = [&]() {
                view->SetText("aaa\nbbb\nccc");
                view->ClearUndoBuffer();
                view->MarkSavePoint();
                // 第 0 行第 1 列 → 第 2 行第 3 列（矩形选区左闭右开，宽度 = 2 列）
                view->SetRectangularSelection(1, 11);
                return view->GetAllText();
            };
            reset();

            Check(view->IsRectangularSelection(), "选区处于矩形模式");
            Check(view->Send(2570) == 3, "矩形选区被展开成 3 条选区");   // SCI_GETSELECTIONS
            int32_t firstLine = -1;
            int32_t lastLine = -1;
            view->ColumnRange(firstLine, lastLine);
            Check(firstLine == 0 && lastLine == 2, "矩形选区覆盖第 0~2 行");
            Check(view->LineSelectionStart(0) == 1, "第 0 行选区起点在第 1 列");
            Check(view->LineSelectionStart(1) == 5, "第 1 行选区起点在第 1 列");
            Check(view->LineSelectionStart(2) == 9, "第 2 行选区起点在第 1 列");
            Check(view->LineSelectionEnd(0) == 3, "第 0 行选区终点在第 3 列");

            // 说明：在矩形选区上打字会逐行插入，这条依赖 Scintilla 的
            // SetRectangularRange，而它需要渲染表面——隐藏窗口里展开不出来。
            // 因此该行为放到界面回归脚本里用真实窗口验证。

            // 批量插入（数字序列）
            reset();
            const int applied = npcc::edit::ApplyColumnInsert(
                *view, {L"1", L"2", L"3"}, true, 936);
            Check(applied == 3, "三行都插入了");
            CheckEqual(view->GetAllText(), "a1aa\nb2bb\nc3cc", "列插入结果正确");

            // 一次撤销应回退整个列插入
            view->Send(2176);   // SCI_UNDO
            CheckEqual(view->GetAllText(), "aaa\nbbb\nccc", "列插入是单个撤销单元");

            // 删除列选区
            reset();
            const int removed = npcc::edit::DeleteColumnSelection(*view);
            Check(removed == 3, "三行的列选区都被删除");
            CheckEqual(view->GetAllText(), "a\nb\nc", "删除列块结果正确（每行保留首字符）");

            // 覆盖模式
            Check(!view->OverwriteMode(), "默认是插入模式");
            view->SetOverwriteMode(true);
            Check(view->OverwriteMode(), "可以切到覆盖模式");
            view->SetOverwriteMode(false);
            Check(!view->OverwriteMode(), "可以切回插入模式");

            // 参差不齐的行：虚拟空间让选区能越过短行的行尾
            view->SetText("aaa\nb\nccc");
            view->ClearUndoBuffer();
            view->SetRectangularSelection(1, 9);
            Check(view->IsRectangularSelection(), "参差行上仍是矩形选区");
            int32_t shortFirst = -1;
            int32_t shortLast = -1;
            view->ColumnRange(shortFirst, shortLast);
            Check(shortFirst == 0 && shortLast == 2, "参差行的行范围仍正确");
            const int32_t shortStart = view->LineSelectionStart(1);
            Check(shortStart >= 0, "短行也能取到列选区起点（虚拟空间生效）");

            view->Destroy();
            ::DestroyWindow(host);
        }
    }

    // ---------------------------------------------------------------- 书签
    Section("书签（标记 / 跳转 / 清除 / 随会话保存）");
    {
        HWND host = CreateHiddenHost();
        Check(host != nullptr, "创建隐藏宿主窗口");
        if (host != nullptr) {
            auto view = std::make_unique<npcc::edit::EditView>();
            Check(view->Create(host, 601), "创建编辑视图");

            std::string text;
            for (int i = 0; i < 10; ++i) {
                if (i > 0) {
                    text += '\n';   // 行间换行；结尾不加，否则 Scintilla 会多出一个空行
                }
                text += "line" + std::to_string(i);
            }
            view->SetText(text);
            view->ClearUndoBuffer();
            view->MarkSavePoint();
            view->ConfigureBookmarkMargin();
            Check(view->LineCount() == 10, "文档共 10 行");

            Check(view->AllBookmarkLines().empty(), "初始没有书签");
            Check(!view->HasBookmark(2), "第 2 行本来没有书签");

            view->AddBookmark(2);
            view->AddBookmark(5);
            view->AddBookmark(9);
            Check(view->HasBookmark(2) && view->HasBookmark(5) && view->HasBookmark(9),
                  "三个书签都已加上");
            Check(!view->HasBookmark(3), "未标记的行没有书签");
            {
                const std::vector<int32_t> lines = view->AllBookmarkLines();
                Check(lines.size() == 3 && lines[0] == 2 && lines[1] == 5 && lines[2] == 9,
                      "书签行号按升序返回");
            }
            view->AddBookmark(2);
            Check(view->AllBookmarkLines().size() == 3, "重复添加不会产生两个书签");

            // 向后跳转
            Check(view->NextBookmarkLine(0, false) == 2, "从第 0 行找到第 2 行");
            Check(view->NextBookmarkLine(3, false) == 5, "从第 3 行找到第 5 行");
            Check(view->NextBookmarkLine(6, false) == 9, "从第 6 行找到第 9 行");
            Check(view->NextBookmarkLine(10, false) == -1, "关闭循环时末尾之后没有书签");
            Check(view->NextBookmarkLine(10, true) == 2, "开启循环时回到第一个书签");

            // 向前跳转
            Check(view->PrevBookmarkLine(8, false) == 5, "从第 8 行向前找到第 5 行");
            Check(view->PrevBookmarkLine(4, false) == 2, "从第 4 行向前找到第 2 行");
            Check(view->PrevBookmarkLine(1, false) == -1, "关闭循环时开头之前没有书签");
            Check(view->PrevBookmarkLine(1, true) == 9, "开启循环时回到最后一个书签");

            // 取消与清除
            view->ToggleBookmark(5);
            Check(!view->HasBookmark(5), "切换后第 5 行书签被取消");
            Check(view->AllBookmarkLines().size() == 2, "剩余两个书签");
            view->ToggleBookmark(5);
            Check(view->HasBookmark(5), "再次切换又加回来");
            view->ClearAllBookmarks();
            Check(view->AllBookmarkLines().empty(), "清除全部书签");

            // 随会话保存与恢复
            view->AddBookmark(3);
            view->AddBookmark(7);
            npcc::core::SessionSnapshot snapshot;
            npcc::core::SessionDocument entry;
            entry.filePath = L"D:\\工程\\bookmarks.txt";
            entry.bookmarks = view->AllBookmarkLines();
            snapshot.documents.push_back(entry);

            std::wstring error;
            Check(npcc::core::SessionManager::Save(snapshot, &error), "保存含书签的会话");
            npcc::core::SessionSnapshot reloaded;
            Check(npcc::core::SessionManager::Load(reloaded, &error), "重新加载会话");
            Check(reloaded.documents.size() == 1, "会话里有 1 个文档");
            if (reloaded.documents.size() == 1) {
                const std::vector<int32_t>& marks = reloaded.documents[0].bookmarks;
                Check(marks.size() == 2, "书签数量往返一致");
                Check(marks.size() == 2 && marks[0] == 3 && marks[1] == 7,
                      "书签行号往返一致");
            }
            std::error_code ec;
            std::filesystem::remove(npcc::core::SessionManager::SessionPath(), ec);

            view->Destroy();
            ::DestroyWindow(host);
        }
    }

    // ---------------------------------------------------------------- 配置中心
    Section("ConfigCenter（配置读写与容错）");
    {
        npcc::infra::InitializePaths(true);   // 会话测试里已经调用过，这里幂等
        const std::filesystem::path path = npcc::core::ConfigCenter::Path().wstring();
        std::error_code ec;
        std::filesystem::remove(path, ec);

        // 没有配置文件时用默认值，不算失败
        {
            npcc::core::ConfigCenter config;
            std::wstring error;
            Check(config.Load(&error), "配置文件不存在时加载成功（用默认值）");
            Check(error.empty(), "不存在的配置不算错误");
            Check(config.Edit().tabWidth == 4, "默认制表符宽度是 4");
            Check(!config.Edit().showWhitespace, "默认不显示空白符号");
            Check(config.NewFile().defaultUniMode == npcc::edit::UniMode::Utf8,
                  "默认新建编码是 UTF-8");
            Check(config.LargeFile().tier1MaxBytes == 8ull * 1024 * 1024,
                  "默认大文件阈值是 8 MB");
        }

        // 改值 → 保存 → 重新加载
        {
            npcc::core::ConfigCenter config;
            config.Load(nullptr);
            config.Edit().tabWidth = 2;
            config.Edit().showWhitespace = true;
            config.Edit().wrapLongLines = true;
            config.General().restoreSessionOnStart = false;
            config.NewFile().defaultEol = npcc::edit::EolMode::Lf;
            config.NewFile().defaultCodePage = 950;
            config.Backup().intervalSeconds = 45;
            config.LargeFile().tier1MaxBytes = 4ull * 1024 * 1024;
            config.LargeFile().tier2MaxBytes = 32ull * 1024 * 1024;
            config.LargeFile().refuseAboveBytes = 256ull * 1024 * 1024;
            config.Search().lastFilter = L"*.rs;*.toml";
            std::wstring error;
            Check(config.Save(true, &error), "保存配置成功");
            Check(std::filesystem::exists(path), "配置文件已落盘");

            npcc::core::ConfigCenter reloaded;
            Check(reloaded.Load(&error), "重新加载成功");
            Check(reloaded.Edit().tabWidth == 2, "制表符宽度往返一致");
            Check(reloaded.Edit().showWhitespace, "显示空白符号往返一致");
            Check(reloaded.Edit().wrapLongLines, "自动换行往返一致");
            Check(!reloaded.General().restoreSessionOnStart, "恢复会话开关往返一致");
            Check(reloaded.NewFile().defaultEol == npcc::edit::EolMode::Lf, "默认行尾往返一致");
            Check(reloaded.NewFile().defaultCodePage == 950, "默认代码页往返一致");
            Check(reloaded.Backup().intervalSeconds == 45, "备份间隔往返一致");
            Check(reloaded.LargeFile().tier1MaxBytes == 4ull * 1024 * 1024, "大文件阈值往返一致");
            Check(reloaded.LargeFile().tier2MaxBytes == 32ull * 1024 * 1024,
                  "大文件 T2 阈值往返一致");
            Check(reloaded.Search().lastFilter == L"*.rs;*.toml", "过滤器往返一致");
            Check(reloaded.General().theme == L"light", "主题默认是浅色");
        }

        // 主题名：认 dark；认不出的名字记警告并回浅色
        {
            npcc::core::ConfigCenter config;
            config.Load(nullptr);
            config.General().theme = L"dark";
            std::wstring error;
            Check(config.Save(true, &error), "把主题存成 dark");
            npcc::core::ConfigCenter reloaded;
            Check(reloaded.Load(&error), "重新加载成功");
            Check(reloaded.General().theme == L"dark", "主题往返一致");

            const std::string badTheme =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadccConfig schemaVersion=\"1\">\n"
                "  <general theme=\"neon\"/>\n"
                "</NotepadccConfig>\n";
            npcc::io::WriteFileAtomic(path.wstring(), badTheme);
            npcc::core::ConfigCenter fallback;
            Check(fallback.Load(&error), "认不出的主题名不让加载失败");
            Check(fallback.General().theme == L"light", "认不出的主题名回退浅色");
            bool sawThemeWarning = false;
            for (const std::wstring& warning : fallback.Warnings()) {
                if (warning.find(L"theme") != std::wstring::npos) {
                    sawThemeWarning = true;
                }
            }
            Check(sawThemeWarning, "认不出的主题名留下了警告");
        }

        // 坏配置：非法值回退默认，绝不能因此起不来
        {
            const std::string broken =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadccConfig schemaVersion=\"1\">\n"
                "  <general recentFileCount=\"999999\" restoreSessionOnStart=\"maybe\"/>\n"
                "  <edit tabWidth=\"0\" showWhitespace=\"yes\" zoomLevel=\"abc\"/>\n"
                "  <newFile defaultUniMode=\"Latin1\" defaultEol=\"Whatever\"/>\n"
                "  <largeFile tier1MaxMB=\"64\" tier2MaxMB=\"8\" refuseAboveMB=\"4\"/>\n"
                "</NotepadccConfig>\n";
            npcc::io::WriteFileAtomic(path.wstring(), broken);

            npcc::core::ConfigCenter config;
            std::wstring error;
            Check(config.Load(&error), "含非法值的配置仍能加载成功");
            Check(config.Edit().tabWidth == 4, "非法制表符宽度回退默认值");
            Check(config.Edit().showWhitespace, "合法的布尔值仍然生效");
            Check(config.Edit().zoomLevel == 0, "非法缩放值回退默认值");
            Check(config.General().recentFileCount == 20, "超范围的最近文件数回退默认值");
            Check(config.General().restoreSessionOnStart, "非法布尔值回退默认值");
            Check(config.NewFile().defaultUniMode == npcc::edit::UniMode::Utf8,
                  "未知编码名回退默认值");
            Check(config.NewFile().defaultEol == npcc::edit::EolMode::Crlf,
                  "未知行尾名回退默认值");
            Check(config.LargeFile().tier1MaxBytes == 8ull * 1024 * 1024,
                  "阈值顺序颠倒时整组回退默认值");
            Check(!config.Warnings().empty(), "回退过的配置项会被记录下来");
        }

        // 彻底损坏的 XML：加载失败，但不崩
        {
            npcc::io::WriteFileAtomic(path.wstring(), "<NotepadccConfig><edit tabWidth=");
            npcc::core::ConfigCenter config;
            std::wstring error;
            Check(!config.Load(&error), "损坏的配置文件加载失败");
            Check(!error.empty(), "失败时给出可读原因");
        }

        // 来自更新版本：拒绝，而不是按错误结构猜
        {
            npcc::io::WriteFileAtomic(
                path.wstring(), "<?xml version=\"1.0\"?><NotepadccConfig schemaVersion=\"99\"/>");
            npcc::core::ConfigCenter config;
            std::wstring error;
            Check(!config.Load(&error), "高版本配置被拒绝");
            Check(!error.empty(), "拒绝时给出原因");
        }

        // 合并写盘：一秒内的第二次请求延后，Flush 才落盘
        {
            std::filesystem::remove(path, ec);
            npcc::core::ConfigCenter config;
            config.Load(nullptr);
            config.Edit().tabWidth = 7;
            config.RequestSave();
            Check(!config.IsDirty(), "第一次请求就落盘了");
            config.Edit().tabWidth = 8;
            config.RequestSave();
            Check(config.IsDirty(), "一秒内的第二次请求被延后");
            config.Flush();
            Check(!config.IsDirty(), "Flush 之后落盘完成");

            npcc::core::ConfigCenter reloaded;
            reloaded.Load(nullptr);
            Check(reloaded.Edit().tabWidth == 8, "延后的那次改动没有丢");
        }
        {
            // SaveNow：无视合并窗口立刻落盘。切界面语言这类"马上要生效"的动作走它——
            // 光靠 RequestSave 的话，那次改动会一直挂到下一次保存或退出（强杀就丢）
            std::filesystem::remove(path, ec);
            npcc::core::ConfigCenter config;
            config.Load(nullptr);
            config.General().uiLanguage = L"en-US";
            config.RequestSave();          // 第一次落盘（同时开始计时）
            Check(!config.IsDirty(), "第一次请求落盘了");
            config.General().uiLanguage = L"zh-CN";
            config.RequestSave();
            Check(config.IsDirty(), "窗口内的第二次请求被延后");
            config.SaveNow();
            Check(!config.IsDirty(), "SaveNow 立刻落盘");
            npcc::core::ConfigCenter reloaded;
            reloaded.Load(nullptr);
            Check(reloaded.General().uiLanguage == L"zh-CN",
                  "SaveNow 写下去的是最后那个值");
        }

        // 工具栏：可见性与按钮表。"没配过（用默认表）"与"用户把表删空"必须是
        // 两种状态——不然删空之后下次启动又会悄悄退回默认表
        {
            std::filesystem::remove(path, ec);
            npcc::core::ConfigCenter config;
            config.Load(nullptr);
            Check(config.Toolbar().visible, "工具栏默认可见");
            Check(config.Toolbar().iconSize == 16, "默认图标尺寸是 16 逻辑像素");
            Check(!config.Toolbar().custom, "默认没有自定义按钮表");
            std::wstring error;
            Check(config.Save(true, &error), "默认状态保存成功");
            {
                const npcc::io::ReadResult read = npcc::io::ReadWholeFile(path.wstring());
                Check(read.ok, "读回配置");
                // 默认表不进文件：写进去只会变成"将来改了默认表、这份旧文件还钉着"
                Check(read.bytes.find("<Item") == std::string::npos,
                      "没配过按钮表时配置里没有 <Item>");
                Check(read.bytes.find("visible=") != std::string::npos, "可见性写进文件");
            }

            config.Toolbar().visible = false;
            config.Toolbar().items = {L"cmd.edit.undo", npcc::core::kToolbarSeparator,
                                      L"cmd.edit.redo"};
            config.Toolbar().custom = true;
            Check(config.Save(true, &error), "保存自定义按钮表成功");
            npcc::core::ConfigCenter reloaded;
            Check(reloaded.Load(&error), "重新加载成功");
            Check(!reloaded.Toolbar().visible, "可见性往返一致");
            Check(reloaded.Toolbar().custom, "自定义标记往返一致");
            Check(reloaded.Toolbar().items == npcc::core::ToolbarItems(
                                                    {L"cmd.edit.undo",
                                                     npcc::core::kToolbarSeparator,
                                                     L"cmd.edit.redo"}),
                  "按钮表（含分隔符）往返一致，顺序不变");

            // 用户把按钮删空：文件里写 empty="yes"，再启动时仍然是"配过的空表"
            reloaded.Toolbar().items.clear();
            Check(reloaded.Save(true, &error), "保存空按钮表成功");
            npcc::core::ConfigCenter emptied;
            Check(emptied.Load(&error), "重新加载成功");
            Check(emptied.Toolbar().custom, "空表仍然算「用户配过」");
            Check(emptied.Toolbar().items.empty(), "空表往返一致（没有退回默认表）");
            {
                const npcc::io::ReadResult read = npcc::io::ReadWholeFile(path.wstring());
                Check(read.ok && read.bytes.find("empty=\"yes\"") != std::string::npos,
                      "空表在文件里记成 empty=yes");
            }
        }

        std::filesystem::remove(path, ec);
    }

    // ---------------------------------------------------------------- XML
    Section("Xml 解析与序列化");
    {
        const char* source =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!-- 这是注释 -->\n"
            "<Config schemaVersion=\"2\" name=\"notepadcc\">\n"
            "  <General theme=\"Dark\" width=\"1200\"/>\n"
            "  <Docs>\n"
            "    <Doc path=\"D:\\工作\\a b.txt\" caret=\"12\"/>\n"
            "    <Doc path=\"&lt;escape&gt; &amp; &quot;quoted&quot;\"/>\n"
            "    <Doc path=\"版权 © 中文\">文本内容 &lt;tag&gt;</Doc>\n"
            "  </Docs>\n"
            "</Config>\n";

        npcc::infra::XmlDocument document;
        std::wstring error;
        Check(npcc::infra::XmlDocument::Parse(source, document, &error), "解析合法 XML 成功");

        const npcc::infra::XmlElement* root = document.Root();
        Check(root != nullptr && root->name == L"Config", "根元素名正确");
        Check(root != nullptr && root->AttributeInt(L"schemaVersion", 0) == 2, "整数属性读取");
        Check(root != nullptr && root->AttributeOr(L"name", L"") == L"notepadcc", "字符串属性读取");
        Check(root != nullptr && root->AttributeInt(L"missing", 42) == 42, "缺失属性取默认值");
        Check(root != nullptr && root->Attribute(L"nothing") == nullptr, "缺失属性返回空指针");

        const npcc::infra::XmlElement* general = root->Child(L"General");
        Check(general != nullptr && general->AttributeOr(L"theme", L"") == L"Dark",
              "子元素属性读取");
        Check(general != nullptr && general->children.empty(), "自闭合元素没有子节点");

        const npcc::infra::XmlElement* docs = root->Child(L"Docs");
        const auto entries = docs != nullptr ? docs->Children(L"Doc")
                                             : std::vector<const npcc::infra::XmlElement*>{};
        Check(entries.size() == 3, "重复子元素全部取出");
        Check(entries.size() > 0 &&
                  entries[0]->AttributeOr(L"path", L"") == L"D:\\工作\\a b.txt",
              "反斜杠与空格在属性中保持不变");
        Check(entries.size() > 1 &&
                  entries[1]->AttributeOr(L"path", L"") == L"<escape> & \"quoted\"",
              "实体解码正确");
        Check(entries.size() > 2 && entries[2]->text == L"文本内容 <tag>", "文本节点实体解码");
        Check(entries.size() > 2 && entries[2]->AttributeOr(L"path", L"") == L"版权 © 中文",
              "非 ASCII 属性值正确");
        Check(docs != nullptr && docs->Child(L"NoSuch") == nullptr, "查找不存在的子元素返回空");

        // 往返：序列化后再解析，结构应保持一致
        const std::string serialized = document.ToUtf8();
        Check(serialized.find("\r\n") != std::string::npos, "序列化带行尾");
        npcc::infra::XmlDocument again;
        Check(npcc::infra::XmlDocument::Parse(serialized, again, &error), "重新解析序列化结果成功");
        const npcc::infra::XmlElement* rootAgain = again.Root();
        Check(rootAgain != nullptr && rootAgain->Child(L"Docs") != nullptr, "往返后子结构完整");
        Check(rootAgain != nullptr && rootAgain->Child(L"Docs")->Children(L"Doc").size() == 3,
              "往返后重复子元素数量不变");
        Check(rootAgain != nullptr &&
                  rootAgain->Child(L"Docs")->Children(L"Doc")[1]->AttributeOr(L"path", L"") ==
                      L"<escape> & \"quoted\"",
              "往返后转义字符保持一致");

        // 边界与错误
        const std::string withCdata =
            "<Root><A><![CDATA[原样保留 <tag> & 符号]]></A></Root>";
        npcc::infra::XmlDocument cdata;
        Check(npcc::infra::XmlDocument::Parse(withCdata, cdata, &error), "CDATA 解析成功");
        Check(cdata.Root() != nullptr && cdata.Root()->Child(L"A") != nullptr &&
                  cdata.Root()->Child(L"A")->text == L"原样保留 <tag> & 符号",
              "CDATA 内容不被解码");

        const char* broken[] = {
            "<Root><A></Root>",              // 结束标签不匹配
            "<Root><A></A>",                 // 根元素未闭合
            "<Root attr=value></Root>",      // 属性值缺少引号
            "<Root/><Extra/>",               // 根元素之后有多余内容
            "   ",                           // 空内容
            "<Root>&nosuch;</Root>",         // 未知实体的处理见下（应被当作字面量，不算错误）
        };
        for (size_t i = 0; i < 5; ++i) {
            npcc::infra::XmlDocument bad;
            error.clear();
            const bool ok = npcc::infra::XmlDocument::Parse(broken[i], bad, &error);
            Check(!ok, "非法 XML 应当解析失败");
            Check(!error.empty(), "失败时给出错误说明");
        }
        {
            npcc::infra::XmlDocument unknown;
            Check(npcc::infra::XmlDocument::Parse(broken[5], unknown, &error),
                  "未知实体按字面量处理，不视为错误");
        }

        // 文件往返（含原子写盘）
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "notepadcc_xml_test";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / "round.xml";
        Check(npcc::io::SaveXmlFile(file.wstring(), document, &error), "写入 XML 文件成功");
        npcc::infra::XmlDocument loaded;
        Check(npcc::io::LoadXmlFile(file.wstring(), loaded, &error),
              "从文件读回 XML 成功");
        Check(loaded.Root() != nullptr &&
                  loaded.Root()->Child(L"Docs")->Children(L"Doc").size() == 3,
              "文件往返后内容完整");
        std::filesystem::remove_all(dir, ec);
    }

    // ---------------------------------------------------------------- 会话
    Section("SessionManager");
    {
        // 会话路径来自配置根目录，先按便携模式初始化，避免写到用户的 %APPDATA%
        npcc::infra::InitializePaths(true);
        const std::wstring path = npcc::core::SessionManager::SessionPath();
        Check(!path.empty(), "会话路径非空");
        Check(path.find(L"session.xml") != std::wstring::npos, "会话文件名正确");

        std::error_code ec;
        std::filesystem::remove(path, ec);   // 清掉可能存在的旧会话

        npcc::core::SessionSnapshot empty;
        std::wstring error;
        Check(!npcc::core::SessionManager::Load(empty, &error), "会话文件不存在时返回失败");
        Check(!error.empty(), "失败时给出原因");

        npcc::core::SessionSnapshot snapshot;
        snapshot.split = true;
        snapshot.splitRatioPercent = 37;
        snapshot.splitSyncScroll = false;
        snapshot.splitActiveSlot = 1;
        snapshot.documents.push_back(npcc::core::SessionDocument{
            L"D:\\工程\\main.cpp", 128, 30, 2, false, {3, 9}, {14, 40}});
        snapshot.documents.push_back(npcc::core::SessionDocument{
            L"D:\\工程\\含中文 与空格.txt", 7, 0, 0, true, {}, {}});
        Check(npcc::core::SessionManager::Save(snapshot, &error), "保存会话成功");
        Check(std::filesystem::exists(path), "会话文件已落盘");

        npcc::core::SessionSnapshot reloaded;
        Check(npcc::core::SessionManager::Load(reloaded, &error), "重新加载会话成功");
        Check(reloaded.documents.size() == 2, "文档条目数量一致");
        if (reloaded.documents.size() == 2) {
            Check(reloaded.documents[0].filePath == L"D:\\工程\\main.cpp", "路径往返一致");
            Check(reloaded.documents[0].caretPos == 128, "光标位置往返一致");
            Check(reloaded.documents[0].firstVisibleLine == 30, "首可见行往返一致");
            Check(reloaded.documents[0].zoomLevel == 2, "缩放级别往返一致");
            Check(!reloaded.documents[0].isActive, "非活动标记往返一致");
            Check(reloaded.documents[1].isActive, "活动标记往返一致");
            Check(reloaded.documents[1].filePath == L"D:\\工程\\含中文 与空格.txt",
                  "中文与空格路径往返一致");
            Check(reloaded.documents[0].bookmarks.size() == 2 &&
                      reloaded.documents[0].bookmarks[1] == 9,
                  "书签行号往返一致");
            Check(reloaded.documents[0].folds.size() == 2 && reloaded.documents[0].folds[0] == 14 &&
                      reloaded.documents[0].folds[1] == 40,
                  "折叠行号往返一致");
            Check(reloaded.documents[1].folds.empty(), "没有折叠的文档不会多出折叠记录");
            Check(reloaded.split, "分屏状态往返一致");
            Check(reloaded.splitRatioPercent == 37, "分屏比例往返一致");
            Check(!reloaded.splitSyncScroll, "同步滚动开关往返一致");
            Check(reloaded.splitActiveSlot == 1, "分屏焦点槽位往返一致");
        }

        // 来自更新版本的会话文件应当被拒绝，而不是按错误结构猜测
        {
            npcc::infra::XmlDocument future;
            npcc::infra::XmlElementPtr root = future.CreateRoot(L"Session");
            root->SetAttributeInt(L"schemaVersion", 99);
            root->AddChild(L"Doc")->SetAttribute(L"path", L"x");
            Check(npcc::io::SaveXmlFile(path, future, &error), "写入高版本会话文件");
            npcc::core::SessionSnapshot rejected;
            Check(!npcc::core::SessionManager::Load(rejected, &error),
                  "高版本会话文件被拒绝");
        }

        // 空路径条目应被忽略
        {
            npcc::infra::XmlDocument doc;
            npcc::infra::XmlElementPtr root = doc.CreateRoot(L"Session");
            root->SetAttributeInt(L"schemaVersion", 1);
            root->AddChild(L"Doc")->SetAttribute(L"path", L"");
            root->AddChild(L"Doc")->SetAttribute(L"path", L"keep.txt");
            Check(npcc::io::SaveXmlFile(path, doc, &error), "写入含空路径的会话文件");
            npcc::core::SessionSnapshot parsed;
            Check(npcc::core::SessionManager::Load(parsed, &error), "加载含空路径的会话");
            Check(parsed.documents.size() == 1, "空路径条目被忽略");
            Check(parsed.documents.size() == 1 && parsed.documents[0].filePath == L"keep.txt",
                  "保留有效条目");
        }

        std::filesystem::remove(path, ec);
    }

    // ---------------------------------------------------------------- 扩展模式转义
    Section("UnescapeExtended");
    {
        npcc::search::SearchOutcome outcome;
        std::string out;

        Check(npcc::search::UnescapeExtended("a\\nb", out, outcome) && out == "a\nb",
              "\\n 转成换行");
        Check(npcc::search::UnescapeExtended("\\t\\r", out, outcome) && out == "\t\r",
              "\\t \\r");
        Check(npcc::search::UnescapeExtended("\\x41\\x42", out, outcome) && out == "AB",
              "\\xNN 两位十六进制");
        Check(npcc::search::UnescapeExtended("\\x{4e2d}", out, outcome) && out == "中",
              "\\x{NNNN} 转成 UTF-8");
        Check(npcc::search::UnescapeExtended("c:\\\\work", out, outcome) && out == "c:\\work",
              "\\\\ 表示一个反斜杠");
        Check(npcc::search::UnescapeExtended("\\q", out, outcome) && out == "\\q",
              "不认识的转义原样保留");
        Check(npcc::search::UnescapeExtended("tail\\", out, outcome) && out == "tail\\",
              "结尾孤立反斜杠原样保留");
        Check(npcc::search::UnescapeExtended("中文\\n", out, outcome) && out == "中文\n",
              "多字节字符不受影响");

        outcome = npcc::search::SearchOutcome{};
        Check(!npcc::search::UnescapeExtended("\\x{ZZ}", out, outcome),
              "\\x{} 内的非法十六进制应当失败");
        Check(outcome.error == npcc::search::SearchError::BadPattern, "错误类型正确");
        outcome = npcc::search::SearchOutcome{};
        Check(!npcc::search::UnescapeExtended("\\x{110000}", out, outcome),
              "码点超出 Unicode 范围应当失败");
    }

    // ---------------------------------------------------------------- 正则引擎
    Section("RegexEngine（PCRE2）");
    {
        npcc::search::RegexOptions options;
        options.utfMode = true;
        npcc::search::SearchOutcome outcome;

        // 基本匹配
        {
            npcc::search::RegexEngine engine;
            Check(engine.Compile(L"\\d+", options, outcome), "编译 \\d+ 成功");
            Check(engine.JitEnabled(), "JIT 已启用");
            const std::string subject = "abc12345def";
            npcc::search::MatchRange match;
            Check(engine.Find(subject, 0, match, outcome), "找到数字串");
            Check(match.start == 3 && match.end == 8, "匹配区间正确");
            Check(!engine.Find(subject, match.end, match, outcome), "从匹配之后继续搜不到");
            Check(outcome.Ok(), "无匹配不是错误");
        }

        // 大小写
        {
            npcc::search::RegexEngine engine;
            npcc::search::MatchRange match;
            npcc::search::RegexOptions insensitive;
            insensitive.utfMode = true;
            Check(engine.Compile(L"hello", insensitive, outcome), "编译 hello 成功");
            const std::string subject = "say HELLO there";
            Check(engine.Find(subject, 0, match, outcome), "默认不区分大小写");

            npcc::search::RegexOptions sensitive;
            sensitive.utfMode = true;
            sensitive.matchCase = true;
            Check(engine.Compile(L"hello", sensitive, outcome), "编译区分大小写的 hello");
            Check(!engine.Find(subject, 0, match, outcome), "区分大小写时不应命中");
            Check(engine.Find("say hello there", 0, match, outcome), "小写仍能命中");
        }

        // 点号是否跨行
        {
            const std::string subject = "a\nb";
            npcc::search::RegexEngine engine;
            npcc::search::MatchRange match;
            npcc::search::RegexOptions plain;
            plain.utfMode = true;
            Check(engine.Compile(L"a.b", plain, outcome), "编译 a.b");
            Check(!engine.Find(subject, 0, match, outcome), "默认 . 不匹配换行");

            npcc::search::RegexOptions dotall;
            dotall.utfMode = true;
            dotall.dotMatchesNewline = true;
            Check(engine.Compile(L"a.b", dotall, outcome), "编译带 DOTALL 的 a.b");
            Check(engine.Find(subject, 0, match, outcome), "开启后 . 匹配换行");
            Check(match.start == 0 && match.end == 3, "跨行匹配区间正确");
        }

        // Unicode 语义
        {
            npcc::search::RegexEngine engine;
            npcc::search::MatchRange match;
            Check(engine.Compile(L"\\p{Han}+", options, outcome), "编译 \\p{Han}+");
            const std::string subject = "abc 中文内容 def";
            Check(engine.Find(subject, 0, match, outcome), "UTF 模式下命中汉字");
            Check(match.end - match.start == 12, "四个汉字占 12 字节");

            npcc::search::RegexOptions byteMode;   // utfMode = false，按字节处理
            byteMode.codePage = 936;
            npcc::search::RegexEngine byteEngine;
            Check(byteEngine.Compile(L"\\p{Han}+", byteMode, outcome),
                  "非 UTF 模式下也能编译（只是语义退化为字节）");
            Check(!byteEngine.Find(subject, 0, match, outcome), "字节模式下 \\p{Han} 不命中");
        }

        // 全字匹配
        {
            npcc::search::RegexOptions word;
            word.utfMode = true;
            word.wholeWordOnly = true;
            npcc::search::RegexEngine engine;
            npcc::search::MatchRange match;
            Check(engine.Compile(L"cat", word, outcome), "编译全字模式 cat");
            Check(!engine.Find("concat", 0, match, outcome), "不应命中 concat 内部");
            Check(engine.Find("a cat here", 0, match, outcome), "应命中独立的 cat");
        }

        // 捕获组替换
        {
            npcc::search::RegexEngine engine;
            Check(engine.Compile(L"(\\w+)@(\\w+)", options, outcome), "编译邮箱模式");
            const std::string subject = "user@host";
            npcc::search::MatchRange match;
            Check(engine.Find(subject, 0, match, outcome), "命中 user@host");
            std::string replaced;
            Check(engine.ExpandReplacement(subject, L"$2@$1", replaced, outcome),
                  "展开替换串成功");
            CheckEqual(replaced, "host@user", "捕获组引用正确");
        }

        // 灾难性回溯保护
        {
            npcc::search::RegexEngine engine;
            Check(engine.Compile(L"(a+)+$", options, outcome), "编译易回溯的模式");
            const std::string subject(40, 'a');
            npcc::search::MatchRange match;
            const bool found = engine.Find(subject + "b", 0, match, outcome);
            Check(!found, "极端回溯不产生匹配");
            Check(outcome.error == npcc::search::SearchError::MatchLimitExceeded,
                  "回溯超限被识别并中止，而不是一直跑下去");
            Check(!outcome.message.empty(), "给出可读的提示");
        }

        // 编译期错误
        {
            npcc::search::RegexEngine engine;
            npcc::search::SearchOutcome bad;
            Check(!engine.Compile(L"(unclosed", options, bad), "括号不闭合应当编译失败");
            Check(bad.error == npcc::search::SearchError::BadPattern, "错误类型为模式错误");
            Check(!bad.message.empty(), "给出可读的错误说明");
            Check(!engine.Ready(), "失败后引擎不可用");

            npcc::search::SearchOutcome empty;
            npcc::search::RegexEngine engine2;
            Check(!engine2.Compile(L"", options, empty), "空模式应当被拒绝");
            Check(empty.error == npcc::search::SearchError::EmptyPattern, "错误类型为空模式");
        }

        // 空匹配不应让调用方陷入死循环（引擎本身如实返回 0 长度匹配）
        {
            npcc::search::RegexEngine engine;
            Check(engine.Compile(L"x*", options, outcome), "编译可空匹配的模式");
            npcc::search::MatchRange match;
            Check(engine.Find("abc", 0, match, outcome), "在 abc 上产生空匹配");
            Check(match.start == 0 && match.end == 0, "空匹配区间为 0 长度");
        }
    }

    // ---------------------------------------------------------------- 单文档查找替换
    Section("InDocSearch（单文档查找与替换）");
    {
        HWND host = CreateHiddenHost();
        Check(host != nullptr, "创建隐藏宿主窗口");
        if (host != nullptr) {
            auto view = std::make_unique<npcc::edit::EditView>();
            Check(view->Create(host, 501), "创建编辑视图");

            // 字节偏移对照：one two one\ntwo one two
            const std::string text = "one two one\ntwo one two";
            auto reset = [&]() {
                view->SetText(text);
                view->ClearUndoBuffer();
                view->MarkSavePoint();
                view->SetSelectionRange(0, 0);
            };
            reset();

            npcc::search::InDocSearch search(*view);
            npcc::search::SearchConfig config;
            npcc::search::SearchOutcome outcome;
            config.utfMode = true;

            // 计数
            int count = 0;
            config.pattern = L"one";
            Check(search.Count(config, count, outcome), "统计 one");
            Check(count == 3, "one 出现 3 次");

            config.pattern = L"two";
            Check(search.Count(config, count, outcome) && count == 3, "two 出现 3 次");

            config.pattern = L"missing";
            Check(search.Count(config, count, outcome) && count == 0, "不存在的词计数为 0");

            // 大小写
            config.pattern = L"ONE";
            Check(search.Count(config, count, outcome) && count == 3, "默认不区分大小写");
            config.matchCase = true;
            Check(search.Count(config, count, outcome) && count == 0, "区分大小写时 ONE 无匹配");
            config.matchCase = false;

            // 全字匹配
            config.pattern = L"on";
            Check(search.Count(config, count, outcome) && count == 3, "on 作为子串出现 3 次");
            config.wholeWordOnly = true;
            Check(search.Count(config, count, outcome) && count == 0, "全字匹配下 on 无匹配");
            config.wholeWordOnly = false;

            // 前向查找与选中
            config.pattern = L"one";
            bool wrapped = false;
            view->SetSelectionRange(0, 0);
            Check(search.FindNext(config, true, wrapped, outcome), "从文档头找到第一个 one");
            Check(view->SelectionStart() == 0 && view->SelectionEnd() == 3,
                  "第一个匹配的区间正确");
            Check(!wrapped, "未发生回绕");

            Check(search.FindNext(config, true, wrapped, outcome), "找到第二个 one");
            Check(view->SelectionStart() == 8 && view->SelectionEnd() == 11,
                  "第二个匹配的区间正确");

            Check(search.FindNext(config, true, wrapped, outcome), "找到第三个 one");
            Check(view->SelectionStart() == 16 && view->SelectionEnd() == 19,
                  "第三个匹配的区间正确");

            Check(search.FindNext(config, true, wrapped, outcome), "回绕到第一个 one");
            Check(view->SelectionStart() == 0, "回绕后位置正确");
            Check(wrapped, "回绕标志置位");

            // 不循环时应当搜不到
            view->SetSelectionRange(16, 19);
            config.wrapAround = false;
            Check(!search.FindNext(config, true, wrapped, outcome), "关闭循环后不再回绕");
            Check(outcome.Ok(), "搜不到不是错误");
            config.wrapAround = true;

            // 后向查找
            view->SetSelectionRange(19, 19);
            Check(search.FindNext(config, false, wrapped, outcome), "向前查找成功");
            Check(view->SelectionStart() == 16, "向前查找到前一个匹配");
            Check(search.FindNext(config, false, wrapped, outcome), "继续向前");
            Check(view->SelectionStart() == 8, "再前一个匹配");

            // 扩展模式：\n 应当参与匹配
            npcc::search::SearchConfig escaped;
            escaped.utfMode = true;
            escaped.mode = npcc::search::SearchMode::Escaped;
            escaped.pattern = L"one\\ntwo";
            Check(search.Count(escaped, count, outcome), "扩展模式下统计跨行模式");
            Check(count == 1, "one\\ntwo 跨行匹配 1 次");

            // 正则模式
            npcc::search::SearchConfig regex;
            regex.utfMode = true;
            regex.mode = npcc::search::SearchMode::Regex;
            regex.pattern = L"o.e";
            Check(search.Count(regex, count, outcome), "正则统计 o.e");
            Check(count == 3, "o.e 匹配 3 次");

            regex.pattern = L"(unclosed";
            npcc::search::SearchOutcome bad = npcc::search::SearchOutcome{};
            Check(!search.Count(regex, count, bad), "非法正则让统计失败");
            Check(bad.error == npcc::search::SearchError::BadPattern, "错误类型正确");

            // 标记全部（只验证不崩溃并返回数量；indicator 是渲染层的事）
            regex.pattern = L"o.e";
            Check(search.MarkAll(regex, count, outcome) && count == 3, "标记全部匹配");
            search.ClearMarks();

            // 替换当前：选区不是匹配项时绝对不许改内容
            npcc::search::SearchConfig replaceConfig;
            replaceConfig.utfMode = true;
            replaceConfig.pattern = L"one";
            replaceConfig.replaceTo = L"1";
            reset();
            view->SetSelectionRange(4, 7);   // 选中的是 "two"
            bool replaced = true;
            Check(search.ReplaceCurrent(replaceConfig, replaced, outcome),
                  "选区不是匹配项时调用替换");
            Check(!replaced, "选区不是匹配项时不得替换");
            CheckEqual(view->GetAllText(), text, "内容必须保持不变");

            // 替换当前：选区正好是匹配项时替换，并自动选中下一个
            reset();
            view->SetSelectionRange(0, 3);
            Check(search.ReplaceCurrent(replaceConfig, replaced, outcome), "替换当前匹配");
            Check(replaced, "确实发生了替换");
            Check(view->GetAllText() == "1 two one\ntwo one two", "第一条被替换");
            Check(view->SelectionStart() == 6 && view->SelectionEnd() == 9,
                  "替换后自动选中下一个匹配");

            // 全部替换（含正则捕获组）
            reset();
            npcc::search::SearchConfig swapConfig;
            swapConfig.utfMode = true;
            swapConfig.mode = npcc::search::SearchMode::Regex;
            swapConfig.pattern = L"(o)(n)(e)";
            swapConfig.replaceTo = L"$3$2$1";
            int replacedCount = 0;
            Check(search.ReplaceAll(swapConfig, replacedCount, outcome), "正则全部替换成功");
            Check(replacedCount == 3, "替换了 3 处");
            CheckEqual(view->GetAllText(), "eno two eno\ntwo eno two", "捕获组重排结果正确");

            // 全部替换必须是一个撤销单元：一次撤销就应回到替换前
            view->Send(2176);   // SCI_UNDO
            CheckEqual(view->GetAllText(), text, "一次撤销即可回到替换前的内容");

            // 字面量全部替换
            reset();
            Check(search.ReplaceAll(replaceConfig, replacedCount, outcome), "字面量全部替换");
            Check(replacedCount == 3, "字面量替换 3 处");
            CheckEqual(view->GetAllText(), "1 two 1\ntwo 1 two", "字面量替换结果正确");

            view->Destroy();
            ::DestroyWindow(host);
        }
    }

    // ---------------------------------------------------------------- 通配符与过滤器
    Section("Wildcard / 文件过滤器");
    {
        Check(npcc::search::WildcardMatch(L"*.txt", L"a.txt"), "*.txt 匹配 a.txt");
        Check(!npcc::search::WildcardMatch(L"*.txt", L"a.cpp"), "*.txt 不匹配 a.cpp");
        Check(npcc::search::WildcardMatch(L"a?c", L"abc"), "? 匹配单个字符");
        Check(npcc::search::WildcardMatch(L"*.TXT", L"a.txt"), "文件名匹配默认不区分大小写");
        Check(npcc::search::WildcardMatch(L"*", L"anything"), "* 匹配一切");
        Check(!npcc::search::WildcardMatch(L"a*b*c", L"aXb"), "跨多段的 * 不匹配");
        Check(npcc::search::WildcardMatch(L"a*b*c", L"aXbYc"), "跨多段的 * 匹配");
        Check(npcc::search::WildcardMatch(L"", L""), "空模式匹配空串");
        Check(!npcc::search::WildcardMatch(L"", L"x"), "空模式不匹配非空串");

        const npcc::search::FileFilter filter =
            npcc::search::ParseFileFilter(L"*.js; *.html ;!node_modules;!*.min.js");
        Check(filter.includes.size() == 2, "解析出两个包含模式（含空白裁剪）");
        Check(filter.excludes.size() == 2, "解析出两个排除模式");
        Check(npcc::search::FilterAllowsFile(filter, L"app.js"), "允许 app.js");
        Check(!npcc::search::FilterAllowsFile(filter, L"app.css"), "不在包含列表里的被拒绝");
        Check(!npcc::search::FilterAllowsFile(filter, L"lib.min.js"), "排除 *.min.js");
        Check(!npcc::search::FilterAllowsDirectory(filter, L"node_modules"),
              "排除 node_modules 目录");
        Check(npcc::search::FilterAllowsDirectory(filter, L"src"), "允许 src 目录");

        const npcc::search::FileFilter allowAll = npcc::search::ParseFileFilter(L"*.*");
        Check(npcc::search::FilterAllowsFile(allowAll, L"anything.xyz"),
              "只有 *.* 时允许任意文件");
        const npcc::search::FileFilter empty = npcc::search::ParseFileFilter(L"");
        Check(npcc::search::FilterAllowsFile(empty, L"anything"), "空过滤器不限制");
    }

    // ---------------------------------------------------------------- 在文件中查找
    Section("CrossFileSearch（在文件中查找）");
    {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "notepadcc_cross_test";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "sub", ec);
        std::filesystem::create_directories(root / "node_modules", ec);
        std::filesystem::create_directories(root / "other", ec);

        const auto write = [](const std::filesystem::path& path, std::string_view bytes) {
            npcc::io::WriteFileAtomic(path.wstring(), bytes);
        };
        write(root / "a.txt", "first line\r\nneedle here\r\nthird\r\nneedle again\r\n");
        write(root / "b.cpp", "// needle in cpp\n");
        write(root / "sub" / "c.md", "needle in subdirectory\n");
        write(root / "node_modules" / "skip.txt", "needle must not be found here\n");
        write(root / "other" / "ignored.dat", "needle in dat\n");
        write(root / "binary.bin", std::string("\x00\x01\x02needle\x03", 10));

        npcc::search::SearchConfig config;
        config.pattern = L"needle";
        config.directories = {root.wstring()};
        config.filter = L"*.txt;*.cpp;*.md";
        config.includeSubDirs = true;

        const std::vector<npcc::search::FileSearchResult> results =
            npcc::search::CrossFileSearch::RunSynchronously(config, 2);

        auto findFile = [&results](const std::wstring& name)
            -> const npcc::search::FileSearchResult* {
            for (const auto& result : results) {
                if (result.path.size() >= name.size() &&
                    result.path.compare(result.path.size() - name.size(), name.size(), name) == 0) {
                    return &result;
                }
            }
            return nullptr;
        };

        Check(results.size() == 3, "命中 3 个文件（其余被过滤或跳过）");
        Check(findFile(L"a.txt") != nullptr, "a.txt 命中");
        Check(findFile(L"b.cpp") != nullptr, "b.cpp 命中");
        Check(findFile(L"sub\\c.md") != nullptr || findFile(L"c.md") != nullptr,
              "子目录里的 c.md 命中");
        Check(findFile(L"skip.txt") == nullptr, "node_modules 下的文件被跳过");
        Check(findFile(L"ignored.dat") == nullptr, "不在过滤器内的文件被跳过");
        Check(findFile(L"binary.bin") == nullptr, "二进制文件被跳过");

        if (const auto* a = findFile(L"a.txt"); a != nullptr) {
            Check(a->hits.size() == 2, "a.txt 两行命中");
            if (a->hits.size() == 2) {
                Check(a->hits[0].line == 2, "第一处命中在第 2 行");
                CheckEqual(a->hits[0].text, "needle here", "行文本正确");
                Check(a->hits[1].line == 4, "第二处命中在第 4 行");
                Check(a->hits[0].matches.size() == 1, "第 2 行有 1 处匹配");
                Check(a->hits[0].matches[0].start == 0 && a->hits[0].matches[0].end == 6,
                      "行内偏移正确（相对行首）");
            }
        }

        // 只搜 .txt
        npcc::search::SearchConfig txtOnly = config;
        txtOnly.filter = L"*.txt";
        const auto txtResults = npcc::search::CrossFileSearch::RunSynchronously(txtOnly, 2);
        Check(txtResults.size() == 1, "限制为 *.txt 时只命中 a.txt");

        // 不递归
        npcc::search::SearchConfig noSub = config;
        noSub.includeSubDirs = false;
        const auto noSubResults = npcc::search::CrossFileSearch::RunSynchronously(noSub, 2);
        Check(noSubResults.size() == 2, "不递归时子目录里的 c.md 不再命中");

        // 正则模式跨文件
        npcc::search::SearchConfig regexConfig = config;
        regexConfig.mode = npcc::search::SearchMode::Regex;
        regexConfig.pattern = L"needle\\s+\\w+";
        const auto regexResults =
            npcc::search::CrossFileSearch::RunSynchronously(regexConfig, 2);
        Check(!regexResults.empty(), "正则跨文件搜索有结果");
        if (const auto* a = [&regexResults]() -> const npcc::search::FileSearchResult* {
                for (const auto& result : regexResults) {
                    if (result.path.size() > 6 &&
                        result.path.compare(result.path.size() - 6, 6, L"a.txt") == 0) {
                        return &result;
                    }
                }
                return nullptr;
            }(); a != nullptr && !a->hits.empty()) {
            const auto& match = a->hits[0].matches[0];
            Check(match.end - match.start > 6, "正则匹配覆盖了 needle 之后的词");
        }

        // 取消：命中第一个结果后就取消，应当提前收工
        {
            const std::filesystem::path many = root / "many";
            std::filesystem::create_directories(many, ec);
            for (int i = 0; i < 200; ++i) {
                write(many / (L"f" + std::to_wstring(i) + L".txt"), "needle\n");
            }
            npcc::search::SearchConfig cancelConfig = config;
            cancelConfig.directories = {many.wstring()};
            cancelConfig.filter = L"*.txt";

            npcc::search::CrossFileSearch searcher(2);
            std::shared_ptr<npcc::search::CrossSearchJob> job;
            std::atomic<int> seen{0};
            npcc::search::CrossSearchCallbacks callbacks;
            callbacks.onFileResult = [&](const npcc::search::FileSearchResult&) {
                if (seen.fetch_add(1) == 0 && job) {
                    job->Cancel();
                }
            };
            job = searcher.Start(cancelConfig, std::move(callbacks));
            job->Wait();
            const npcc::search::SearchProgress progress = job->Progress();
            Check(progress.cancelled, "取消标志被记录");
            Check(progress.finished, "取消后任务仍然正常收尾");
            Check(progress.filesScanned < 200, "取消后没有扫完全部文件");
            Check(seen.load() > 0, "取消前已经产生了结果");
        }

        std::filesystem::remove_all(root, ec);
    }

    // ---------------------------------------------------------------- 在文件中替换
    Section("CrossFileReplace（在文件中替换）");
    {
        const auto writeFile = [](const std::filesystem::path& path, std::string_view bytes) {
            npcc::io::WriteFileAtomic(path.wstring(), bytes);
        };
        const auto readFile = [](const std::filesystem::path& path) {
            const auto result = npcc::io::ReadWholeFile(path.wstring());
            return result.ok ? result.bytes : std::string();
        };

        std::error_code ec;
        int treeCounter = 0;
        const auto makeTree = [&](const char* tag) {
            const auto dir = std::filesystem::temp_directory_path() /
                             ("notepadcc_replace_" + std::to_string(++treeCounter) + "_" + tag);
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir / "sub", ec);
            writeFile(dir / "a.txt", "alpha needle beta\nsecond line\nneedle again\n");
            writeFile(dir / "b.txt", "no match here\n");
            writeFile(dir / "c.txt", "needle\n");
            writeFile(dir / "sub" / "d.txt", "needle in sub\n");
            return dir;
        };

        npcc::search::SearchConfig config;
        config.pattern = L"needle";
        config.replaceTo = L"NEEDLE";
        config.filter = L"*.txt";
        npcc::search::ReplaceOptions options;

        // 生成计划
        {
            const auto dir = makeTree("plan");
            config.directories = {dir.wstring()};
            const auto plans = npcc::search::CrossFileReplace::BuildPlanSync(config, options, 2);

            Check(plans.size() == 3, "只有含匹配的 3 个文件进入计划");
            const auto findPlan = [&plans](const std::wstring& name)
                -> const npcc::search::FileReplacePlan* {
                for (const auto& plan : plans) {
                    if (plan.path.size() >= name.size() &&
                        plan.path.compare(plan.path.size() - name.size(), name.size(), name) == 0) {
                        return &plan;
                    }
                }
                return nullptr;
            };
            Check(findPlan(L"b.txt") == nullptr, "没有匹配的文件不进计划");
            if (const auto* plan = findPlan(L"a.txt"); plan != nullptr) {
                Check(plan->replacements == 2, "a.txt 计划替换 2 处");
                Check(plan->selected, "计划默认勾选");
                Check(plan->samples.size() == 2, "两处改动各有一条预览");
                if (plan->samples.size() == 2) {
                    Check(plan->samples[0].line == 1, "第一处改动在第 1 行");
                    CheckEqual(plan->samples[0].before, "alpha needle beta", "预览的替换前文本");
                    CheckEqual(plan->samples[0].after, "alpha NEEDLE beta", "预览的替换后文本");
                }
            }
            std::filesystem::remove_all(dir, ec);
        }

        // 应用
        {
            const auto dir = makeTree("apply");
            config.directories = {dir.wstring()};
            auto plans = npcc::search::CrossFileReplace::BuildPlanSync(config, options, 2);
            const auto report = npcc::search::CrossFileReplace::Apply(config, options, plans);
            Check(report.applied == 3, "3 个文件写回成功");
            Check(report.replacements == 4, "共替换 4 处");
            Check(report.failed == 0 && report.skipped == 0, "没有失败或跳过");
            CheckEqual(readFile(dir / "a.txt"), "alpha NEEDLE beta\nsecond line\nNEEDLE again\n",
                       "a.txt 内容正确替换");
            CheckEqual(readFile(dir / "sub" / "d.txt"), "NEEDLE in sub\n", "子目录文件也被替换");
            CheckEqual(readFile(dir / "b.txt"), "no match here\n", "无匹配文件保持原样");
            std::filesystem::remove_all(dir, ec);
        }

        // 未勾选的文件不动
        {
            const auto dir = makeTree("select");
            config.directories = {dir.wstring()};
            auto plans = npcc::search::CrossFileReplace::BuildPlanSync(config, options, 2);
            for (auto& plan : plans) {
                if (plan.path.size() >= 5 &&
                    plan.path.compare(plan.path.size() - 5, 5, L"a.txt") == 0) {
                    plan.selected = false;
                }
            }
            const auto report = npcc::search::CrossFileReplace::Apply(config, options, plans);
            Check(report.applied == 2, "只写回了勾选的 2 个文件");
            Check(report.skipped == 1, "未勾选的 1 个被跳过");
            CheckEqual(readFile(dir / "a.txt"), "alpha needle beta\nsecond line\nneedle again\n",
                       "未勾选的文件内容不变");
            std::filesystem::remove_all(dir, ec);
        }

        // 生成计划后文件被外部改动：必须跳过，不能照着旧内容覆盖
        {
            const auto dir = makeTree("stale");
            config.directories = {dir.wstring()};
            auto plans = npcc::search::CrossFileReplace::BuildPlanSync(config, options, 2);
            // 模拟别的程序改了 a.txt
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            writeFile(dir / "a.txt", "别人改过的内容 needle\n");
            const auto report = npcc::search::CrossFileReplace::Apply(config, options, plans);
            Check(report.skipped >= 1, "被外部改动的文件被跳过");
            CheckEqual(readFile(dir / "a.txt"), "别人改过的内容 needle\n",
                       "绝不能覆盖别人的改动");
            std::filesystem::remove_all(dir, ec);
        }

        // 编码保持：UTF-8 BOM 与 GBK 都不能被改掉
        {
            const auto dir = std::filesystem::temp_directory_path() / "notepadcc_replace_enc";
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            writeFile(dir / "bom.txt", std::string("\xEF\xBB\xBF") + "needle 中文\r\n");
            std::string gbk;
            gbk += "\xB5\xDA\xD2\xBB\xD0\xD0";   // GBK 的「第一行」
            gbk += " needle\r\n";
            writeFile(dir / "gbk.txt", gbk);

            config.directories = {dir.wstring()};
            const auto plans = npcc::search::CrossFileReplace::BuildPlanSync(config, options, 1);
            const auto report = npcc::search::CrossFileReplace::Apply(config, options, plans);
            Check(report.applied == 2, "两个编码的文件都写回成功");

            const std::string bomAfter = readFile(dir / "bom.txt");
            Check(bomAfter.size() > 3 && static_cast<unsigned char>(bomAfter[0]) == 0xEF &&
                      static_cast<unsigned char>(bomAfter[1]) == 0xBB &&
                      static_cast<unsigned char>(bomAfter[2]) == 0xBF,
                  "UTF-8 BOM 保持不变");
            Check(bomAfter.find("NEEDLE") != std::string::npos, "BOM 文件内容已替换");

            const std::string gbkAfter = readFile(dir / "gbk.txt");
            Check(gbkAfter.size() >= 6 &&
                      static_cast<unsigned char>(gbkAfter[0]) == 0xB5 &&
                      static_cast<unsigned char>(gbkAfter[1]) == 0xDA,
                  "GBK 的中文原字节未被破坏");
            Check(gbkAfter.find("NEEDLE") != std::string::npos, "GBK 文件内容已替换");
            std::filesystem::remove_all(dir, ec);
        }

        // 替换串无法用目标编码表示：跳过该文件并保留原内容
        {
            const auto dir = std::filesystem::temp_directory_path() / "notepadcc_replace_enc2";
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            std::string gbk;
            gbk += "\xB5\xDA\xD2\xBB\xD0\xD0";
            gbk += " needle\r\n";
            writeFile(dir / "gbk.txt", gbk);

            npcc::search::SearchConfig emoji = config;
            emoji.directories = {dir.wstring()};
            emoji.replaceTo = L"😀";   // GBK 无法表示
            const auto plans = npcc::search::CrossFileReplace::BuildPlanSync(emoji, options, 1);
            Check(plans.size() == 1, "无法表示的文件仍会出现在计划里以便提示用户");
            Check(!plans.empty() && !plans[0].error.empty(), "计划里带有错误说明");
            const auto report = npcc::search::CrossFileReplace::Apply(emoji, options, plans);
            Check(report.applied == 0, "该文件没有被写回");
            Check(report.failed == 1, "计为失败");
            const std::string after = readFile(dir / "gbk.txt");
            Check(after.find("needle") != std::string::npos, "原内容保持不动");
            Check(after.find("NEEDLE") == std::string::npos, "没有发生替换");
            std::filesystem::remove_all(dir, ec);
        }

        // 正则捕获组跨文件替换
        {
            const auto dir = std::filesystem::temp_directory_path() / "notepadcc_replace_regex";
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            writeFile(dir / "x.txt", "user@host\nother@place\n");

            npcc::search::SearchConfig regex = config;
            regex.directories = {dir.wstring()};
            regex.mode = npcc::search::SearchMode::Regex;
            regex.pattern = L"(\\w+)@(\\w+)";
            regex.replaceTo = L"$2#$1";
            const auto plans = npcc::search::CrossFileReplace::BuildPlanSync(regex, options, 1);
            Check(plans.size() == 1 && plans[0].replacements == 2, "正则计划命中 2 处");
            const auto report = npcc::search::CrossFileReplace::Apply(regex, options, plans);
            Check(report.applied == 1, "正则替换写回成功");
            CheckEqual(readFile(dir / "x.txt"), "host#user\nplace#other\n", "捕获组已重排");
            std::filesystem::remove_all(dir, ec);
        }
    }

    // ---------------------------------------------------------------- 大文件分档
    Section("大文件分档（T1 常规 / T2 大文件 / T3 只读）");
    {
        npcc::edit::TierPolicy policy;
        Check(policy.Classify(0) == npcc::edit::FileTier::Normal, "空文件属常规档");
        Check(policy.Classify(policy.normalMaxBytes) == npcc::edit::FileTier::Normal,
              "等于常规上限仍在常规档");
        Check(policy.Classify(policy.normalMaxBytes + 1) == npcc::edit::FileTier::Large,
              "刚过常规上限进入大文件档");
        Check(policy.Classify(policy.largeMaxBytes) == npcc::edit::FileTier::Large,
              "等于大文件上限仍在大文件档");
        Check(policy.Classify(policy.largeMaxBytes + 1) == npcc::edit::FileTier::Huge,
              "超过大文件上限进入只读档");
        Check(policy.Acceptable(policy.refuseAboveBytes), "等于拒绝上限仍可打开");
        Check(!policy.Acceptable(policy.refuseAboveBytes + 1), "超过拒绝上限不可打开");

        // 只探测编码、不复制内容
        {
            const auto utf8 = npcc::edit::DetectEncodingFromHead("hello 中文", 936);
            Check(utf8.uniMode == npcc::edit::UniMode::Utf8, "头部探测出 UTF-8");
            const std::string bomHead = std::string("\xEF\xBB\xBF") + "x";
            const auto bom = npcc::edit::DetectEncodingFromHead(bomHead, 936);
            Check(bom.uniMode == npcc::edit::UniMode::Utf8Bom && bom.hasBom,
                  "头部探测出 UTF-8 BOM");
            const std::string gbkHead("\xB5\xDA\xD2\xBB", 4);
            const auto ansi = npcc::edit::DetectEncodingFromHead(gbkHead, 936);
            Check(ansi.uniMode == npcc::edit::UniMode::Ansi && ansi.codePage == 936,
                  "头部探测出 ANSI");
            const std::string utf16Head("\xFF\xFE", 2);
            const auto utf16 = npcc::edit::DetectEncodingFromHead(utf16Head, 936);
            Check(utf16.uniMode == npcc::edit::UniMode::Utf16LE, "头部探测出 UTF-16LE");
        }

        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "notepadcc_tier_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        // 造一份有规律的多行文本，便于核对装载完整性
        std::string big;
        big.reserve(16 * 1024);
        for (int i = 0; i < 800; ++i) {
            big += "line " + std::to_string(i) + " abcdefghijklmnopqrstuvwxyz\n";
        }
        const std::filesystem::path bigPath = dir / "big.txt";
        npcc::io::WriteFileAtomic(bigPath.wstring(), big);

        // 内存映射本身
        {
            npcc::io::MappedFile mapped(bigPath.wstring());
            Check(mapped.Valid(), "映射文件成功");
            Check(mapped.Size() == big.size(), "映射大小与文件一致");
            Check(mapped.Data() != nullptr &&
                      std::string_view(mapped.Data(), 5) == "line ",
                  "映射内容可读");

            npcc::io::MappedFile missing((dir / "no_such.txt").wstring());
            Check(!missing.Valid(), "映射不存在的文件应当失败");
            Check(!missing.Error().empty(), "失败时给出可读原因");
        }

        HWND host = CreateHiddenHost();
        Check(host != nullptr, "创建隐藏宿主窗口");
        if (host != nullptr) {
            npcc::core::DocumentManager manager(host);
            // 把阈值压低，用几个小文件就能覆盖三档，不必真造几十 MB 的文件
            manager.TierPolicyRef().normalMaxBytes = 1024;
            manager.TierPolicyRef().largeMaxBytes = 64 * 1024;
            manager.TierPolicyRef().refuseAboveBytes = 256 * 1024;
            Check(big.size() > manager.TierPolicyRef().normalMaxBytes &&
                      big.size() <= manager.TierPolicyRef().largeMaxBytes,
                  "测试用的文件体积落在 T2 区间内");

            const auto smallPath = dir / "small.txt";
            npcc::io::WriteFileAtomic(smallPath.wstring(), "tiny\n");
            const auto open1 = manager.Open(smallPath.wstring());
            Check(open1.status == npcc::core::OpenStatus::Ok, "小文件打开成功");
            Check(open1.document != nullptr &&
                      open1.document->buffer.tier == npcc::edit::FileTier::Normal,
                  "小文件归入常规档");
            Check(open1.document != nullptr && !open1.document->buffer.isReadOnly,
                  "常规档可编辑");

            const auto open2 = manager.Open(bigPath.wstring());
            Check(open2.status == npcc::core::OpenStatus::Ok, "大文件打开成功");
            Check(open2.document != nullptr &&
                      open2.document->buffer.tier == npcc::edit::FileTier::Large,
                  "大文件归入 T2 档");
            if (open2.document != nullptr && open2.document->view) {
                // 分块装载后的内容必须与文件完全一致
                const std::string loaded = open2.document->view->GetAllText();
                Check(loaded.size() == big.size(), "分块装载后长度一致");
                Check(loaded == big, "分块装载后内容一致");
                Check(!open2.document->buffer.isReadOnly, "T2 档仍可编辑");
                Check(!open2.document->view->IsModified(), "刚一打开不应是已修改状态");
            }

            // 把阈值再压低，让同一份文件落进 T3
            manager.TierPolicyRef().largeMaxBytes = 2048;
            npcc::io::WriteFileAtomic((dir / "huge.txt").wstring(), big);
            const auto open4 = manager.Open((dir / "huge.txt").wstring());
            Check(open4.status == npcc::core::OpenStatus::Ok, "超大文件（相对阈值）打开成功");
            Check(open4.document != nullptr &&
                      open4.document->buffer.tier == npcc::edit::FileTier::Huge,
                  "归入 T3 只读档");
            if (open4.document != nullptr && open4.document->view) {
                Check(open4.document->buffer.isReadOnly, "T3 档标记为只读");
                Check(open4.document->view->GetAllText() == big, "T3 装载内容一致");
                // 只读必须真的挡住写入
                open4.document->view->SetCaretPosition(0);
                open4.document->view->ReplaceSelection("X");
                Check(open4.document->view->GetAllText() == big, "只读文档拒绝写入");
            }

            // 超过拒绝上限
            manager.TierPolicyRef().refuseAboveBytes = 4096;
            const auto refused = manager.Open((dir / "huge.txt").wstring());
            Check(refused.status == npcc::core::OpenStatus::AlreadyOpen,
                  "已打开的文档会直接复用，不再走体积判定");
            npcc::io::WriteFileAtomic((dir / "toobig.txt").wstring(), big);
            const auto tooBig = manager.Open((dir / "toobig.txt").wstring());
            Check(tooBig.status == npcc::core::OpenStatus::TooLarge,
                  "超过拒绝上限时报 TooLarge");
            Check(!tooBig.message.empty(), "拒绝时给出可读说明");

            std::filesystem::remove_all(dir, ec);
            ::DestroyWindow(host);
        }
    }

    // ---------------------------------------------------------------- 文件状态检测
    Section("FileWatcher（外部改动检测）");
    {
        const auto plainWrite = [](const std::filesystem::path& path, std::string_view bytes) {
            return WriteFileDirect(path, bytes);
        };

        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "notepadcc_watch_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path target = dir / "watched.txt";
        plainWrite(target, "first\n");

        npcc::io::FileWatcher watcher;
        std::mutex mutex;
        std::vector<std::pair<std::wstring, npcc::io::FileChangeKind>> events;
        watcher.SetCallback([&mutex, &events](const std::wstring& path,
                                              npcc::io::FileChangeKind kind) {
            std::lock_guard<std::mutex> guard(mutex);
            events.emplace_back(path, kind);
        });

        Check(watcher.WatchedCount() == 0, "初始没有监视任何文件");
        watcher.Watch(target.wstring());
        Check(watcher.WatchedCount() == 1, "监视了一个文件");
        watcher.Watch(target.wstring());
        Check(watcher.WatchedCount() == 1, "重复监视同一个文件不会算两次");

        // 等回调：目录通知是异步的，给它一点时间
        const auto waitForEvent = [&mutex, &events](std::size_t wanted,
                                                    int timeoutMs) -> bool {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    if (events.size() >= wanted) {
                        return true;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };

        // 目录通知是异步的，偶尔会有秒级延迟；窗口给宽一点，
        // 免得留下一个偶发失败的测试
        plainWrite(target, "second\n");
        Check(waitForEvent(1, 10000), "外部修改触发了通知");
        {
            std::lock_guard<std::mutex> guard(mutex);
            Check(!events.empty(), "收到了事件");
            if (!events.empty()) {
                Check(events[0].second == npcc::io::FileChangeKind::Modified,
                      "事件类型是 Modified");
                Check(events[0].first.size() >= target.wstring().size() &&
                          events[0].first.compare(events[0].first.size() -
                                                      target.wstring().size(),
                                                  target.wstring().size(),
                                                  target.wstring()) == 0,
                      "事件指向被监视的那个文件");
            }
        }

        // 取消监视之后不该再有通知
        watcher.Unwatch(target.wstring());
        Check(watcher.WatchedCount() == 0, "取消监视后计数归零");
        {
            // 清空必须放在 Unwatch **之后**：取消监视时，可能刚好有一个通知已经进了
            // 回调、还没落地，它会在 join 之前补进来。不清掉它，下面那句断言就会偶发失败
            // （实测偶发过一次）。
            std::lock_guard<std::mutex> guard(mutex);
            events.clear();
        }
        plainWrite(target, "third\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        {
            std::lock_guard<std::mutex> guard(mutex);
            Check(events.empty(), "取消监视后不再收到通知");
        }

        // 删除文件应当上报 Deleted
        watcher.Watch(target.wstring());
        {
            std::lock_guard<std::mutex> guard(mutex);
            events.clear();
        }
        std::filesystem::remove(target, ec);
        Check(waitForEvent(1, 10000), "删除文件触发了通知");
        {
            std::lock_guard<std::mutex> guard(mutex);
            if (!events.empty()) {
                Check(events[0].second == npcc::io::FileChangeKind::Deleted,
                      "事件类型是 Deleted");
            }
        }

        watcher.Clear();
        Check(watcher.WatchedCount() == 0, "Clear 之后不再监视任何文件");

        // 文件身份：改了内容之后 size/时间必须变
        {
            const auto idPath = dir / "identity.txt";
            plainWrite(idPath, "abc");
            std::uint64_t size1 = 0;
            std::uint64_t time1 = 0;
            Check(npcc::io::QueryFileIdentity(idPath.wstring(), size1, time1),
                  "查询文件身份成功");
            Check(size1 == 3, "身份里的体积正确");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            plainWrite(idPath, "abcdef");
            std::uint64_t size2 = 0;
            std::uint64_t time2 = 0;
            Check(npcc::io::QueryFileIdentity(idPath.wstring(), size2, time2),
                  "再次查询文件身份成功");
            Check(size2 == 6 && (size2 != size1 || time2 != time1),
                  "体积或时间戳能反映出改动");
        }

        std::filesystem::remove_all(dir, ec);
    }

    // ---------------------------------------------------------------- 外部改动与重新加载
    Section("外部改动检测与重新加载");
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "notepadcc_reload_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path path = dir / "doc.txt";
        WriteFileDirect(path, "first\n");

        HWND host = CreateHiddenHost();
        Check(host != nullptr, "创建隐藏宿主窗口");
        if (host != nullptr) {
            npcc::core::DocumentManager manager(host);
            const auto opened = manager.Open(path.wstring());
            Check(opened.status == npcc::core::OpenStatus::Ok, "打开文件成功");

            if (opened.document != nullptr) {
                Check(!manager.HasExternallyChanged(*opened.document),
                      "刚打开时没有外部改动");

                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                WriteFileDirect(path, "second\n");
                Check(manager.HasExternallyChanged(*opened.document),
                      "磁盘上的改动被检测到");

                // 用户选择"保留当前内容"时刷新记录，之后不该反复提示
                manager.RefreshIdentity(*opened.document);
                Check(!manager.HasExternallyChanged(*opened.document),
                      "刷新记录之后不再报改动");

                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                WriteFileDirect(path, "third\n");
                Check(manager.HasExternallyChanged(*opened.document), "再次改动被检测到");

                std::wstring error;
                Check(manager.Reload(*opened.document, error), "重新加载成功");
                CheckEqual(opened.document->view->GetAllText(), "third\n",
                           "重新加载后内容来自磁盘");
                Check(!manager.HasExternallyChanged(*opened.document),
                      "重新加载后不再报改动");

                // 有未保存改动时重新加载：应当以磁盘为准并清掉修改标记
                opened.document->view->SetCaretPosition(0);
                opened.document->view->ReplaceSelection("X");
                Check(opened.document->view->IsModified(), "编辑之后处于已修改状态");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                WriteFileDirect(path, "fourth\n");
                Check(manager.Reload(*opened.document, error),
                      "覆盖未保存改动也能重新加载");
                CheckEqual(opened.document->view->GetAllText(), "fourth\n",
                           "内容被磁盘版本替换");
                Check(!opened.document->view->IsModified(), "重新加载后修改标记被清除");

                // 文件被删除同样算"变了"
                std::filesystem::remove(path, ec);
                Check(manager.HasExternallyChanged(*opened.document),
                      "文件被删除也算外部改动");
                Check(!manager.Reload(*opened.document, error),
                      "文件不存在时重新加载应当失败");
                Check(!error.empty(), "失败时给出可读原因");
            }
            ::DestroyWindow(host);
        }
        std::filesystem::remove_all(dir, ec);
    }

    Section("快捷键（命令文本 / 绑定编辑 / shortcuts.xml）");
    {
        using npcc::core::CmdCategory;
        using npcc::core::CommandRegistry;
        using npcc::core::ShortcutBinding;
        using npcc::core::ShortcutStore;

        // ---- 命令显示文本：菜单助记符与结尾的快捷键提示都要去掉 ----
        CheckWide(npcc::core::StripAccelerator(L"保存(&S)"), L"保存", "去掉结尾的(&S)");
        CheckWide(npcc::core::StripAccelerator(L"列块编辑(&C)..."), L"列块编辑",
                  "省略号与助记符一起去掉");
        CheckWide(npcc::core::StripAccelerator(L"关于 notepadcc(&A)"), L"关于 notepadcc",
                  "中英混排的助记符");
        CheckWide(npcc::core::StripAccelerator(L"普通文本"), L"普通文本", "没有助记符时原样返回");
        CheckWide(npcc::core::StripAccelerator(L"UTF-8（带 BOM）"), L"UTF-8（带 BOM）",
                  "普通括号不能被当成助记符砍掉");
        CheckWide(npcc::core::CategoryText(CmdCategory::File), L"文件", "类别名：文件");
        CheckWide(npcc::core::CategoryText(CmdCategory::Settings), L"设置", "类别名：设置");
        CheckWide(npcc::core::CategoryText(CmdCategory::Plugin), L"插件", "类别名：插件");
        Check(npcc::core::CategoryMnemonic(CmdCategory::File) == L'F', "类别助记符：F");

        // ---- 快捷键显示文本 ----
        Check(npcc::core::ShortcutText(npcc::core::kModNone, 0).empty(), "未绑定的快捷键显示为空");
        CheckWide(npcc::core::ShortcutText(npcc::core::kModCtrl, 'S'), L"Ctrl+S", "Ctrl+S 的显示");
        CheckWide(npcc::core::ShortcutText(
                      npcc::core::kModCtrl | npcc::core::kModAlt | npcc::core::kModShift, 'S'),
                  L"Ctrl+Alt+Shift+S", "修饰键顺序固定为 Ctrl→Alt→Shift");

        // ---- 可绑定规则 ----
        Check(!npcc::core::IsBindableShortcut(npcc::core::kModNone, 0), "空按键不可绑定");
        Check(!npcc::core::IsBindableShortcut(npcc::core::kModNone, 'A'),
              "单按字母不可绑定（否则打字就会触发命令）");
        Check(!npcc::core::IsBindableShortcut(npcc::core::kModShift, 'A'),
              "Shift+字母不可绑定（Shift+A 就是大写 A）");
        Check(npcc::core::IsBindableShortcut(npcc::core::kModCtrl, 'A'), "Ctrl+字母可绑定");
        Check(npcc::core::IsBindableShortcut(npcc::core::kModAlt, 'A'), "Alt+字母可绑定");
        Check(npcc::core::IsBindableShortcut(npcc::core::kModNone, VK_F2), "F2 可绑定");
        Check(npcc::core::IsBindableShortcut(npcc::core::kModShift, VK_F3), "Shift+F3 可绑定");
        Check(npcc::core::IsBindableShortcut(npcc::core::kModNone, VK_INSERT), "Insert 可绑定");
        Check(!npcc::core::IsBindableShortcut(npcc::core::kModNone, VK_HOME),
              "Home 不可绑定（要留给界面导航）");
        Check(!npcc::core::IsBindableShortcut(npcc::core::kModCtrl, VK_CONTROL),
              "修饰键自己不可绑定");

        // ---- 注册表上的绑定编辑 ----
        {
            CommandRegistry registry;
            const npcc::core::CmdId a = AddTestCommand(registry, CmdCategory::File, L"cmd.a", L"A");
            const npcc::core::CmdId b = AddTestCommand(registry, CmdCategory::Edit, L"cmd.b", L"B");
            registry.AssignShortcut(a, npcc::core::kModCtrl, 'K');
            registry.AssignShortcut(b, npcc::core::kModCtrl, 'L');
            Check(registry.BindingsOf(a).size() == 1, "AssignShortcut 建立一条绑定");

            // 一个命令多条绑定时，AssignShortcut 要整体替换而不是追加
            registry.BindShortcut(a, npcc::core::kModCtrl, VK_TAB, false);
            Check(registry.BindingsOf(a).size() == 2, "允许一个命令挂多条绑定");
            registry.AssignShortcut(a, npcc::core::kModAlt, 'X');
            Check(registry.BindingsOf(a).size() == 1, "AssignShortcut 替换该命令的全部绑定");
            Check(registry.ResolveShortcut(npcc::core::kModAlt, 'X') == a, "替换后新组合生效");
            Check(registry.ResolveShortcut(npcc::core::kModCtrl, 'K') == 0, "旧组合已被摘掉");

            Check(!registry.BindingsOf(b).empty(), "另一个命令不受影响");
            registry.ClearShortcutsOf(b);
            Check(registry.BindingsOf(b).empty(), "ClearShortcutsOf 只清指定命令");
            Check(registry.BindingsOf(a).size() == 1, "ClearShortcutsOf 不碰别的命令");

            registry.BindShortcut(b, npcc::core::kModCtrl, 'L', false);
            Check(registry.UnbindShortcut(b, npcc::core::kModCtrl, 'L'), "精确摘掉一条绑定");
            Check(registry.BindingsOf(b).empty(), "摘掉之后该命令没有绑定");
            Check(!registry.UnbindShortcut(b, npcc::core::kModCtrl, 'L'), "再摘一次返回失败");

            const std::vector<ShortcutBinding> keep = registry.AllBindings();
            registry.ClearShortcuts();
            Check(registry.AllBindings().empty(), "ClearShortcuts 清空全部");
            registry.SetShortcuts(keep);
            Check(registry.AllBindings().size() == keep.size(), "SetShortcuts 整表恢复");
        }

        // ---- shortcuts.xml 往返 ----
        npcc::infra::InitializePaths(true);
        const std::filesystem::path path = ShortcutStore::Path();
        std::error_code ec;
        std::filesystem::remove(path, ec);

        CommandRegistry saved;
        const npcc::core::CmdId saveA =
            AddTestCommand(saved, CmdCategory::File, L"cmd.save.a", L"命令 A(&A)");
        const npcc::core::CmdId saveB =
            AddTestCommand(saved, CmdCategory::Edit, L"cmd.save.b", L"命令 B(&B)");
        saved.AssignShortcut(saveA, npcc::core::kModCtrl, 'K');
        saved.BindShortcut(saveB, npcc::core::kModCtrl, VK_TAB, false);
        saved.BindShortcut(saveB, npcc::core::kModCtrl, VK_NEXT, false);
        // cmd.save.c 故意不绑定 → 文件里应出现 Key="0"
        AddTestCommand(saved, CmdCategory::View, L"cmd.save.c", L"命令 C(&C)");
        {
            std::wstring error;
            Check(ShortcutStore().Save(saved, &error), "保存 shortcuts.xml 成功");
            Check(std::filesystem::exists(path), "shortcuts.xml 已落盘");
            const std::string text = [&path] {
                std::string raw;
                const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                                  nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    return raw;
                }
                char buffer[4096] = {};
                DWORD read = 0;
                if (::ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr) != 0) {
                    raw.assign(buffer, read);
                }
                ::CloseHandle(file);
                return raw;
            }();
            Check(text.find("cmd.save.a") != std::string::npos, "文件里按命令键记录");
            Check(text.find("Key=\"0\"") != std::string::npos, "没绑定的命令记成 Key=\"0\"");
        }

        {
            // 新注册表：默认方案与文件不同，且多出一个文件里没有的命令 D
            CommandRegistry loaded;
            const npcc::core::CmdId a =
                AddTestCommand(loaded, CmdCategory::File, L"cmd.save.a", L"命令 A(&A)");
            const npcc::core::CmdId b =
                AddTestCommand(loaded, CmdCategory::Edit, L"cmd.save.b", L"命令 B(&B)");
            const npcc::core::CmdId c =
                AddTestCommand(loaded, CmdCategory::View, L"cmd.save.c", L"命令 C(&C)");
            const npcc::core::CmdId d =
                AddTestCommand(loaded, CmdCategory::Help, L"cmd.save.d", L"命令 D(&D)");
            loaded.AssignShortcut(a, npcc::core::kModCtrl, 'P');   // 默认值应被文件覆盖
            loaded.AssignShortcut(b, npcc::core::kModCtrl, VK_TAB);
            loaded.AssignShortcut(c, npcc::core::kModCtrl, 'L');   // 默认值应被 Key="0" 清掉
            loaded.AssignShortcut(d, npcc::core::kModCtrl, 'Q');   // 文件里没有 → 保持默认

            std::wstring error;
            std::vector<std::wstring> warnings;
            Check(ShortcutStore().Load(loaded, &error, &warnings), "加载 shortcuts.xml 成功");
            Check(error.empty(), "正常文件不报错");
            Check(warnings.empty(), "正常文件不产生警告");

            Check(loaded.ResolveShortcut(npcc::core::kModCtrl, 'K') == a, "文件里的绑定覆盖了默认值");
            Check(loaded.ResolveShortcut(npcc::core::kModCtrl, 'P') == 0, "被覆盖的默认绑定不再存在");

            const std::vector<ShortcutBinding> multi = loaded.BindingsOf(b);
            Check(multi.size() == 2, "多条绑定原样恢复");
            Check(loaded.ResolveShortcut(npcc::core::kModCtrl, VK_TAB) == b, "多绑定之一：Ctrl+Tab");
            Check(loaded.ResolveShortcut(npcc::core::kModCtrl, VK_NEXT) == b, "多绑定之二：Ctrl+PgDn");

            Check(loaded.BindingsOf(c).empty(), "Key=\"0\" 的命令保持无绑定（删除不会被复活）");
            Check(loaded.ResolveShortcut(npcc::core::kModCtrl, 'Q') == d,
                  "文件里没有的命令保留默认绑定（新增命令不会失去默认快捷键）");
        }

        // ---- 文件里出现不认识的命令 / 坏文件 ----
        {
            const std::string custom =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadPlusCompat schemaVersion=\"1\">\n"
                "  <InternalCommands>\n"
                "    <Shortcut name=\"cmd.unknown\" Ctrl=\"yes\" Key=\"75\"/>\n"
                "    <Shortcut Ctrl=\"yes\" Alt=\"no\" Shift=\"no\" Key=\"76\"/>\n"
                "    <Shortcut name=\"cmd.save.a\" Ctrl=\"yes\" Key=\"77\"/>\n"
                "  </InternalCommands>\n"
                "</NotepadPlusCompat>\n";
            Check(WriteFileDirect(path, custom), "写入测试用 shortcuts.xml");
            CommandRegistry registry;
            const npcc::core::CmdId a =
                AddTestCommand(registry, CmdCategory::File, L"cmd.save.a", L"命令 A(&A)");
            std::vector<std::wstring> warnings;
            std::wstring error;
            Check(ShortcutStore().Load(registry, &error, &warnings), "含未知命令的文件仍能加载");
            Check(warnings.size() >= 2, "未知命令名与缺 name 的记录都给出警告");
            Check(registry.ResolveShortcut(npcc::core::kModCtrl, 'M') == a,
                  "能识别的记录照常生效");
        }
        {
            Check(WriteFileDirect(path, "这不是 XML"), "写入损坏的 shortcuts.xml");
            CommandRegistry registry;
            AddTestCommand(registry, CmdCategory::File, L"cmd.save.a", L"命令 A(&A)");
            std::wstring error;
            Check(!ShortcutStore().Load(registry, &error), "损坏文件加载失败");
            Check(!error.empty(), "失败时给出可读原因");
        }
        std::filesystem::remove(path, ec);
    }

    // ---------------------------------------------------------------- 宏
    // 宏是"把操作序列变成可重复的资产"，所以这一段的重心是：**录下来的东西回放出来
    // 结果一样**，以及**存进文件再读回来还是同一条宏**。只测"录制器存下了几个步骤"
    // 是测不到这两件事的。
    Section("宏（录制 / 回放 / shortcuts.xml 持久化）");
    {
        using npcc::core::CmdId;
        using npcc::core::Command;
        using npcc::core::Macro;
        using npcc::core::MacroAction;
        using npcc::core::MacroRecorder;
        using npcc::core::MacroRunResult;
        using npcc::core::MacroRunner;

        npcc::infra::InitializePaths(true);

        Command sampleCommand;
        sampleCommand.id = 57349;
        sampleCommand.key = L"cmd.tool.sortAsc";
        sampleCommand.text = L"升序排序";

        // 1) 录制：文本按空闲合并、命令把文本截断、停止之后不再记
        {
            MacroRecorder recorder;
            Check(!recorder.Recording() && recorder.Empty(), "没开始录制时是空的");
            recorder.Start();
            Check(recorder.Recording(), "开始录制");
            recorder.RecordChar(L'a', 1000);
            recorder.RecordChar(L'b', 1100);   // 100 ms 内 → 合并进同一步
            Check(recorder.Actions().size() == 1, "连续输入合并成一步");
            Check(recorder.Actions()[0].kind == MacroAction::Kind::InsertText &&
                      recorder.Actions()[0].text == L"ab",
                  "合并后的文本内容正确");
            recorder.RecordChar(L'c', 2000);   // 间隔 900 ms → 新的一步
            Check(recorder.Actions().size() == 2, "空闲超过 300 ms 就另起一步");
            recorder.RecordCommand(sampleCommand);
            recorder.RecordChar(L'd', 2100);
            Check(recorder.Actions().size() == 4, "命令把文本段落截断（命令前后各一段）");
            Check(recorder.Actions()[2].kind == MacroAction::Kind::Command &&
                      recorder.Actions()[2].commandId == 57349 &&
                      recorder.Actions()[2].commandName == L"cmd.tool.sortAsc",
                  "命令步骤记下了 ID 与命令键");
            recorder.RecordMessage(2170, 1, 0);
            Check(recorder.Actions().size() == 5 &&
                      recorder.Actions()[4].kind == MacroAction::Kind::ScintillaMessage &&
                      recorder.Actions()[4].message == 2170 && recorder.Actions()[4].wParam == 1,
                  "Scintilla 消息按数值参数记下来");
            recorder.Stop();
            Check(!recorder.Recording(), "停止录制");
            recorder.RecordCommand(sampleCommand);
            Check(recorder.Actions().size() == 5, "停止之后不再记录");
            recorder.Reset();
            Check(recorder.Empty(), "Reset 清空");
        }

        // 2) 回放：顺序要对、次数要对、失败要报出"第几步"、能中断
        {
            Macro macro;
            macro.name = L"测试宏";
            MacroAction text;
            text.kind = MacroAction::Kind::InsertText;
            text.text = L"xy";
            macro.actions.push_back(text);
            MacroAction command;
            command.kind = MacroAction::Kind::Command;
            command.commandId = 42;
            macro.actions.push_back(command);
            MacroAction message;
            message.kind = MacroAction::Kind::ScintillaMessage;
            message.message = 2170;
            message.wParam = 3;
            macro.actions.push_back(message);

            std::vector<std::wstring> trace;
            MacroRunner runner;
            runner.insertText = [&trace](std::wstring_view value) {
                trace.push_back(L"text:" + std::wstring(value));
            };
            runner.runCommand = [&trace](CmdId id) {
                trace.push_back(L"cmd:" + std::to_wstring(id));
                return true;
            };
            runner.sendMessage = [&trace](const MacroAction& action) {
                trace.push_back(L"msg:" + std::to_wstring(action.message) + L":" +
                                std::to_wstring(action.wParam));
            };
            const MacroRunResult ok = npcc::core::RunMacro(macro, runner, 1, true);
            Check(ok.ok && ok.completed == 1 && ok.failedStep == -1, "一遍跑完且没有失败");
            Check(trace.size() == 3 && trace[0] == L"text:xy" && trace[1] == L"cmd:42" &&
                      trace[2] == L"msg:2170:3",
                  "步骤按录制顺序执行");

            trace.clear();
            const MacroRunResult three = npcc::core::RunMacro(macro, runner, 3, true);
            Check(three.ok && three.completed == 3 && trace.size() == 9, "重复 3 遍就真的跑 3 遍");

            // 第 2 步失败：默认立即停（第 3 步不执行），宽松模式则继续
            trace.clear();
            MacroRunner failing = runner;
            failing.runCommand = [&trace](CmdId id) {
                trace.push_back(L"cmd:" + std::to_wstring(id));
                return false;   // 这一条命令失败（比如当前版本里没有它）
            };
            const MacroRunResult stopped = npcc::core::RunMacro(macro, failing, 1, true);
            Check(!stopped.ok && stopped.failedStep == 2 && stopped.failedRepeat == 1,
                  "失败时报出是第几步");
            Check(trace.size() == 2, "失败即停：第 3 步没有再执行");
            Check(stopped.message.find(L"第 2 步") != std::wstring::npos, "说明里写清了第几步");

            trace.clear();
            const MacroRunResult lenient = npcc::core::RunMacro(macro, failing, 1, false);
            Check(!lenient.ok && lenient.completed == 1 && trace.size() == 3,
                  "宽松模式跳过失败步骤继续跑完");

            // 中断：keepGoing 返回 false 之后立刻停手
            trace.clear();
            int guard = 0;
            MacroRunner interruptible = runner;
            interruptible.keepGoing = [&guard] { return ++guard <= 1; };
            const MacroRunResult cut = npcc::core::RunMacro(macro, interruptible, 5, true);
            Check(cut.ok && cut.message == L"已中断" && trace.size() <= 3,
                  "用户中断时不算失败，但要如实报告");

            Macro empty;
            Check(!npcc::core::RunMacro(empty, runner, 1, true).ok, "空宏直接报错，不静默成功");
        }

        // 3) 持久化：写进 shortcuts.xml 再读回来，是同一条宏
        {
            Macro macro;
            macro.name = L"TrimAndSort";
            macro.modifiers = npcc::core::kModCtrl | npcc::core::kModAlt;
            macro.keyCode = 84;
            MacroAction text;
            text.kind = MacroAction::Kind::InsertText;
            text.text = L"line1\nline2";   // 带换行的文本要能原样往返
            macro.actions.push_back(text);
            MacroAction command;
            command.kind = MacroAction::Kind::Command;
            command.commandId = sampleCommand.id;
            command.commandName = sampleCommand.key;
            macro.actions.push_back(command);
            MacroAction message;
            message.kind = MacroAction::Kind::ScintillaMessage;
            message.message = 2170;
            message.wParam = 1;
            macro.actions.push_back(message);

            npcc::infra::XmlDocument document;
            npcc::infra::XmlElementPtr root = document.CreateRoot(L"NotepadPlusCompat");
            npcc::core::AppendMacrosXml(*root, {macro});
            const std::string xml = document.ToUtf8();
            Check(xml.find("<Macros>") != std::string::npos &&
                      xml.find("TrimAndSort") != std::string::npos,
                  "写出的 XML 里有宏名");

            npcc::infra::XmlDocument reparsed;
            std::wstring parseError;
            Check(npcc::infra::XmlDocument::Parse(xml, reparsed, &parseError),
                  "写出的 XML 能重新解析");
            std::vector<std::wstring> warnings;
            const std::vector<Macro> back = npcc::core::ReadMacrosXml(
                *reparsed.Root(),
                [&sampleCommand](const std::wstring& key) -> CmdId {
                    return key == sampleCommand.key ? sampleCommand.id : 0;
                },
                warnings);
            Check(warnings.empty(), "往返没有警告");
            Check(back.size() == 1, "读回一条宏");
            if (!back.empty()) {
                Check(back[0].name == L"TrimAndSort" && back[0].keyCode == 84 &&
                          back[0].modifiers == (npcc::core::kModCtrl | npcc::core::kModAlt),
                      "宏名与快捷键往返一致");
                Check(back[0].actions.size() == 3, "三个步骤都在");
                Check(back[0].actions[0].kind == MacroAction::Kind::InsertText &&
                          back[0].actions[0].text == L"line1\nline2",
                      "带换行的文本往返一致");
                Check(back[0].actions[1].kind == MacroAction::Kind::Command &&
                          back[0].actions[1].commandId == sampleCommand.id &&
                          back[0].actions[1].commandName == sampleCommand.key,
                      "命令步骤往返一致（ID 与命令键都在）");
                Check(back[0].actions[2].kind == MacroAction::Kind::ScintillaMessage &&
                          back[0].actions[2].message == 2170 && back[0].actions[2].wParam == 1,
                      "消息步骤往返一致");
            }

            // 只有命令键、没有 ID 的文件（手工写的、或跨版本迁移过来的）也要认
            const std::string byName =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadPlusCompat schemaVersion=\"1\">\n"
                "  <Macros>\n"
                "    <Macro name=\"手写的\" Ctrl=\"no\" Alt=\"no\" Shift=\"no\" Key=\"0\">\n"
                "      <Action type=\"0\" name=\"cmd.tool.sortAsc\"/>\n"
                "    </Macro>\n"
                "  </Macros>\n"
                "</NotepadPlusCompat>\n";
            npcc::infra::XmlDocument hand;
            Check(npcc::infra::XmlDocument::Parse(byName, hand, &parseError), "手写的文件能解析");
            warnings.clear();
            const std::vector<Macro> resolved = npcc::core::ReadMacrosXml(
                *hand.Root(),
                [&sampleCommand](const std::wstring& key) -> CmdId {
                    return key == sampleCommand.key ? sampleCommand.id : 0;
                },
                warnings);
            Check(resolved.size() == 1 && !resolved.empty() &&
                      resolved[0].actions[0].commandId == sampleCommand.id,
                  "只有命令键时按名字换成 ID");

            // 坏文件：认不出的步骤类型、没名字的宏、空文本——跳过并记警告，别整个失效
            const std::string broken =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadPlusCompat schemaVersion=\"1\">\n"
                "  <Macros>\n"
                "    <Macro Ctrl=\"no\" Alt=\"no\" Shift=\"no\" Key=\"0\">\n"
                "      <Action type=\"1\" message=\"2170\"/>\n"
                "    </Macro>\n"
                "    <Macro name=\"认不得的步骤\" Key=\"0\">\n"
                "      <Action type=\"7\" message=\"1\"/>\n"
                "    </Macro>\n"
                "    <Macro name=\"半条好的\" Key=\"0\">\n"
                "      <Action type=\"7\" message=\"1\"/>\n"
                "      <Action type=\"2\" sParam=\"ok\"/>\n"
                "    </Macro>\n"
                "  </Macros>\n"
                "</NotepadPlusCompat>\n";
            npcc::infra::XmlDocument bad;
            Check(npcc::infra::XmlDocument::Parse(broken, bad, &parseError), "坏文件能解析");
            warnings.clear();
            const std::vector<Macro> survived = npcc::core::ReadMacrosXml(*bad.Root(), {}, warnings);
            Check(survived.size() == 1, "只有一条能用的宏留下来");
            Check(!survived.empty() && survived[0].name == L"半条好的" &&
                      survived[0].actions.size() == 1,
                  "认不出的步骤被跳过，剩下的步骤照常留下");
            Check(warnings.size() >= 3, "每种问题都留了警告");
        }

        // 4) 与快捷键共用一个文件：改快捷键那条路不能把宏抹掉
        {
            npcc::core::CommandRegistry registry;
            Command command;
            command.id = 57349;
            command.key = L"cmd.tool.sortAsc";
            command.text = L"升序排序";
            registry.Register(command);

            const std::filesystem::path path = npcc::core::ShortcutStore::Path();
            std::error_code ec;
            std::filesystem::remove(path, ec);

            Macro macro;
            macro.name = L"存下来的";
            MacroAction action;
            action.kind = MacroAction::Kind::Command;
            action.commandId = command.id;
            action.commandName = command.key;
            macro.actions.push_back(action);

            npcc::core::ShortcutStore store;
            store.SetMacros({macro});
            std::wstring error;
            Check(store.Save(registry, &error), "保存（快捷键 + 宏）成功");

            npcc::core::ShortcutStore reloaded;
            Check(reloaded.Load(registry, &error), "重新加载成功");
            Check(reloaded.Macros().size() == 1 && reloaded.Macros()[0].name == L"存下来的",
                  "宏随文件读回来");

            // 现在模拟"用户改了快捷键又保存"：store 手里有宏，所以写出去的文件里宏还在
            registry.AssignShortcut(command.id, npcc::core::kModCtrl, 'K');
            Check(reloaded.Save(registry, &error), "改完快捷键再保存");
            npcc::core::ShortcutStore again;
            Check(again.Load(registry, &error), "再读一次");
            Check(again.Macros().size() == 1, "改快捷键没有把宏抹掉（一份文件只有一个写者）");
            const std::vector<npcc::core::ShortcutBinding> bindings = registry.BindingsOf(command.id);
            Check(bindings.size() == 1 && bindings[0].keyCode == 'K', "快捷键改动也生效了");
            std::filesystem::remove(path, ec);
        }
    }

    // ---------------------------------------------------------------- 插件宿主
    // 这一段的重点是"**真的把 DLL 加载起来**"：示例插件在构建树里（CMake 会先把它编译好），
    // 测试把它拷进配置目录的 plugins/ 下，再让 PluginHost 去发现、校验、加载、注册命令。
    Section("插件宿主（SDK 校验 / 发现 / 加载 / 拒绝）");
    {
        using npcc::plugin::HostActions;
        using npcc::plugin::PluginHost;
        using npcc::plugin::PluginStatus;

        npcc::infra::InitializePaths(true);
        const std::filesystem::path samples = NPCC_SAMPLE_PLUGINS_DIR;
        const std::filesystem::path root = PluginHost::PluginsRoot();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove(PluginHost::StatePath(), ec);
        std::filesystem::create_directories(root, ec);
        // 示例插件拷进来（保持目录名与 plugin.json 里声明的 dll 名一致）
        std::filesystem::copy(samples / L"hello", root / L"hello",
                              std::filesystem::copy_options::recursive, ec);
        Check(std::filesystem::exists(root / L"hello" / L"hello.dll"), "示例插件拷进配置目录");

        // 记录外壳被要求做什么（真外壳是 MainWindow，这里只验证宿主有没有正确回调）
        class Recorder : public HostActions {
        public:
            bool RegisterPluginCommand(const std::wstring& pluginName, const std::wstring& key,
                                       const std::wstring& name, void (*execute)(void*),
                                       void* pluginCookie) override {
                ++registered;
                lastPlugin = pluginName;
                lastKey = key;
                lastName = name;
                lastExecute = execute;
                lastCookie = pluginCookie;
                return true;
            }
            std::wstring ConfigPath() const override {
                return npcc::infra::ConfigRoot().wstring();
            }
            std::string EditorText() const override { return "0123456789"; }
            bool ReplaceSelection(std::string_view utf8) override {
                replaced.assign(utf8.begin(), utf8.end());
                return true;
            }
            void ShowMessage(const std::wstring&) override {}
            void Log(int, const std::wstring& text) override { logs.push_back(text); }

            int         registered = 0;
            std::wstring lastPlugin;
            std::wstring lastKey;
            std::wstring lastName;
            void (*lastExecute)(void*) = nullptr;
            void*        lastCookie = nullptr;
            std::string  replaced;
            std::vector<std::wstring> logs;
        };

        // 1) 正常加载：发现 → 校验 → Init → 注册命令
        {
            Recorder recorder;
            npcc::core::CommandRegistry registry;
            PluginHost host;
            host.LoadAll(recorder, &registry);
            Check(host.Plugins().size() == 1, "发现一个插件");
            Check(host.Plugins()[0].loaded && host.Plugins()[0].name == L"hello",
                  "hello 插件加载成功");
            Check(host.LoadedCount() == 1, "加载计数");
            Check(recorder.registered == 1, "插件注册了一条命令");
            Check(recorder.lastPlugin == L"hello" && recorder.lastKey == L"hello.insert",
                  "宿主把插件名与命令键都传对了");
            // 执行那条命令：应当回调进插件，插件再调宿主的 SetEditorText
            Check(recorder.lastExecute != nullptr, "命令回调就绪");
            if (recorder.lastExecute != nullptr) {
                recorder.lastExecute(recorder.lastCookie);
                Check(recorder.replaced == "// 来自 hello 插件\n",
                      "执行插件命令 → 插件把文本交给了宿主");
            }
            // 插件在 Init 里也调了宿主的 Log / GetConfigPath / GetEditorText
            bool sawInit = false;
            for (const std::wstring& line : recorder.logs) {
                if (line.find(L"hello 插件已初始化") != std::wstring::npos) {
                    sawInit = true;
                }
            }
            Check(sawInit, "插件的 Init 通过宿主服务写了日志");
            host.ShutdownAll();
        }

        // 2) 禁用：写进 plugins.xml，重新加载时就跳过（不是错误）
        {
            std::wstring error;
            Check(PluginHost::SetDisabled(L"hello", true, &error), "禁用写盘成功");
            Check(std::filesystem::exists(PluginHost::StatePath()), "plugins.xml 落盘");
            Check(PluginHost::IsDisabled(L"hello"), "禁用状态读得回来");
            Recorder recorder;
            npcc::core::CommandRegistry registry;
            PluginHost host;
            host.LoadAll(recorder, &registry);
            Check(host.Plugins().size() == 1 && host.Plugins()[0].disabled &&
                      !host.Plugins()[0].loaded,
                  "被禁用的插件只登记不加载");
            Check(recorder.registered == 0, "被禁用时不会注册命令");
            Check(PluginHost::SetDisabled(L"hello", false, &error), "重新启用写盘成功");
        }

        // 3) ABI 不匹配：拒载，理由里要写清"插件要几、宿主是几"
        {
            std::filesystem::copy(samples / L"badabi", root / L"badabi",
                                  std::filesystem::copy_options::recursive, ec);
            Recorder recorder;
            npcc::core::CommandRegistry registry;
            PluginHost host;
            host.LoadAll(recorder, &registry);
            Check(host.Plugins().size() == 2, "发现两个插件");
            const PluginStatus* bad = nullptr;
            for (const PluginStatus& status : host.Plugins()) {
                if (status.dllPath.filename() == L"badabi.dll") {
                    bad = &status;
                }
            }
            Check(bad != nullptr && !bad->loaded && !bad->reason.empty(), "ABI 不匹配的被拒载");
            Check(bad != nullptr && bad->reason.find(L"ABI") != std::wstring::npos,
                  "理由里说明是 ABI 版本问题");
            host.ShutdownAll();
        }

        // 4) 不是 DLL 的文件、缺导出函数的 DLL：都要给出可读原因，而不是崩
        {
            const std::filesystem::path junk = root / L"junk" / L"junk.dll";
            std::filesystem::create_directories(junk.parent_path(), ec);
            WriteFileDirect(junk.wstring(), "这不是一个 DLL，只是一段文本");
            Recorder recorder;
            npcc::core::CommandRegistry registry;
            PluginHost host;
            host.LoadAll(recorder, &registry);
            const PluginStatus* broken = nullptr;
            for (const PluginStatus& status : host.Plugins()) {
                if (status.name == L"junk") {
                    broken = &status;
                }
            }
            Check(broken != nullptr && !broken->loaded, "坏文件不会被当成插件加载");
            Check(broken != nullptr &&
                      (broken->reason.find(L"加载 DLL 失败") != std::wstring::npos ||
                       broken->reason.find(L"不是 x64") != std::wstring::npos),
                  "理由里说明是加载失败/位数不对");
            host.ShutdownAll();
        }

        // 5) 清单解析：缺字段用缺省值、坏 JSON 报错、依赖缺失则拒载
        {
            npcc::core::JsonObject object;
            std::wstring error;
            Check(npcc::core::ParseJsonObject(
                      R"({"name":"a","version":"2.1","loadOrder":5,"disabled":true})", object,
                      &error),
                  "扁平 JSON 解析成功");
            Check(object.StringOr("name", "") == "a" &&
                      object.StringOr("version", "0") == "2.1" && object.NumberOr("loadOrder", 0) == 5 &&
                      object.BoolOr("disabled", false),
                  "字符串/数字/布尔都读对了");
            Check(object.StringOr("author", "缺省") == "缺省", "缺字段用缺省值");
            Check(!npcc::core::ParseJsonObject(R"({"name": })", object, &error),
                  "坏 JSON 解析失败");
            Check(!error.empty(), "失败时给出位置");
            Check(!npcc::core::ParseJsonObject("[\"array\"]", object, &error),
                  "只认对象，数组要拒绝");

            // 依赖缺失：写一个依赖 hello 但 hello 被禁用（上面第 2 步用过、这里先启用回来）
            const std::filesystem::path dep = root / L"needsdep";
            std::filesystem::create_directories(dep, ec);
            WriteFileDirect((dep / L"plugin.json").wstring(),
                            R"({"name":"needsdep","dll":"needsdep.dll","dependencies":"nosuchplugin"})");
            WriteFileDirect((dep / L"needsdep.dll").wstring(), "MZ");
            Recorder recorder;
            npcc::core::CommandRegistry registry;
            PluginHost host;
            host.LoadAll(recorder, &registry);
            bool sawMissingDep = false;
            for (const PluginStatus& status : host.Plugins()) {
                if (status.name == L"needsdep" &&
                    status.reason.find(L"缺少依赖") != std::wstring::npos) {
                    sawMissingDep = true;
                }
            }
            Check(sawMissingDep, "依赖没满足时拒载并说明缺谁");
            host.ShutdownAll();
        }

        std::filesystem::remove_all(root, ec);
        std::filesystem::remove(PluginHost::StatePath(), ec);
    }

    // ---------------------------------------------------------------- 导出与打印
    // 生成器是纯函数：给"字节 + 样式号 + 样式外观"，吐出字符串。导出这条路上最容易错的
    // 地方（转义、样式去重、行号对齐、颜色表、UTF-8 切断）都能在这里钉死，
    // 不必靠"导出一份文件再肉眼看"。
    Section("导出与打印（HTML / RTF / 页眉页脚模板）");
    {
        using npcc::core::BuildHtml;
        using npcc::core::BuildRtf;
        using npcc::core::EscapeHtmlText;
        using npcc::core::ExpandPrintTemplate;
        using npcc::core::ExportOptions;
        using npcc::core::ExportStyle;
        using npcc::core::PrintTextContext;
        using npcc::core::StyledByte;

        // 造一份"两行、三种样式"的输入：默认 / 注释(绿斜) / 关键字(蓝粗)
        const std::string source = "// 注释\nint x = 1;\n";
        std::vector<StyledByte> bytes;
        std::vector<int> styles;
        for (const char ch : source) {
            bytes.push_back(StyledByte{ch, 0});
        }
        // 手工打样式："// 注释" 按字节算是 3 + 3 + 3 = 9 个（中文一个字三字节），
        // 接着是换行，再往后是 "int"
        for (std::size_t index = 0; index < 9; ++index) {
            bytes[index].style = 2;
        }
        for (std::size_t index = 10; index < 13; ++index) {   // "int"
            bytes[index].style = 5;
        }
        ExportOptions options;
        options.title = "sample.cpp";
        options.lineNumbers = true;
        options.pageFore = 0x1E1E1E;
        options.pageBack = 0xFFFFFF;
        const std::vector<ExportStyle> styleTable = {
            {2, 0x2E7D32, false, true},
            {5, 0x0000C0, true, false},
            {7, 0x1E1E1E, false, false},   // 与默认前景相同：不该写进 CSS
        };

        // 1) HTML 转义
        Check(EscapeHtmlText("a<b>&\"c\"") == "a&lt;b&gt;&amp;&quot;c&quot;",
              "HTML 转义五个字符（& 先换，不重复转）");
        Check(EscapeHtmlText("中文") == "中文", "中文原样保留（UTF-8 不被转义）");

        // 2) HTML：行号列、样式类、颜色、粗斜体、转义
        {
            const std::string html = BuildHtml(bytes, styleTable, options);
            Check(html.find("<td class=\"ln\">1</td>") != std::string::npos &&
                      html.find("<td class=\"ln\">2</td>") != std::string::npos,
                  "两行各有一格行号");
            Check(html.find("span.s2 { color: #2E7D32; font-style: italic; }") !=
                      std::string::npos,
                  "注释样式：绿色 + 斜体");
            Check(html.find("span.s5 { color: #0000C0; font-weight: bold; }") !=
                      std::string::npos,
                  "关键字样式：蓝色 + 粗体");
            Check(html.find("span.s7") == std::string::npos,
                  "与默认色相同又无粗斜体的样式不写 CSS（少写噪音）");
            Check(html.find("<span class=\"s2\">// 注释</span>") != std::string::npos,
                  "注释按段包成一个 span（不是一字一个）");
            Check(html.find("#include") == std::string::npos, "没有凭空多出内容");
        }

        // 3) HTML：CRLF 不留 CR、纯文本也有一行、转义进正文
        {
            std::vector<StyledByte> crlf;
            for (const char ch : std::string("a<b\r\nc")) {
                crlf.push_back(StyledByte{ch, 0});
            }
            const std::string html = BuildHtml(crlf, {}, options);
            Check(html.find("a&lt;b") != std::string::npos, "正文里的 < 被转义");
            Check(html.find('\r') == std::string::npos, "CRLF 不留 CR");
            Check(html.find("<td class=\"ln\">2</td>") != std::string::npos, "CRLF 也切成两行");
        }

        // 4) RTF：颜色表、颜色引用、粗斜体开关、转义、\par
        {
            const std::string rtf = BuildRtf(bytes, styleTable, options);
            Check(rtf.rfind("{\\rtf1", 0) == 0, "以 {\\rtf1 开头");
            Check(rtf.find("\\red46\\green125\\blue50;") != std::string::npos,
                  "颜色表里有注释色 (#2E7D32)");
            Check(rtf.find("\\red0\\green0\\blue192;") != std::string::npos,
                  "颜色表里有关键字色 (#0000C0)");
            Check(rtf.find("\\cf") != std::string::npos, "正文用 \\cf 引用颜色");
            Check(rtf.find("\\i") != std::string::npos && rtf.find("\\b") != std::string::npos,
                  "斜体与粗体开关都出现了");
            Check(rtf.find("\\par") != std::string::npos, "换行写成 \\par");
            Check(rtf.find("\\u") != std::string::npos, "非 ASCII 用 \\uN? 形式");
            // 颜色表只列用到的颜色（注释、关键字、默认 = 3 个）
            std::size_t semicolons = 0;
            for (std::size_t index = 0; index < rtf.size(); ++index) {
                if (rtf[index] == ';' && index < rtf.find("}\\n")) {
                    ++semicolons;
                }
            }
            Check(semicolons >= 3, "颜色表里至少有默认/注释/关键字三项");
        }

        // 5) 页眉页脚模板
        {
            PrintTextContext context;
            context.fileName = L"sample.cpp";
            context.filePath = L"D:\\work\\sample.cpp";
            context.page = 2;
            context.totalPages = 7;
            context.date = L"2026-10-16";
            Check(ExpandPrintTemplate(L"$(FILE_NAME)", context) == L"sample.cpp", "文件名");
            Check(ExpandPrintTemplate(L"$(FILE_PATH)", context) == L"D:\\work\\sample.cpp",
                  "完整路径（不会被 FILE_NAME 抢掉前缀）");
            Check(ExpandPrintTemplate(L"第 $(PAGE) / $(PAGES) 页", context) == L"第 2 / 7 页",
                  "页码与总页数");
            Check(ExpandPrintTemplate(L"$(DATE)", context) == L"2026-10-16", "日期");
            Check(ExpandPrintTemplate(L"$(UNKNOWN)", context) == L"$(UNKNOWN)",
                  "不认识的占位符原样留着（不静默吃掉）");
        }

        // 6) 分页循环：正常、单页、不前进（防死循环）、排不动
        {
            using npcc::core::CollectPageStarts;
            // 100 个字，每页排 30 个 → 4 页（0/30/60/90）
            const std::vector<int32_t> pages = CollectPageStarts(100, [](int32_t start) {
                return static_cast<intptr_t>(start + 30);
            });
            Check(pages.size() == 4, "100 个字节、每页 30 → 4 页");
            Check(!pages.empty() && pages[0] == 0 && pages[3] == 90, "每页起点正确");

            const std::vector<int32_t> single = CollectPageStarts(20, [](int32_t start) {
                return static_cast<intptr_t>(start + 30);
            });
            Check(single.size() == 1 && single[0] == 0, "排得下就一页");

            // 不前进必须停下：否则打印任务会卡住、纸一直出
            int calls = 0;
            const std::vector<int32_t> stuck = CollectPageStarts(100, [&calls](int32_t start) {
                ++calls;
                return static_cast<intptr_t>(start);   // 原地不动
            });
            Check(calls == 1 && stuck.size() == 1, "位置不前进时立刻停手（不死循环）");

            Check(CollectPageStarts(0, [](int32_t start) { return start + 10; }).empty(),
                  "空文档没有页");
            Check(CollectPageStarts(50, {}).empty(), "没有回调时不硬来");
            const std::vector<int32_t> failed = CollectPageStarts(50, [](int32_t) {
                return static_cast<intptr_t>(-1);
            });
            Check(failed.empty(), "排不动（-1）时返回空");
        }

        // 7) 真排版一次：拿内存 DC 当"打印机"，验证 SCI_FORMATRANGE 这条链是通的
        //    （不往真打印机上送纸；打印对话框那一段由界面回归验证"能弹出"）
        {
            const HWND host = CreateHiddenHost();
            npcc::edit::EditView view;
            HDC memoryDc = ::CreateCompatibleDC(nullptr);
            HBITMAP bitmap = ::CreateCompatibleBitmap(memoryDc, 800, 600);
            const HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);
            if (host != nullptr && view.Create(host, 920) && memoryDc != nullptr) {
                view.ApplyLightTheme();
                view.SetCodePage(npcc::edit::kCodePageUtf8);
                std::string text;
                for (int line = 0; line < 400; ++line) {
                    text += "int function_" + std::to_string(line) + "() { return " +
                            std::to_string(line) + "; }\n";
                }
                view.SetText(text);
                const int32_t length = static_cast<int32_t>(text.size());

                // 800x600 像素在 96 DPI 下 = 12000x9000 twips（1 像素 = 15 twips）
                npcc::edit::EditView::PrintRange range{};
                range.hdc = memoryDc;
                range.hdcTarget = memoryDc;
                range.pageLeft = 0;
                range.pageTop = 0;
                range.pageRight = 800 * 15;
                range.pageBottom = 600 * 15;
                range.rcLeft = 100;
                range.rcTop = 100;
                range.rcRight = 800 * 15 - 100;
                range.rcBottom = 600 * 15 - 100;
                view.SetPrintColourMode(npcc::edit::EditView::kPrintColourOnWhite);
                view.SetPrintWrapMode(npcc::edit::EditView::kPrintWrapNone);
                view.SetPrintMagnification(0);

                const std::vector<int32_t> pages =
                    npcc::core::CollectPageStarts(length, [&](int32_t start) {
                        range.start = start;
                        range.end = length;
                        return view.FormatRange(false, range);
                    });
                Check(pages.size() > 1, "400 行文本在 600 高的页面上不止一页");
                Check(!pages.empty() && pages[0] == 0, "第一页从文首开始");
                // 真画一页：不检查像素，只要求它不崩、且报告了排到哪
                range.start = pages.empty() ? 0 : pages[0];
                range.end = length;
                const intptr_t painted = view.FormatRange(true, range);
                Check(painted > 0 && painted <= length, "真排版一页能返回「下一处位置」");
            } else {
                Check(false, "创建隐藏宿主窗口或内存 DC 失败");
            }
            if (oldBitmap != nullptr) {
                ::SelectObject(memoryDc, oldBitmap);
            }
            if (bitmap != nullptr) {
                ::DeleteObject(bitmap);
            }
            if (memoryDc != nullptr) {
                ::DeleteDC(memoryDc);
            }
            if (host != nullptr) {
                ::DestroyWindow(host);
            }
        }
    }

    // ---------------------------------------------------------------- 函数列表
    // 提取器是**纯函数**：给语言与文本、返回符号表。所以这一段直接造源码比对
    // 名字/行号/层级，不涉及任何窗口——"结构提取"错了就是错了，不该靠看界面发现。
    Section("函数列表（结构提取）");
    {
        using npcc::core::ExtractFunctionList;
        using npcc::core::FunctionSymbol;
        using npcc::core::HasFunctionExtractor;

        // 1) C 家族：函数、类、层级，注释与字符串里的括号不能把层级带歪
        {
            const std::string source =
                "#include <cstdio>\n"                         // 0
                "\n"                                          // 1
                "int helper(int x) {\n"                       // 2
                "    return x + 1;\n"                         // 3
                "}\n"                                         // 4
                "\n"                                          // 5
                "class Widget {\n"                            // 6
                "public:\n"                                   // 7
                "    void draw() {\n"                         // 8
                "        printf(\"}\");\n"                    // 9 字符串里有个 }
                "    }\n"                                     // 10
                "    // } 注释里的也不该算\n"                  // 11
                "};\n"                                        // 12
                "\n"                                          // 13
                "int main() {\n"                              // 14
                "    if (1) { return 0; }\n"                  // 15 控制流不算函数
                "}\n";                                        // 16
            const std::vector<FunctionSymbol> symbols =
                ExtractFunctionList(L"cpp", source);
            Check(symbols.size() == 4, "cpp：找到 4 个符号（helper / Widget / draw / main）");
            if (symbols.size() == 4) {
                Check(symbols[0].name == L"helper" && symbols[0].line == 2 &&
                          symbols[0].depth == 0 && symbols[0].kind == L"函数",
                      "helper 在第 3 行、顶层、算函数");
                Check(symbols[1].name == L"Widget" && symbols[1].line == 6 && symbols[1].kind == L"类",
                      "Widget 是类");
                Check(symbols[2].name == L"draw" && symbols[2].line == 8 && symbols[2].depth == 1,
                      "draw 在类里面（层级 1）");
                Check(symbols[3].name == L"main" && symbols[3].line == 14 && symbols[3].depth == 0,
                      "main 回到顶层（字符串与注释里的 } 没有把层级带歪）");
            }
            Check(!HasFunctionExtractor(L"json"), "json 没有提取器");
        }

        // 2) Python：靠缩进分层，async def 与嵌套 def 都对
        {
            const std::string source =
                "import os\n"                    // 0
                "\n"                             // 1
                "def top():\n"                   // 2
                "    pass\n"                     // 3
                "\n"                             // 4
                "class Service:\n"               // 5
                "    def handle(self):\n"        // 6
                "        def inner():\n"         // 7
                "            pass\n"             // 8
                "\n"                             // 9
                "async def fetch():\n"           // 10
                "    pass\n";                    // 11
            const std::vector<FunctionSymbol> symbols = ExtractFunctionList(L"python", source);
            Check(symbols.size() == 5, "python：找到 5 个符号（含 async def）");
            if (symbols.size() == 5) {
                Check(symbols[0].name == L"top" && symbols[0].line == 2 && symbols[0].depth == 0,
                      "顶层函数 depth 0");
                Check(symbols[1].name == L"Service" && symbols[1].kind == L"类", "class 也列出来");
                Check(symbols[2].name == L"handle" && symbols[2].depth == 1,
                      "类里的方法 depth 1");
                Check(symbols[3].name == L"inner" && symbols[3].depth == 2,
                      "方法里的嵌套函数 depth 2");
                Check(symbols[4].name == L"fetch" && symbols[4].depth == 0,
                      "async def 也认，缩进回到顶层");
            }
        }

        // 3) Markdown：标题层级，代码块里的 # 不算
        {
            const std::string source =
                "# 标题一\n"                      // 0
                "正文\n"                          // 1
                "## 标题二\n"                     // 2
                "```\n"                           // 3
                "# 这是代码里的井号\n"            // 4
                "```\n"                           // 5
                "### 标题三 ###\n"                // 6
                "#没有空格不是标题\n";            // 7
            const std::vector<FunctionSymbol> symbols = ExtractFunctionList(L"markdown", source);
            Check(symbols.size() == 3, "markdown：三个标题（代码块里那个不算）");
            if (symbols.size() == 3) {
                Check(symbols[0].name == L"标题一" && symbols[0].depth == 0, "一级标题");
                Check(symbols[1].name == L"标题二" && symbols[1].depth == 1, "二级标题");
                Check(symbols[2].name == L"标题三" && symbols[2].depth == 2,
                      "三级标题（结尾的 # 会被去掉）");
            }
        }

        // 4) CSS：选择器
        {
            const std::string source =
                "body {\n"                        // 0
                "  color: red;\n"                 // 1
                "}\n"                             // 2
                ".card > .title {\n"              // 3
                "  font-weight: bold;\n"          // 4
                "}\n"                             // 5
                "@media screen {\n"               // 6
                "  .inner { color: blue; }\n"     // 7
                "}\n";                            // 8
            const std::vector<FunctionSymbol> symbols = ExtractFunctionList(L"css", source);
            Check(symbols.size() == 3, "css：两个选择器 + 一个嵌套选择器（@media 不算）");
            if (symbols.size() == 3) {
                Check(symbols[0].name == L"body" && symbols[0].depth == 0, "body");
                Check(symbols[1].name == L".card > .title" && symbols[1].depth == 0,
                      "组合选择器整段显示");
                Check(symbols[2].name == L".inner" && symbols[2].depth == 1,
                      "媒体查询里的选择器层级为 1");
            }
        }

        // 5) 边界：不认识的语言、空文本、CRLF、中文名
        {
            Check(ExtractFunctionList(L"json", "{\"a\": 1}").empty(), "不认识的语言返回空");
            Check(ExtractFunctionList(L"cpp", "").empty(), "空文本返回空");
            Check(!HasFunctionExtractor(L"我自己的语言"), "UDL 语言默认没有提取器");
            const std::string crlf = "int a() {\r\n    return 1;\r\n}\r\nint b() {\r\n}\r\n";
            const std::vector<FunctionSymbol> symbols = ExtractFunctionList(L"cpp", crlf);
            Check(symbols.size() == 2 && symbols[1].line == 3, "CRLF 文档的行号也对");
            const std::string chinese = "// 中文注释\nint 中文函数() {\n    return 0;\n}\n";
            const std::vector<FunctionSymbol> named = ExtractFunctionList(L"cpp", chinese);
            Check(named.size() == 1 && named[0].name == L"中文函数", "中文标识符也能提出来");
        }

        // 6) 面板：把符号表填进树，层级按 depth 建；选中 → 通知外壳跳转
        {
            const HWND host = CreateNotifyHost();
            if (host == nullptr) {
                Check(false, "创建通知宿主失败");
            } else {
                Check(npcc::shell::FunctionListPanel::RegisterWindowClass(
                          ::GetModuleHandleW(nullptr)),
                      "注册函数面板窗口类");
                npcc::shell::FunctionListPanel panel;
                Check(panel.Create(host, 910), "创建函数面板");
                std::vector<FunctionSymbol> symbols = {
                    {L"helper", 2, 0, L"函数"},
                    {L"Widget", 6, 0, L"类"},
                    {L"draw", 8, 1, L"函数"},
                    {L"main", 14, 0, L"函数"},
                };
                panel.SetSymbols(symbols);
                const HWND tree = panel.Tree();
                Check(tree != nullptr && ::SendMessageW(tree, TVM_GETCOUNT, 0, 0) == 4,
                      "树里 4 个符号");
                // 顶层第一个是 helper；Widget 下面挂着 draw（层级 1）
                const HTREEITEM first = reinterpret_cast<HTREEITEM>(
                    ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
                Check(first != nullptr, "有顶层节点");
                Check(reinterpret_cast<HTREEITEM>(::SendMessageW(
                          tree, TVM_GETNEXTITEM, TVGN_CHILD,
                          reinterpret_cast<LPARAM>(first))) == nullptr,
                      "顶层第一个符号没有子节点");
                const HTREEITEM widget = reinterpret_cast<HTREEITEM>(::SendMessageW(
                    tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(first)));
                const HTREEITEM draw = reinterpret_cast<HTREEITEM>(::SendMessageW(
                    tree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(widget)));
                Check(draw != nullptr, "Widget 下面挂着 draw（层级关系建对了）");

                // 选中 draw（层级 1，先展开）→ 通知里应当带着它的行号
                ::SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(widget));
                ::SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(draw));
                Check(panel.SelectedLine() == 8, "选中项的选中行号读得回来");
                g_functionNotifyLine = -1;
                g_functionNotifyCount = 0;
                panel.ActivateSelection();
                Check(g_functionNotifyCount == 1 && g_functionNotifyLine == 8,
                      "跳转通知带上了正确的行号");

                panel.SetSymbols({});
                Check(::SendMessageW(tree, TVM_GETCOUNT, 0, 0) == 0, "清空之后树也空了");
                ::DestroyWindow(host);
            }
        }
    }

    // ---------------------------------------------------------------- 文件夹工作区
    // 两半分开测：目录树模型是纯函数（造目录、比对结构与顺序）；面板是窗口层的活
    //（选中 → 向父窗口发通知），后者在**同进程**里驱动——跨进程传不了树节点的句柄，
    // 键盘导航那一路由界面回归负责。
    Section("文件夹工作区（目录树模型 + 面板通知）");
    {
        using npcc::core::BuildFolderTree;
        using npcc::core::FolderNode;
        using npcc::core::FolderTreeOptions;
        using npcc::core::ReadFolderChildren;

        npcc::infra::InitializePaths(true);
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "notepadcc_folder_test";
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base / L"sub" / L"deep", ec);
        WriteFileDirect((base / L"a.txt").wstring(), "a\n");
        WriteFileDirect((base / L"alpha.md").wstring(), "#a\n");
        WriteFileDirect((base / L"Zed.txt").wstring(), "z\n");
        WriteFileDirect((base / L"sub" / L"b.cpp").wstring(), "int main() {}\n");
        WriteFileDirect((base / L"sub" / L"deep" / L"c.txt").wstring(), "c\n");

        // 1) 建树：结构、顺序、目录优先
        {
            const FolderNode tree = BuildFolderTree(base.wstring());
            Check(!tree.Empty() && tree.isFolder, "根节点就是一个目录");
            Check(tree.children.size() == 4, "根下有 4 个子项（1 个目录 + 3 个文件）");
            Check(tree.children[0].name == L"sub" && tree.children[0].isFolder,
                  "目录排在最前");
            Check(tree.children[1].name == L"a.txt" && tree.children[2].name == L"alpha.md" &&
                      tree.children[3].name == L"Zed.txt",
                  "文件按名字不区分大小写排序（Zed 排在最后）");
            Check(tree.children[0].children.size() == 2,
                  "子目录里也有内容（目录优先：deep 在 b.cpp 之前）");
            Check(tree.children[0].children[0].name == L"deep" &&
                      tree.children[0].children[1].name == L"b.cpp",
                  "深层目录的顺序也对");
            Check(tree.children[0].children[0].children.size() == 1 &&
                      tree.children[0].children[0].children[0].name == L"c.txt",
                  "第三层也读到了");
            Check(tree.children[1].path == (base / L"a.txt").wstring(), "每个节点带完整路径");
        }

        // 2) 深度与子项数上限：超大目录不该把界面拖死
        {
            FolderTreeOptions shallow;
            shallow.maxDepth = 1;
            const FolderNode limited = BuildFolderTree(base.wstring(), shallow);
            Check(limited.children.size() == 4, "深度 1 时根的子项照常列出");
            Check(limited.children[0].children.empty(), "但下钻被上限拦住了");

            FolderTreeOptions few;
            few.maxChildren = 2;
            const FolderNode capped = BuildFolderTree(base.wstring(), few);
            Check(capped.children.size() == 2, "子项数上限生效");
        }

        // 3) 读一层（按需展开用）与"目录不存在"的边界
        {
            const std::vector<FolderNode> children = ReadFolderChildren(base.wstring());
            Check(children.size() == 4, "单独读一层拿到 4 项");
            Check(ReadFolderChildren((base / L"并不存在").wstring()).empty(),
                  "目录不存在时返回空，不抛异常");
            Check(BuildFolderTree(L"").Empty(), "空路径返回空节点");
            Check(BuildFolderTree((base / L"a.txt").wstring()).Empty(),
                  "给的是文件而不是目录时也返回空");
        }

        // 4) 面板：把目录挂上去，树里真的有那么多节点
        {
            const HWND host = CreateHiddenHost();
            if (host == nullptr) {
                Check(false, "创建隐藏宿主窗口失败");
            } else {
                Check(npcc::shell::FolderPanel::RegisterWindowClass(::GetModuleHandleW(nullptr)),
                      "注册面板窗口类");
                npcc::shell::FolderPanel panel;
                Check(panel.Create(host, 900), "创建面板");
                panel.SetRoot(base.wstring());
                Check(panel.Root() == base.wstring(), "根目录记住了");
                const HWND tree = panel.Tree();
                Check(tree != nullptr, "树控件在");
                // 根 + 4 个子项 + sub 下的 2 项 + deep 下的 1 项 = 8
                Check(tree != nullptr && ::SendMessageW(tree, TVM_GETCOUNT, 0, 0) == 8,
                      "树里一共 8 个节点（根 + 4 + 2 + 1）");
                // 选中第 1 个子项（目录 sub）：只展开，不发通知；
                // 再选它下面的 b.cpp：发一条"打开这个文件"的通知给父窗口
                const HWND notifyHost = CreateNotifyHost();
                if (notifyHost != nullptr) {
                    ::DestroyWindow(host);
                    Check(npcc::shell::FolderPanel::RegisterWindowClass(
                              ::GetModuleHandleW(nullptr)),
                          "注册面板窗口类（复用）");
                    panel.Destroy();
                    Check(panel.Create(notifyHost, 901), "在新宿主上重建面板");
                    panel.SetRoot(base.wstring());
                    const HWND newTree = panel.Tree();
                    const HTREEITEM rootItem = reinterpret_cast<HTREEITEM>(
                        ::SendMessageW(newTree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
                    const HTREEITEM subItem = reinterpret_cast<HTREEITEM>(::SendMessageW(
                        newTree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(rootItem)));
                    Check(subItem != nullptr, "找得到根的第一个子项");
                    ::SendMessageW(newTree, TVM_SELECTITEM, TVGN_CARET,
                                   reinterpret_cast<LPARAM>(subItem));
                    g_folderNotifyCount = 0;
                    panel.ActivateSelection();
                    Check(g_folderNotifyCount == 0, "选中的是目录：只展开，不发打开通知");
                    // 子目录里第一个子项是它自己的子目录（目录仍然排在前），
                    // 所以要往后挪一格才是 b.cpp
                    const HTREEITEM firstOfSub = reinterpret_cast<HTREEITEM>(::SendMessageW(
                        newTree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(subItem)));
                    const HTREEITEM fileItem = reinterpret_cast<HTREEITEM>(::SendMessageW(
                        newTree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(firstOfSub)));
                    Check(fileItem != nullptr, "找得到子目录里的文件项");
                    // 先展开子目录：折叠着的子项没法被选中（控件会拒绝）
                    ::SendMessageW(newTree, TVM_EXPAND, TVE_EXPAND,
                                   reinterpret_cast<LPARAM>(subItem));
                    ::SendMessageW(newTree, TVM_SELECTITEM, TVGN_CARET,
                                   reinterpret_cast<LPARAM>(fileItem));
                    panel.ActivateSelection();
                    Check(g_folderNotifyCount == 1, "选中的是文件：发出了一条打开通知");
                    Check(g_lastFolderNotifyPath == (base / L"sub" / L"b.cpp").wstring(),
                          "通知里的路径就是那个文件");
                    ::DestroyWindow(notifyHost);
                }
                panel.SetRoot(L"");
                const HWND liveTree = panel.Tree();
                Check(liveTree != nullptr && ::SendMessageW(liveTree, TVM_GETCOUNT, 0, 0) == 0,
                      "清空根目录之后树也空了");
            }
        }

        std::filesystem::remove_all(base, ec);
    }

    Section("崩溃快照（标识映射 / 文件格式 / 落盘与清理）");
    {
        using npcc::core::SnapshotInfo;
        using npcc::core::SnapshotStore;

        // ---- 编码与行尾标识：只有这一份映射，两个方向都必须对得上 ----
        // 崩溃抢救曾经在这里踩过：处理器里按"数字"重新抄了一份映射，抄反了
        // Utf8 与 Utf8Bom，恢复出来的文件会莫名多一个 BOM。所以先钉住这一层。
        {
            const npcc::edit::UniMode modes[] = {
                npcc::edit::UniMode::Ansi,    npcc::edit::UniMode::Utf8,
                npcc::edit::UniMode::Utf8Bom, npcc::edit::UniMode::Utf16LE,
                npcc::edit::UniMode::Utf16BE};
            bool allModes = true;
            for (const npcc::edit::UniMode mode : modes) {
                npcc::edit::UniMode back = npcc::edit::UniMode::Ansi;
                if (!npcc::edit::ParseUniModeToken(npcc::edit::UniModeToken(mode), back) ||
                    back != mode) {
                    allModes = false;
                }
            }
            Check(allModes, "UniMode 标识往返一致");
            Check(std::string(npcc::edit::UniModeToken(npcc::edit::UniMode::Utf8)) !=
                      std::string(npcc::edit::UniModeToken(npcc::edit::UniMode::Utf8Bom)),
                  "Utf8 与 Utf8Bom 的标识必须不同");

            const npcc::edit::EolMode eols[] = {
                npcc::edit::EolMode::Crlf, npcc::edit::EolMode::Lf,
                npcc::edit::EolMode::Cr,   npcc::edit::EolMode::Mixed};
            bool allEols = true;
            for (const npcc::edit::EolMode eol : eols) {
                npcc::edit::EolMode back = npcc::edit::EolMode::Crlf;
                if (!npcc::edit::ParseEolToken(npcc::edit::EolToken(eol), back) || back != eol) {
                    allEols = false;
                }
            }
            Check(allEols, "EolMode 标识往返一致");

            npcc::edit::UniMode mode = npcc::edit::UniMode::Utf8;
            Check(!npcc::edit::ParseUniModeToken("Utf7", mode), "认不出的编码标识返回 false");
            npcc::edit::EolMode eol = npcc::edit::EolMode::Crlf;
            Check(!npcc::edit::ParseEolToken("crlf", eol), "标识区分大小写（crlf 不是合法标识）");
        }

        // ---- 快照文件格式 ----
        {
            SnapshotInfo info;
            info.originalPath = L"D:\\work\\a.cpp";
            info.displayName = L"a.cpp";
            info.encoding.uniMode = npcc::edit::UniMode::Utf8Bom;
            info.encoding.codePage = 65001;
            info.encoding.hasBom = true;
            info.eol = npcc::edit::EolMode::Lf;
            info.untitled = false;
            info.savedAt = L"2026-10-15 12:00:00";
            const std::string content("line1\r\nline2\n\0tail", 19);

            const std::string raw = SnapshotStore::Build(info, content);
            SnapshotInfo parsed;
            std::string bytes;
            Check(SnapshotStore::Parse(raw, parsed, bytes), "快照能解析回来");
            CheckEqual(bytes, content, "内容逐字节一致（含 CRLF 与 NUL）");
            CheckWide(parsed.originalPath, info.originalPath, "路径一致");
            CheckWide(parsed.displayName, info.displayName, "展示名一致");
            Check(parsed.encoding.uniMode == npcc::edit::UniMode::Utf8Bom, "编码一致");
            Check(parsed.encoding.hasBom, "BOM 标记一致");
            Check(parsed.eol == npcc::edit::EolMode::Lf, "行尾一致");
            Check(!parsed.untitled, "未命名标记一致");
            Check(parsed.length == content.size(), "长度一致");
            CheckWide(parsed.savedAt, L"2026-10-15 12:00:00", "时间一致");

            Check(!SnapshotStore::Parse(raw.substr(0, raw.size() - 3), parsed, bytes),
                  "内容被截断要拒绝");
            Check(!SnapshotStore::Parse(std::string("not-a-snapshot\r\n\r\nxx"), parsed, bytes),
                  "头部标识不对要拒绝");
            Check(!SnapshotStore::Parse(std::string("notepadcc-snapshot 1\r\nname=a\r\n"),
                                        parsed, bytes),
                  "头部没有以空行收尾要拒绝");

            std::string unknownKey = raw;
            unknownKey.replace(unknownKey.find("uniMode="), 8, "uniModf=");
            Check(SnapshotStore::Parse(unknownKey, parsed, bytes),
                  "不认识的键忽略即可（以后加字段要向前兼容）");

            std::string badMode = raw;
            badMode.replace(badMode.find("Utf8Bom"), 7, "Utf7Bom");
            Check(!SnapshotStore::Parse(badMode, parsed, bytes), "认不出的编码标识要拒绝");
        }

        // ---- 落盘、扫描、清理 ----
        {
            npcc::infra::InitializePaths(true);
            const SnapshotStore store;
            std::error_code ec;
            std::filesystem::remove_all(SnapshotStore::BackupRoot(), ec);

            SnapshotInfo info;
            info.file = SnapshotStore::AutosaveDir() / L"doc_1.snap";
            info.displayName = L"未命名 1";
            info.untitled = true;
            info.savedAt = L"2026-10-15 10:00:00";
            std::wstring error;
            Check(store.Write(info, "hello", &error), "写快照成功");
            Check(std::filesystem::exists(info.file), "快照文件已落盘");
            Check(std::filesystem::exists(SnapshotStore::AutosaveDir()), "自动快照目录已建立");

            const std::vector<SnapshotInfo> listed = store.List();
            Check(listed.size() == 1, "扫描到 1 份快照");
            Check(!listed.empty() && listed[0].displayName == L"未命名 1", "扫描出的展示名正确");
            Check(!listed.empty() && listed[0].untitled, "扫描出的未命名标记正确");
            Check(!listed.empty() && listed[0].length == 5, "扫描出的长度正确");

            std::string bytes;
            Check(store.Read(listed.front(), bytes, &error) && bytes == "hello",
                  "按扫描结果读回内容一致");

            // 崩溃目录里的快照也要被扫到，且新的排在前面
            const std::filesystem::path crashDir = SnapshotStore::CrashDirFor(L"20261015-120000");
            std::filesystem::create_directories(crashDir, ec);
            SnapshotInfo crashInfo = info;
            crashInfo.file = crashDir / L"0.snap";
            crashInfo.savedAt = L"2026-10-15 12:00:00";
            Check(store.Write(crashInfo, "from-crash", &error), "写崩溃抢救内容成功");
            const std::vector<SnapshotInfo> both = store.List();
            Check(both.size() == 2, "崩溃目录里的快照同样会被扫到");
            Check(both.size() == 2 && both[0].savedAt > both[1].savedAt, "新的排在前面");

            // 坏文件只跳过，不影响其余快照
            WriteFileDirect(SnapshotStore::AutosaveDir() / L"doc_9.snap", "garbage");
            Check(store.List().size() == 2, "解析不了的快照被跳过而不是让整次扫描失败");

            Check(store.Remove(crashInfo), "删除指定快照");
            Check(store.List().size() == 1, "删除后只剩 1 份");

            for (int i = 0; i < 5; ++i) {
                std::filesystem::create_directories(
                    SnapshotStore::CrashDirFor(std::to_wstring(20261010 + i)), ec);
            }
            store.PruneCrashDirs(3);
            int dirCount = 0;
            for (const auto& entry :
                 std::filesystem::directory_iterator(SnapshotStore::BackupRoot(), ec)) {
                const std::wstring name = entry.path().filename().wstring();
                if (name.rfind(L"crash_", 0) == 0) {
                    ++dirCount;
                }
            }
            Check(dirCount == 3, "崩溃目录只保留最近 3 个");

            store.RemoveAllAutosave();
            Check(!std::filesystem::exists(SnapshotStore::AutosaveDir()), "清空自动快照目录");
            std::filesystem::remove_all(SnapshotStore::BackupRoot(), ec);
        }
    }

    // ---------------------------------------------------------------- 代码折叠
    // 折叠层级是 Lexilla 算的、折叠状态是 Scintilla 存的，我们能验的是
    // "开关给对了没有、层级读得对不对、折起来之后可见行是不是真的少了"。
    Section("代码折叠（层级 / 开关 / 存取）");
    {
        const HWND host = CreateHiddenHost();
        if (host == nullptr) {
            Check(false, "创建隐藏宿主窗口失败");
        } else {
            npcc::edit::EditView view;
            if (!view.Create(host, 900)) {
                Check(false, "创建编辑控件失败");
            } else {
                view.ApplyLightTheme();
                view.SetCodePage(npcc::edit::kCodePageUtf8);
                view.SetText("int a() {\n  x;\n}\nint b() {\n  y;\n}\n");
                view.ApplyLanguageForPath(L"D:\\t\\sample.cpp");
                view.ClearUndoBuffer();
                view.MarkSavePoint();

                Check(view.FoldingEnabled(), "C++ 文件启用折叠");
                Check(view.LineIsFoldHeader(0), "第 0 行被算成折叠头");
                Check(view.LineIsFoldHeader(3), "第 3 行被算成折叠头");
                Check(!view.LineIsFoldHeader(1), "块内部的行不是折叠头");

                const int32_t allVisible = view.VisibleLineCount();
                Check(allVisible == view.LineCount(), "未折叠时可见行数等于总行数");

                view.FoldAll();
                Check(view.VisibleLineCount() < allVisible, "折叠全部之后可见行变少");
                Check(view.IsLineFolded(0), "第 0 行被折起来了");
                Check(view.IsLineFolded(3), "第 3 行被折起来了");

                const std::vector<int32_t> folded = view.FoldedLines();
                Check(folded.size() == 2, "收集到 2 处折叠");
                Check(folded.empty() || folded[0] == 0, "收集到的第一处是第 0 行");

                view.UnfoldAll();
                Check(view.VisibleLineCount() == allVisible, "展开全部回到原来的可见行数");
                Check(view.FoldedLines().empty(), "展开之后收集不到折叠");

                view.RestoreFoldedLines(folded);
                Check(view.VisibleLineCount() < allVisible, "按收集到的行号能恢复折叠");
                Check(view.IsLineFolded(3), "恢复后第 3 行仍是折起来的");
                view.UnfoldAll();

                // 光标在块内部时，快捷键应当折起包住它的那一层
                view.SetCaretPosition(static_cast<int32_t>(view.PositionFromLine(1)));
                view.ToggleFoldAtCaret();
                Check(view.IsLineFolded(0), "在块内部折叠的是包住它的那一层");
                view.ToggleFoldAtCaret();
                Check(!view.IsLineFolded(0), "再按一次展开回来");

                // 顶行往上找不到折叠头时不能越界
                view.SetCaretPosition(0);
                view.ToggleFoldAtCaret();
                Check(view.LineIsFoldHeader(0), "顶行仍可折叠（没有越界）");
                view.UnfoldAll();

                // 普通文本没有折叠可言，边距要收起来
                view.ApplyLanguageForPath(L"D:\\t\\notes.txt");
                Check(!view.FoldingEnabled(), "普通文本不启用折叠");
                view.ApplyLanguageForPath(L"D:\\t\\sample.cpp");
                Check(view.FoldingEnabled(), "切回 C++ 又启用折叠");
                view.FoldAll();
                view.ApplyLargeFileDisplayMode();
                Check(!view.FoldingEnabled(), "大文件档关闭折叠");
                Check(view.VisibleLineCount() == allVisible, "关掉折叠时会把已折的行展开");
            }
            ::DestroyWindow(host);
        }
    }

    // ---------------------------------------------------------------- 分屏与克隆视图
    // 这一段的重点是把 ADR-07 的假设**实测**一遍：共享文档到底共享到什么程度、
    // "只读"是按视图还是按文档、销毁克隆视图会不会连带影响原视图。
    Section("分屏与克隆视图（共享文档 / 引用计数 / 只读范围）");
    {
        const HWND host = CreateHiddenHost();
        if (host == nullptr) {
            Check(false, "创建隐藏宿主窗口失败");
        } else {
            npcc::edit::EditView primary;
            if (!primary.Create(host, 910)) {
                Check(false, "创建主视图失败");
            } else {
                primary.ApplyLightTheme();
                primary.SetCodePage(npcc::edit::kCodePageUtf8);
                primary.SetText("alpha\nbeta\n");

                {
                    npcc::edit::EditView clone;
                    Check(clone.Create(host, 911), "创建克隆视图");
                    clone.ApplyLightTheme();
                    Check(clone.ShareDocumentWith(primary), "克隆视图接上同一份文档");
                    CheckEqual(clone.GetAllText(), "alpha\nbeta\n",
                               "克隆视图立刻显示同一份内容");

                    primary.InsertTextAt(0, "X");
                    CheckEqual(clone.GetAllText(), "Xalpha\nbeta\n",
                               "主视图编辑后克隆视图跟着变（共享的是同一份文档）");
                    clone.InsertTextAt(0, "Y");
                    CheckEqual(primary.GetAllText(), "YXalpha\nbeta\n",
                               "反方向也一样：克隆视图能写，主视图立刻可见");

                    // 光标与滚动是各视图自己的
                    primary.SetCaretPosition(0);
                    clone.SetCaretPosition(3);
                    Check(primary.CaretPosition() == 0, "主视图光标不受影响");
                    Check(clone.CaretPosition() == 3, "克隆视图有自己的光标");
                    primary.SetFirstVisibleLine(0);
                    clone.SetFirstVisibleLine(1);
                    Check(primary.FirstVisibleLine() == 0 && clone.FirstVisibleLine() == 1,
                          "两个视图各有一份滚动位置");

                    // 折叠层级与标记存在文档里，所以两边看到的是同一份
                    primary.ApplyLanguageForPath(L"D:\\t\\clone.cpp");
                    Check(primary.FoldingEnabled(), "主视图按语言启用折叠");

                    // ADR-07 原本打算用 SCI_SETREADONLY 把克隆视图设成只读。
                    // 实测：Scintilla 的只读是**文档级**开关，设到克隆视图上会把
                    // 主视图一起锁住。所以最后没有采用它，这条断言把这个结论钉住。
                    const int32_t beforeReadOnly = primary.ByteLength();
                    clone.SetReadOnly(true);
                    primary.InsertTextAt(0, "Z");
                    Check(primary.ByteLength() == beforeReadOnly,
                          "给克隆视图设只读会连带锁住主视图（只读是文档级，不是视图级）");
                    clone.SetReadOnly(false);
                    primary.InsertTextAt(0, "Z");
                    Check(primary.ByteLength() == beforeReadOnly + 1, "取消只读后主视图恢复可写");
                }

                // 克隆视图销毁之后，原文档必须还在、还能编辑（引用计数没被弄坏）
                Check(primary.Valid(), "销毁克隆视图后主视图仍然存在");
                const int32_t beforeDestroy = primary.ByteLength();
                primary.InsertTextAt(0, "W");
                Check(primary.ByteLength() == beforeDestroy + 1,
                      "销毁克隆视图后主视图仍可编辑");
                CheckEqual(primary.GetAllText().substr(0, 3), "WZY", "内容完好");
            }
            ::DestroyWindow(host);
        }
    }

    // ---------------------------------------------------------------- 语法着色
    // 光"接上词法器"不等于"用户看得见颜色"：颜色要靠把样式号与颜色对应上。
    // 这一段按**真实 token 的位置**核对：那个位置的样式号不是默认样式，
    // 且它的前景色与默认前景色不同。样式号抄错、或者词法器名字写错（本版真发生过：
    // Lexilla 里没有 "javascript" 词法器，JS 文件一直没有任何样式），都会被它抓住。
    // ---------------------------------------------------------------- 编码转换
    // 转换是不可逆的高风险操作，逻辑必须能单独测：能不能转、转出来的字节对不对、
    // 表示不了的字符有没有被**发现**（而不是静默变成 '?'）。
    // ---------------------------------------------------------------- 单实例交接
    // 两个进程之间的交接只能靠一段自定格式的字节流，所以至少把打包/解析单独测了：
    // 路径里有空格、中文、反斜杠都不能出错。
    // ---------------------------------------------------------------- 文件关联
    // 这一段只写 HKCU\Software\notepadcc-selftest 这个临时键，跑完就删，
    // **不会碰机器上真实的关联**。注册表操作要是测不了，就只能靠"点了按钮看着像好了"，
    // 而关联这种东西恰恰是最容易悄悄写错、用户过几天才发现的地方。
    // ---------------------------------------------------------------- 行操作与文本转换
    // 纯函数，所以能测得很细。两条贯穿全部行操作的约定必须钉住：
    // 保留原文的行尾风格、保留结尾换行与否——这类细节用户一眼就能看出来。
    Section("行操作与文本转换（TextTools）");
    {
        using namespace npcc::edit;

        // 行尾探测与结尾换行的保留
        {
            const LineSplit crlf = SplitLines("a\r\nb\r\n");
            Check(crlf.eol == "\r\n" && crlf.trailingEol && crlf.lines.size() == 2,
                  "CRLF 文本识别出行尾与结尾换行");
            const LineSplit lf = SplitLines("a\nb");
            Check(lf.eol == "\n" && !lf.trailingEol && lf.lines.size() == 2,
                  "LF 文本没有结尾换行");
            const LineSplit cr = SplitLines("a\rb\r");
            Check(cr.eol == "\r" && cr.lines.size() == 2, "老式 CR 行尾也能识别");
            Check(SplitLines("").lines.empty(), "空文本没有行");
            Check(SplitLines("\n").lines.size() == 1, "只有一个换行的文本算一行");
        }

        // 排序：保留行尾风格，稳定排序不搅乱相同行
        {
            Check(SortLines("b\r\na\r\nc\r\n", SortOrder::Ascending, true) ==
                      "a\r\nb\r\nc\r\n",
                  "升序排序保留 CRLF 行尾");
            Check(SortLines("a\nc\nb", SortOrder::Descending, true) == "c\nb\na",
                  "降序排序保留「没有结尾换行」这一点");
            Check(SortLines("B\na\nC", SortOrder::Ascending, false) == "a\nB\nC",
                  "忽略大小写排序");
            Check(SortLines("b\nA\nc", SortOrder::Ascending, true) == "A\nb\nc",
                  "区分大小写时大写在前（ASCII 序）");
            Check(SortLines("bb\na\nccc", SortOrder::Ascending, true) == "a\nbb\nccc",
                  "按长度升序排序");
            Check(SortLinesByLength("bb\na\nccc", SortOrder::Descending) == "ccc\nbb\na",
                  "按长度降序排序");
        }

        // 删空行：只删真正空的行
        {
            Check(RemoveEmptyLines("a\n\nb\n") == "a\nb\n", "删除真正的空行");
            Check(RemoveEmptyLines("a\n   \nb\n") == "a\n   \nb\n",
                  "只有空白的行不算空行（与对标产品一致）");
        }

        // 去重：保留首次 / 保留最后一次
        {
            Check(RemoveDuplicateLines("a\nb\na\nc\n", false) == "a\nb\nc\n",
                  "去重保留首次出现");
            Check(RemoveDuplicateLines("a\nb\na\nc\n", true) == "b\na\nc\n",
                  "去重保留最后一次（位置也按最后一次算）");
            Check(RemoveDuplicateLines("x\nx\nx\n", false) == "x\n", "全同去重只剩一行");
        }

        // 反转与去空白
        {
            Check(ReverseLines("1\r\n2\r\n3") == "3\r\n2\r\n1", "反转行序保留行尾风格");
            Check(TrimTrailingWhitespace("a  \nb\t\t\n") == "a\nb\n", "去掉行尾空白");
            Check(TrimLeadingWhitespace("  a\n\tb\n") == "a\nb\n", "去掉行首空白");
            Check(TrimTrailingWhitespace("a   ") == "a", "最后一行没有换行时也去掉空白");
        }

        // 行号：加与去要能往返，且不能吃掉正文开头的数字
        {
            const std::string numbered = AddLineNumbers("x\ny\n", 1, 1, 0);
            Check(numbered == "1\tx\n2\ty\n", "加行号");
            Check(StripLineNumbers(numbered) == "x\ny\n", "去行号与加行号往返一致");
            Check(StripLineNumbers("42 是答案\n") == "是答案\n",
                  "去行号认得「数字 + 空格」这种形态");
            Check(StripLineNumbers("island\n") == "island\n",
                  "没有行首数字的行原样保留");
            Check(AddLineNumbers("a\nb\n", 10, 5, 3) == " 10\ta\n 15\tb\n",
                  "行号支持起始值、步长与补宽");
        }

        // 大小写：ASCII 与非 ASCII 都要对，中文这类没有大小写的字符原样保留
        {
            Check(ToUpperCase("abc", npcc::edit::kCodePageUtf8) == "ABC", "转大写");
            Check(ToLowerCase("ABC", npcc::edit::kCodePageUtf8) == "abc", "转小写");
            Check(ToUpperCase("caf\xC3\xA9", npcc::edit::kCodePageUtf8) == "CAF\xC3\x89",
                  "带重音的拉丁字母也按系统的大小写表转换");
            Check(ToUpperCase("\xE4\xB8\xAD\xE6\x96\x87", npcc::edit::kCodePageUtf8) ==
                      "\xE4\xB8\xAD\xE6\x96\x87",
                  "中文没有大小写，原样保留");
            Check(ToUpperCase("\xD6\xD0\xCE\xC4", 936) == "\xD6\xD0\xCE\xC4",
                  "ANSI(GBK) 缓冲区的中文同样原样保留");
        }

        // Base64：标准向量、往返、以及各种不合法输入
        {
            Check(Base64Encode("Man") == "TWFu", "Base64 标准向量 Man");
            Check(Base64Encode("Ma") == "TWE=", "Base64 补一个等号");
            Check(Base64Encode("M") == "TQ==", "Base64 补两个等号");
            Check(Base64Encode("") == "", "空内容编码为空");
            std::string out;
            Check(Base64Decode("TWFu", out) && out == "Man", "Base64 解码往返");
            std::string plain;
            Check(Base64Decode("VHd1", plain) && plain == "Twu", "Base64 三字节往返");
            Check(Base64Decode("VH d1\n", out) && out == plain,
                  "解码时忽略空格与换行");
            Check(!Base64Decode("TWF", out), "长度不是 4 的倍数要拒绝");
            Check(!Base64Decode("TW!u", out), "含非法字符要拒绝");
            Check(!Base64Decode("T=Fu", out), "填充位置不对要拒绝");
        }

        // URL：标准编码、往返、以及不完整的百分号
        {
            Check(UrlEncode("a b") == "a%20b", "空格编成 %20");
            Check(UrlEncode("\xE4\xB8\xAD\xE6\x96\x87") == "%E4%B8%AD%E6%96%87",
                  "中文按 UTF-8 字节做百分号编码");
            Check(UrlEncode("a-b_c.d~e") == "a-b_c.d~e", "未保留字符不动");
            std::string out;
            Check(UrlDecode("a%20b", out) && out == "a b", "URL 解码往返");
            Check(UrlDecode("%E4%B8%AD", out) && out == "\xE4\xB8\xAD", "多字节字符解码正确");
            Check(!UrlDecode("a%2", out), "百分号后面不足两位要拒绝");
            Check(!UrlDecode("a%zz", out), "百分号后面不是十六进制要拒绝");
            Check(UrlDecode("a+b", out) && out == "a+b", "'+' 保持原样（不做表单式转换）");
        }

        // HTML 实体：转义与还原、数字实体、不认识的实体原样保留
        {
            Check(EscapeHtml("<a href=\"x\">&'") == "&lt;a href=&quot;x&quot;&gt;&amp;&#39;",
                  "HTML 转义五个字符");
            std::string back = UnescapeHtml("&lt;a&gt;&amp;&quot;&#39;");
            Check(back == "<a>&\"'", "HTML 还原");
            Check(UnescapeHtml("&#20013;&#x6587;") == "\xE4\xB8\xAD\xE6\x96\x87",
                  "十进制与十六进制数字实体都能还原");
            Check(UnescapeHtml("&unknown;") == "&unknown;", "不认识的实体原样保留");
            Check(UnescapeHtml("a & b") == "a & b", "孤立的 & 不当实体处理");
            Check(EscapeHtml("") == "" && UnescapeHtml("") == "", "空串两头都不出错");
        }

        // 自动完成的候选词：去重、忽略大小写、按前缀筛、词库文件解析
        Section("自动完成候选词（CollectWords / ParseWordList / FilterByPrefix）");
        {
            const std::vector<std::string> words = CollectWords("alpha beta gamma alpha", 3, 100);
            Check(words.size() == 3, "文档提词去重");
            Check(words[0] == "alpha" && words[1] == "beta" && words[2] == "gamma",
                  "文档提词按字典序");
            Check(CollectWords("ab cd ef", 3, 100).empty(), "短词被过滤掉");
            Check(CollectWords("123 abc", 3, 100) == std::vector<std::string>{"abc"},
                  "纯数字不算词");
            Check(CollectWords("Alpha alpha ALPHA", 3, 100).size() == 1,
                  "忽略大小写去重（同一个词的三种写法算一个）");
            Check(CollectWords("Alpha alpha", 3, 100)[0] == "Alpha",
                  "去重时保留首次出现的写法");
            Check(CollectWords("_x1 foo", 2, 100).size() == 2, "下划线开头的也算词");
            Check(CollectWords("aaa bbb ccc", 3, 2).size() == 2, "候选数量受上限约束");
            Check(CollectWords("中文 测试", 2, 100).size() == 2, "中文也能当词（按字节走）");
            Check(CollectWords("alpha beta gamma", 3, 100, "al") ==
                      std::vector<std::string>{"alpha"},
                  "给了前缀就只收以它开头的词");
            Check(CollectWords("alpha beta gamma", 3, 100, "AL").size() == 1,
                  "前缀同样忽略大小写");

            // 大输入：必须"边收边停"，不能把整篇的词收完再截。这里造 2 万个各不相同的词，
            // 收 100 个就返回；同时前缀命中的词在文末也要找得到（说明确实扫到了末尾）。
            // 之前那版对同一个输入会跑成 O(词数 × 去重表大小)，5 MB 的文件能卡住界面 54 秒。
            std::string big;
            big.reserve(400000);
            for (int i = 0; i < 20000; ++i) {
                big += "identifier";
                big += std::to_string(i);
                big += " = 0;\n";
            }
            const std::vector<std::string> limited = CollectWords(big, 3, 100);
            Check(limited.size() == 100, "大输入下候选数量仍然受上限约束");
            Check(CollectWords(big, 3, 100, "identifier19999").size() == 1,
                  "前缀命中的词在文末也能提到（提词扫到了整篇）");

            const std::vector<std::string> parsed = ParseWordList("a b\n# 注释行\nc\n\n");
            Check(parsed.size() == 3 && parsed[0] == "a" && parsed[2] == "c",
                  "词库文件按空白分隔、# 行跳过");
            Check(ParseWordList("").empty(), "空词库解析出空列表");

            const std::vector<std::string> pool = {"Alpha", "beta", "gamma"};
            Check(FilterByPrefix(pool, "al", 10) == std::vector<std::string>{"Alpha"},
                  "按前缀筛选（忽略大小写）");
            Check(FilterByPrefix(pool, "", 2).size() == 2, "空前缀返回全部并受上限约束");
            Check(FilterByPrefix(pool, "zz", 10).empty(), "没有匹配返回空");
        }

        // 注释：加与取消要往返，缩进要保留，空行不要被动
        {
            Check(ToggleLineComment("a\nb\n", "//") == "// a\n// b\n", "加行注释");
            Check(ToggleLineComment(ToggleLineComment("a\nb\n", "//"), "//") == "a\nb\n",
                  "加注释再取消可往返");
            Check(ToggleLineComment("  a\n", "//") == "  // a\n", "注释符插在缩进之后");
            Check(ToggleLineComment("a\n\nb\n", "//") == "// a\n\n// b\n", "空行不被动");
            Check(ToggleLineComment("// a\n// b\n", "//") == "a\nb\n", "全部注释了才取消");
            Check(ToggleLineComment("//a\nb\n", "//") == "// //a\n// b\n",
                  "只有部分行有注释时统一加上（而不是取消）");
            Check(ToggleLineComment("rem a\n", "REM ") == "a\n", "批处理注释前缀忽略大小写");
            Check(ToggleLineComment("a\n", "") == "a\n", "没有行注释符时原样返回");

            Check(ToggleBlockComment("x", "/*", "*/") == "/*x*/", "加块注释");
            Check(ToggleBlockComment("/* x */", "/*", "*/") == "x",
                  "取消块注释（连两头的空白一起去掉）");
            Check(ToggleBlockComment("/*x*/", "/*", "*/") == "x", "取消块注释（没有多余空白）");
            Check(ToggleBlockComment("a\nb", "<!--", "-->") == "<!--a\nb-->", "多行整段包住");
            Check(ToggleBlockComment("x", "", "") == "x", "没有块注释符时原样返回");
        }

        // 括号匹配（FindBraceMatch）：断言里出现的是确切的字节位置，不是"有没有找到"。
        // 结果压成三种形状：`5->7`（配上了）/ `5+`（有括号但没配上）/ `-`（光标旁没有括号）
        {
            const auto shape = [](const BraceMatch& match) {
                if (!match.hasBrace) {
                    return std::string("-");
                }
                return std::to_string(match.bracePos) +
                       (match.matched ? "->" + std::to_string(match.matchPos) : std::string("+"));
            };

            // 0:i 1:n 2:t 3:' ' 4:a 5:[ 6:3 7:] 8:' ' 9:= 10:' ' 11:{ 12:' ' 13:f 14:( …
            const std::string code = "int a[3] = { f(1), g(2) };\n";
            Check(shape(FindBraceMatch(code, 0)) == "-", "光标在 'i' 后面：左右都不是括号");
            Check(shape(FindBraceMatch(code, 5)) == "5->7", "光标正踩在 '[' 上");
            Check(shape(FindBraceMatch(code, 8)) == "7->5",
                  "光标在 ']' 后面：右边不是括号，才看左边");
            Check(shape(FindBraceMatch(code, 14)) == "14->16", "括号里套括号：最近的这一对");
            Check(shape(FindBraceMatch(code, 11)) == "11->24",
                  "跨过里面的圆括号找花括号（只数同一种括号）");
            Check(shape(FindBraceMatch(code, 23)) == "22->20", "光标在 ')' 后面：往左看");

            // 光标右边是括号、左边也是括号时**右边优先**：刚敲完 ')' 时插入点就在它后面，
            // 用户期望高亮的是刚敲进去的那一个（与对标产品一致）
            const std::string adjacent = "([)]";
            Check(shape(FindBraceMatch(adjacent, 1)) == "1->3", "光标踩在 '[' 上：配 ']'");
            Check(shape(FindBraceMatch(adjacent, 2)) == "2->0",
                  "右边优先：光标后面是 ')'，就不去管左边那个 '['");
            Check(shape(FindBraceMatch(adjacent, 3)) == "3->1", "光标踩在 ']' 上：配 '['");
            // 不做语法感知的嵌套判断——"([)]" 里 '(' 就配到第 2 个字节的 ')'（横跨了 '['）
            Check(shape(FindBraceMatch(adjacent, 0)) == "0->2", "只数同一种括号：'(' 找 ')'");

            // 配不上与边界
            Check(shape(FindBraceMatch("f(", 2)) == "1+", "右括号没来：有括号、没配上");
            Check(shape(FindBraceMatch(")f", 0)) == "0+", "左括号没来：有括号、没配上");
            Check(shape(FindBraceMatch("((x))", 0)) == "0->4", "同种括号嵌套按深度配对");
            Check(shape(FindBraceMatch("", 0)) == "-", "空文本");
            Check(shape(FindBraceMatch("ab", 5)) == "-", "光标位置越界");
            Check(shape(FindBraceMatch("(a)", 1)) == "0->2", "光标夹在括号中间：看左边那个 '('");
            Check(shape(FindBraceMatch("(a)", 3)) == "2->0",
                  "光标停在文末（位置 == 长度）：右边没字符，往左配上 ')'");

            // 窗口：配对括号在窗口之外就按"没配上"处理（整篇扫描会把大文件的界面拖死）
            std::string spread = "(";
            spread.append(100, 'a');
            spread += ")";
            Check(shape(FindBraceMatch(spread, 1, 10)) == "0+",
                  "窗口 10 字节：100 字节外的 ')' 不算");
            Check(shape(FindBraceMatch(spread, 1, 200)) == "0->101", "窗口放开就配得上");
        }

        // 文档统计（StatsOf）：四个数逐个钉死，按 "行/字符/词/字节" 压成一行比对
        {
            const auto shape = [](const DocumentStats& stats) {
                return std::to_string(stats.lines) + "/" + std::to_string(stats.chars) + "/" +
                       std::to_string(stats.words) + "/" + std::to_string(stats.bytes);
            };

            Check(shape(StatsOf("", false)) == "1/0/0/0", "空文档也是 1 行（与编辑器口径一致）");
            Check(shape(StatsOf("a\nb", false)) == "2/3/2/3",
                  "没有结尾换行：两行；换行也算一个字符（所以字符数与字节数相等）");
            Check(shape(StatsOf("a\r\nb\r\n", false)) == "3/6/2/6",
                  "CRLF 是一次换行（行数 3），但两个字节都是字符（字符数 6）");
            Check(shape(StatsOf("a\rb\nc", false)) == "3/5/3/5", "CR / LF 各算一次换行");
            Check(shape(StatsOf("a\tb\vc\fd", false)) == "1/7/4/7",
                  "制表符/垂直制表/换页都算空白，不作词");
            Check(shape(StatsOf("中文 A\n", false)) == "2/5/2/9",
                  "UTF-8：一个汉字算一个字符（3 个字节），末尾换行也算一个字符");
            Check(shape(StatsOf("中文 A\n", true)) == "2/9/2/9",
                  "ANSI（代码页）文档：一个字节就是一个字符");
            Check(shape(StatsOf("中文，汉字", false)) == "1/5/1/15",
                  "中文标点不算空白：整句是一个词（口径写在头文件里）");
            Check(!StatsOf("a", false).truncated,
                  "纯函数不知道截断这件事，truncated 由调用方置位");
        }
    }

    // ---------------------------------------------------------------- 语言定义
    // 语言定义是"用什么词法器、关键字是什么、注释符是什么"的权威来源。它现在可以
    // 被用户的 langs.xml 覆盖，所以这一段要同时钉住"内置表可用的默认值"与
    // "文件里的覆盖确实生效、没提到的语言仍用内置值"。
    // ---------------------------------------------------------------- 样式方案
    // 配色是"按语义角色"给的：用户想调的是"注释用什么颜色"，而不是 Lexilla 的样式号。
    // 这一段既测数据层（覆盖、往返、坏值），也测**它真的改变渲染出来的颜色**——
    // 只测"存取对"是不够的，配色的意义在于屏幕上看得见。
    Section("样式方案（stylers.xml）");
    {
        using npcc::edit::StyleRole;
        using npcc::edit::StyleScheme;

        npcc::infra::InitializePaths(true);
        {
            const StyleScheme defaults = StyleScheme::Defaults();
            Check(defaults.Role(L"cpp", StyleRole::Comment).fore == 0x2E7D32,
                  "默认的注释色是绿");
            Check(defaults.Role(L"cpp", StyleRole::Comment).italic, "默认注释是斜体");
            Check(defaults.Role(L"cpp", StyleRole::Keyword).bold, "默认关键字是粗体");
            Check(defaults.Role(L"还没见过的语言", StyleRole::Keyword).fore ==
                      defaults.Role(L"cpp", StyleRole::Keyword).fore,
                  "没被改过的语言用同一套默认配色");
            Check(defaults.Fore() == 0x1E1E1E && defaults.Back() == 0xFFFFFF, "默认前景/背景");
        }

        {
            StyleScheme scheme = StyleScheme::Defaults();
            Check(!scheme.HasOverride(L"cpp"), "初始没有覆盖");
            npcc::edit::RoleStyle custom;
            custom.fore = 0xFF00FF;
            custom.bold = false;
            custom.italic = false;
            scheme.SetRole(L"cpp", StyleRole::Comment, custom);
            Check(scheme.HasOverride(L"cpp"), "设置之后有覆盖");
            Check(scheme.Role(L"cpp", StyleRole::Comment).fore == 0xFF00FF, "覆盖生效");
            Check(scheme.Role(L"python", StyleRole::Comment).fore == 0x2E7D32,
                  "只影响指定的语言");
            Check(scheme.Role(L"cpp", StyleRole::Keyword).fore == 0x0000C0,
                  "同一语言里没改过的角色仍是默认值");
            scheme.ResetLanguage(L"cpp");
            Check(!scheme.HasOverride(L"cpp") &&
                      scheme.Role(L"cpp", StyleRole::Comment).fore == 0x2E7D32,
                  "恢复默认之后回到内置配色");
        }

        {
            std::uint32_t colour = 0;
            Check(StyleScheme::ParseColour(L"#2E7D32", colour) && colour == 0x2E7D32,
                  "解析 #RRGGBB");
            Check(StyleScheme::ParseColour(L"2e7d32", colour) && colour == 0x2E7D32,
                  "不带 # 与非大写也能解析");
            Check(!StyleScheme::ParseColour(L"#123", colour), "位数不对要拒绝");
            Check(!StyleScheme::ParseColour(L"#GGGGGG", colour), "非十六进制要拒绝");
            Check(!StyleScheme::ParseColour(L"", colour), "空串要拒绝");
            Check(StyleScheme::ColourText(0x0A0B0C) == L"#0A0B0C", "颜色转文本");
        }

        // 存取往返
        {
            const std::filesystem::path path = StyleScheme::Path();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            StyleScheme scheme = StyleScheme::Defaults();
            npcc::edit::RoleStyle custom;
            custom.fore = 0x123456;
            custom.bold = true;
            custom.italic = true;
            scheme.SetRole(L"cpp", StyleRole::Number, custom);
            scheme.SetDefault(0x101010, 0xF0F0F0, L"Consolas", 13);
            std::wstring error;
            Check(scheme.Save(&error), "保存样式方案成功");

            StyleScheme reloaded;
            Check(reloaded.Load(&error), "重新加载样式方案成功");
            Check(reloaded.Role(L"cpp", StyleRole::Number).fore == 0x123456 &&
                      reloaded.Role(L"cpp", StyleRole::Number).bold &&
                      reloaded.Role(L"cpp", StyleRole::Number).italic,
                  "覆盖过的角色往返一致（含粗体斜体）");
            Check(reloaded.Fore() == 0x101010 && reloaded.Back() == 0xF0F0F0 &&
                      reloaded.FontSize() == 13,
                  "默认前景/背景/字号往返一致");
            Check(reloaded.Role(L"cpp", StyleRole::Comment).fore == 0x2E7D32,
                  "没覆盖的角色仍是默认值");

            // 坏值只跳过并记警告，不影响其余部分
            const std::string custom2 =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadccStyles schemaVersion=\"1\">\n"
                "  <Language name=\"cpp\">\n"
                "    <Style role=\"没这个角色\" fore=\"#112233\"/>\n"
                "    <Style role=\"keyword\" fore=\"不是颜色\"/>\n"
                "    <Style role=\"string\" fore=\"#445566\" bold=\"yes\"/>\n"
                "  </Language>\n"
                "</NotepadccStyles>\n";
            Check(WriteFileDirect(path, custom2), "写入带坏值的 stylers.xml");
            StyleScheme tolerant;
            std::vector<std::wstring> warnings;
            Check(tolerant.Load(&error, &warnings), "带坏值的文件仍能加载");
            Check(warnings.size() >= 2, "认不出的角色与坏颜色都给出了警告");
            Check(tolerant.Role(L"cpp", StyleRole::String).fore == 0x445566 &&
                      tolerant.Role(L"cpp", StyleRole::String).bold,
                  "同一份文件里合法的条目照常生效");
            Check(tolerant.Role(L"cpp", StyleRole::Keyword).fore == 0x0000C0,
                  "坏值回退到默认配色");

            const std::string future =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadccStyles schemaVersion=\"99\"/>\n";
            Check(WriteFileDirect(path, future), "写入高版本 stylers.xml");
            StyleScheme rejected;
            Check(!rejected.Load(&error), "高版本 stylers.xml 被拒绝");
            std::filesystem::remove(path, ec);
        }

        // 样式目录：语言 → 可逐项调整的样式号清单（内置与 UDL 一份表，供界面与上色共用）
        {
            using npcc::edit::LanguageDef;
            using npcc::edit::LanguageRegistry;
            LanguageRegistry languages;
            languages.Load(nullptr);
            const LanguageDef* cpp = languages.FindByExtension("cpp");
            Check(cpp != nullptr, "目录：找得到 cpp");
            if (cpp != nullptr) {
                const std::vector<npcc::edit::StyleEntry> entries = npcc::edit::StylesFor(*cpp);
                Check(entries.size() == 7,
                      "cpp 有 7 项可调（行/块注释、关键字、数字、字符串、强调、次要）");
                Check(!entries.empty() && entries.front().number < entries.back().number,
                      "目录按样式号升序");
                Check(entries[0].number == 1 && entries[0].name == L"块注释" &&
                          entries[0].role == StyleRole::Comment,
                      "cpp 的条目带名字与角色（1 = 块注释）");
            }
            // 没有目录的语言：不认识就返回空表，不要瞎猜
            LanguageDef unknown;
            unknown.languageName = L"nothing";
            unknown.lexer = "no_such_lexer";
            Check(npcc::edit::StylesFor(unknown).empty(), "认不出的词法器没有目录");

            // UDL 的目录是它自己的全表：6 项 + 9 组关键字
            LanguageDef udlLanguage;
            udlLanguage.languageName = L"mydsl";
            udlLanguage.udl = std::make_shared<const npcc::edit::UdlDefinition>();
            const std::vector<npcc::edit::StyleEntry> udlEntries =
                npcc::edit::StylesFor(udlLanguage);
            Check(udlEntries.size() == 15, "UDL 有 15 项可调（6 项 + 9 组关键字）");
            bool sawOperator = false;
            bool sawKeyword9 = false;
            for (const npcc::edit::StyleEntry& entry : udlEntries) {
                if (entry.number == npcc::edit::udl::kStyleOperator) {
                    sawOperator = entry.name == L"运算符";
                }
                if (entry.number == npcc::edit::udl::KeywordStyleOf(8)) {
                    sawKeyword9 = entry.name == L"关键字 9";
                }
            }
            Check(sawOperator, "UDL 目录里有运算符");
            Check(sawKeyword9, "UDL 目录里第 9 组关键字有名字");
        }

        // 逐样式号覆盖：存取、往返、坏值
        {
            StyleScheme scheme = StyleScheme::Defaults();
            std::wstring error;
            Check(scheme.Styles(L"cpp").empty(), "初始没有逐项覆盖");
            npcc::edit::RoleStyle custom;
            custom.fore = 0x123456;
            custom.italic = true;
            scheme.SetStyle(L"cpp", 7, L"次要项", custom);
            Check(scheme.Styles(L"cpp").size() == 1, "写入一条覆盖");
            Check(scheme.Styles(L"cpp")[0].number == 7 &&
                      scheme.Styles(L"cpp")[0].name == L"次要项" &&
                      scheme.Styles(L"cpp")[0].value.fore == 0x123456 &&
                      scheme.Styles(L"cpp")[0].value.italic,
                  "覆盖的内容（样式号、名字、颜色、斜体）");
            Check(scheme.Role(L"cpp", StyleRole::Comment).fore != 0x123456,
                  "逐项覆盖不动同语言的角色色");
            scheme.SetStyle(L"cpp", 7, L"次要项", custom);   // 同一个号写两遍
            Check(scheme.Styles(L"cpp").size() == 1, "同一个样式号只留一条");
            npcc::edit::RoleStyle other = custom;
            other.fore = 0x654321;
            scheme.SetStyle(L"cpp", 21, L"字符串", other);
            Check(scheme.Styles(L"cpp").size() == 2, "不同的样式号各一条");
            scheme.ClearStyle(L"cpp", 7);
            Check(scheme.Styles(L"cpp").size() == 1, "删掉一条之后剩一条");
            scheme.SetStyle(L"cpp", 999, L"越界", custom);   // 越界直接拒
            Check(scheme.Styles(L"cpp").size() == 1, "越界的样式号被拒（否则写了也没反应）");
            scheme.ResetLanguage(L"cpp");
            Check(scheme.Styles(L"cpp").empty(), "恢复默认把逐项覆盖也清掉");

            // 往返：写文件再读回来
            const std::filesystem::path path = StyleScheme::Path();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            StyleScheme saved = StyleScheme::Defaults();
            saved.SetStyle(L"cpp", 7, L"次要项", custom);
            Check(saved.Save(&error), "带逐项覆盖的方案落盘");
            StyleScheme reloaded;
            std::vector<std::wstring> warnings;
            Check(reloaded.Load(&error, &warnings), "重新加载成功");
            Check(reloaded.Styles(L"cpp").size() == 1 &&
                      reloaded.Styles(L"cpp")[0].value.fore == 0x123456,
                  "逐项覆盖往返一致");

            // 坏值：颜色不是 #RRGGBB、样式号越界——跳过并记警告，别让整份文件失效
            const std::string broken =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadccStyles schemaVersion=\"1\">\n"
                "  <Language name=\"cpp\">\n"
                "    <Style role=\"comment\" fore=\"#00FF00\" bold=\"no\" italic=\"no\"/>\n"
                "    <StyleNumber number=\"7\" fore=\"not-a-colour\"/>\n"
                "    <StyleNumber number=\"999\" fore=\"#123456\"/>\n"
                "    <StyleNumber number=\"9\" name=\"强调项\" fore=\"#AABBCC\" bold=\"yes\"/>\n"
                "  </Language>\n"
                "</NotepadccStyles>\n";
            Check(WriteFileDirect(path, broken), "写入带坏值的 stylers.xml");
            StyleScheme tolerant;
            warnings.clear();
            Check(tolerant.Load(&error, &warnings), "带坏值的文件仍能加载");
            Check(warnings.size() >= 2, "坏颜色与越界样式号都给出了警告");
            Check(tolerant.Styles(L"cpp").size() == 1, "只有合法的那条逐项覆盖生效");
            Check(!tolerant.Styles(L"cpp").empty() &&
                      tolerant.Styles(L"cpp")[0].number == 9 &&
                      tolerant.Styles(L"cpp")[0].value.fore == 0xAABBCC &&
                      tolerant.Styles(L"cpp")[0].value.bold,
                  "合法的逐项覆盖照常生效");
            Check(tolerant.Role(L"cpp", StyleRole::Comment).fore == 0x00FF00,
                  "同一份文件里的角色色也照常生效");
            std::filesystem::remove(path, ec);
        }

        // 主题（暗色模式）：浅色/深色两套配色共用一份文件，界面只改"当前这一套"
        {
            StyleScheme scheme = StyleScheme::Defaults();
            Check(scheme.ActiveTheme() == L"light" && !scheme.IsDark(), "默认是浅色主题");
            Check(scheme.ThemeNames().size() == 2, "内置两套主题");
            Check(scheme.SetActiveTheme(L"dark") && scheme.IsDark(), "能切到深色主题");
            Check(scheme.SetActiveTheme(L"light") && !scheme.IsDark(), "能切回浅色");
            Check(!scheme.SetActiveTheme(L"没有这套"), "不认识的主题名要拒绝");
            Check(scheme.ActiveTheme() == L"light", "拒绝之后主题不变");

            // 两套主题的默认配色本来就不同：默认前景/背景 + 每个角色的取值
            const std::uint32_t lightBack = scheme.Back();
            const std::uint32_t lightComment = scheme.Role(L"cpp", StyleRole::Comment).fore;
            Check(lightBack == 0xFFFFFF && lightComment == 0x2E7D32, "浅色主题的取值");
            scheme.SetActiveTheme(L"dark");
            Check(scheme.Back() == 0x1E1E1E, "深色主题的背景是深色");
            Check(scheme.Fore() == 0xD4D4D4, "深色主题的前景是浅色（不是把浅色反过来）");
            Check(scheme.Role(L"cpp", StyleRole::Comment).fore == 0x6A9955,
                  "深色主题的注释色与浅色不同");
            Check(scheme.Role(L"cpp", StyleRole::Keyword).bold, "深色的关键字也加粗");
            Check(scheme.Role(L"还没见过的语言", StyleRole::String).fore == 0xCE9178,
                  "没改过的语言跟着主题走（不是跟着浅色走）");

            // 两套主题各改各的：改深色不该动浅色
            npcc::edit::RoleStyle custom;
            custom.fore = 0x123456;
            scheme.SetRole(L"cpp", StyleRole::Comment, custom);
            npcc::edit::RoleStyle perStyle;
            perStyle.fore = 0xABCDEF;
            scheme.SetStyle(L"cpp", 7, L"次要项", perStyle);
            Check(scheme.Role(L"cpp", StyleRole::Comment).fore == 0x123456, "深色里的修改生效");
            scheme.SetActiveTheme(L"light");
            Check(scheme.Role(L"cpp", StyleRole::Comment).fore == lightComment,
                  "浅色主题没被深色那边的修改带跑");
            Check(scheme.Styles(L"cpp").empty(), "浅色主题里没有那条逐样式号覆盖");
            scheme.SetActiveTheme(L"dark");
            Check(scheme.Styles(L"cpp").size() == 1, "切回来深色的覆盖还在");

            // 往返：两套主题一起写文件、读回来
            const std::filesystem::path path = StyleScheme::Path();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::wstring error;
            Check(scheme.Save(&error), "带两套主题的方案落盘");
            StyleScheme reloaded;
            std::vector<std::wstring> warnings;
            Check(reloaded.Load(&error, &warnings), "重新加载成功");
            Check(reloaded.ThemeNames().size() == 2, "两套主题都读回来了");
            Check(reloaded.ActiveTheme() == L"light", "加载后停在浅色（用哪套由配置决定）");
            reloaded.SetActiveTheme(L"dark");
            Check(reloaded.Back() == 0x1E1E1E, "深色的默认背景往返一致");
            Check(reloaded.Role(L"cpp", StyleRole::Comment).fore == 0x123456,
                  "深色的角色修改往返一致");
            Check(reloaded.Styles(L"cpp").size() == 1 &&
                      reloaded.Styles(L"cpp")[0].value.fore == 0xABCDEF,
                  "深色的逐样式号覆盖往返一致");
            reloaded.SetActiveTheme(L"light");
            Check(reloaded.Styles(L"cpp").empty(), "浅色里没有那条覆盖");

            // 旧文件（schemaVersion 1，根下直接放 Default/Language）仍然要读得进来，
            // 并且当作浅色主题；此时深色仍有一套内置值可用
            const std::string legacy =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadccStyles schemaVersion=\"1\">\n"
                "  <Default fore=\"#101010\" back=\"#F0F0F0\" font=\"Consolas\" size=\"12\"/>\n"
                "  <Language name=\"cpp\">\n"
                "    <Style role=\"comment\" fore=\"#FF0000\" bold=\"no\" italic=\"no\"/>\n"
                "  </Language>\n"
                "</NotepadccStyles>\n";
            Check(WriteFileDirect(path, legacy), "写入旧版（扁平）stylers.xml");
            StyleScheme fromLegacy;
            warnings.clear();
            Check(fromLegacy.Load(&error, &warnings), "旧版文件仍能加载");
            Check(fromLegacy.Back() == 0xF0F0F0, "旧版文件的默认背景生效（当作浅色主题）");
            Check(fromLegacy.Role(L"cpp", StyleRole::Comment).fore == 0xFF0000,
                  "旧版文件里的角色色生效");
            Check(fromLegacy.SetActiveTheme(L"dark") && fromLegacy.Back() == 0x1E1E1E,
                  "旧版文件里没有深色主题时，仍有一套内置深色可用（切过去不是没反应）");
            std::filesystem::remove(path, ec);
        }

        // 暗色模式真的落到控件上：主题（语法色）+ 皮肤（边距/光标/选区）
        {
            const HWND host = CreateHiddenHost();
            npcc::edit::EditView view;
            if (host == nullptr || !view.Create(host, 951)) {
                Check(false, "创建隐藏宿主窗口或编辑控件失败");
            } else {
                npcc::edit::LanguageRegistry languages;
                languages.Load(nullptr);
                const npcc::edit::LanguageDef* cpp = languages.FindByExtension("cpp");
                npcc::edit::StyleScheme scheme = npcc::edit::StyleScheme::Defaults();
                view.SetCodePage(npcc::edit::kCodePageUtf8);
                view.SetText("// c\nint x = 42;\n");
                const auto cref = [](std::uint32_t rgb) {
                    return static_cast<int32_t>(npcc::edit::StyleScheme::ToScintillaColour(rgb));
                };

                view.ApplyPalette(npcc::edit::EditorPalette::Light());
                view.ApplyLanguage(*cpp, &scheme);
                const int32_t lightMargin = view.StyleBackColor(STYLE_LINENUMBER);
                Check(lightMargin == cref(0xF5F5F5), "浅色下行号边距是浅灰");
                Check(view.StyleBackColor(STYLE_DEFAULT) == cref(0xFFFFFF), "浅色下编辑区是白底");

                scheme.SetActiveTheme(L"dark");
                view.ApplyPalette(npcc::edit::EditorPalette::Dark());
                view.ApplyLanguage(*cpp, &scheme);
                Check(view.StyleBackColor(STYLE_DEFAULT) == cref(0x1E1E1E),
                      "深色下编辑区背景变深");
                Check(view.StyleBackColor(STYLE_LINENUMBER) == cref(0x2D2D2D),
                      "深色下行号边距跟着变深（不是白的）");
                Check(view.StyleForeColor(5) == cref(0x569CD6),
                      "深色下关键字用的是深色主题的颜色");
                Check(view.StyleForeColor(5) != cref(0x0000C0), "与浅色的关键字色确实不同");

                // 换语言时内部会 STYLECLEARALL（把行号边距等一起清回默认），
                // 皮肤配色必须自己重铺一遍——否则深色下开一个 .py 就露出白边距
                view.ApplyLanguageForPath(L"D:\\t\\a.py", &languages, &scheme);
                Check(view.StyleBackColor(STYLE_LINENUMBER) == cref(0x2D2D2D),
                      "换语言之后深色边距没有被冲回浅色");
                ::DestroyWindow(host);
            }
        }

        // 配对括号的高亮色跟着皮肤走：它不是语法样式（词法器碰不到 34/35 号），
        // 只能由皮肤自己铺——深色底上不铺就是浅色高亮，压在深色背景上几乎看不见
        {
            const HWND host = CreateHiddenHost();
            npcc::edit::EditView view;
            if (host == nullptr || !view.Create(host, 953)) {
                Check(false, "创建隐藏宿主窗口或编辑控件失败");
            } else {
                const auto cref = [](std::uint32_t rgb) {
                    return static_cast<int32_t>(npcc::edit::StyleScheme::ToScintillaColour(rgb));
                };
                const npcc::edit::EditorPalette light = npcc::edit::EditorPalette::Light();
                const npcc::edit::EditorPalette dark = npcc::edit::EditorPalette::Dark();
                // 两套配色必须是两组不同的颜色，否则"跟着皮肤走"就没有可观察的结果
                Check(light.braceBack != dark.braceBack && light.braceBadBack != dark.braceBadBack,
                      "浅色与深色的括号高亮不是同一组颜色");
                Check(light.braceBadBack != light.braceBack,
                      "配上与没配上用的是两种底色（否则用户看不出区别）");

                view.SetCodePage(npcc::edit::kCodePageUtf8);
                view.ApplyPalette(light);
                Check(view.StyleBackColor(STYLE_BRACELIGHT) == cref(light.braceBack) &&
                          view.StyleForeColor(STYLE_BRACELIGHT) == cref(light.braceFore),
                      "浅色下配对成功的高亮色落在 STYLE_BRACELIGHT 上");
                Check(view.StyleBackColor(STYLE_BRACEBAD) == cref(light.braceBadBack),
                      "浅色下没配对的高亮色落在 STYLE_BRACEBAD 上");

                view.ApplyPalette(dark);
                Check(view.StyleBackColor(STYLE_BRACELIGHT) == cref(dark.braceBack),
                      "深色下配对成功的高亮色换成了深色那套");
                Check(view.StyleBackColor(STYLE_BRACEBAD) == cref(dark.braceBadBack),
                      "深色下没配对的高亮色也换了");

                // 画高亮这件事本身没有可读回的接口（Scintilla 不留"当前高亮在哪"），
                // 所以位置与配色分别由上面两段钉住，这里只钉"调用不炸、且不动文档与光标"
                view.SetText("f(a");
                view.SetCaretPosition(1);
                view.HighlightBracesNearCaret();   // 没配上：走 SCI_BRACEBADLIGHT 那条
                Check(view.CaretPosition() == 1 && view.GetAllText() == "f(a",
                      "画高亮不动文档也不动光标");
                view.SetCaretPosition(2);          // 'a'：旁边没有括号
                view.HighlightBracesNearCaret();   // 走"清掉高亮"那条
                view.ClearBraceHighlight();
                Check(view.GetAllText() == "f(a", "清高亮同样不动文档");
                ::DestroyWindow(host);
            }
        }

        // 配色真的落到渲染上：用 cpp 的注释样式号（2）核对
        {
            const HWND host = CreateHiddenHost();
            npcc::edit::EditView view;
            if (host != nullptr && view.Create(host, 950)) {
                npcc::edit::LanguageRegistry languages;
                languages.Load(nullptr);
                const npcc::edit::LanguageDef* cpp = languages.FindByExtension("cpp");
                Check(cpp != nullptr, "找得到 cpp 语言定义");
                if (cpp != nullptr) {
                    view.SetCodePage(npcc::edit::kCodePageUtf8);
                    view.SetText("// c\nint x;\n");

                    view.ApplyLanguage(*cpp, nullptr);
                    // 断言的是交给控件的 COLORREF（0x00BBGGRR），不是文件里的 #RRGGBB。
                    // 只比对"存进去的值"是抓不到红蓝互换的（踩过，见附录 B）
                    Check(view.StyleForeColor(2) == static_cast<int32_t>(
                                                        npcc::edit::StyleScheme::ToScintillaColour(
                                                            0x2E7D32)),
                          "默认配色下注释色是绿（交给控件的是 COLORREF，红蓝没反）");

                    npcc::edit::StyleScheme scheme = npcc::edit::StyleScheme::Defaults();
                    npcc::edit::RoleStyle custom;
                    // 故意挑一个 R/B 不对称的颜色：对称色（如 #FF00FF）即使红蓝互换也看不出差别
                    custom.fore = 0x1A2B3C;
                    scheme.SetRole(L"cpp", StyleRole::Comment, custom);
                    view.ApplyLanguage(*cpp, &scheme);
                    Check(view.StyleForeColor(2) ==
                              static_cast<int32_t>(
                                  npcc::edit::StyleScheme::ToScintillaColour(0x1A2B3C)),
                          "方案里的注释色真的生效了");
                    Check(view.StyleForeColor(5) ==
                              static_cast<int32_t>(
                                  npcc::edit::StyleScheme::ToScintillaColour(0x0000C0)),
                          "没覆盖的角色不受影响");

                    // 逐样式号：压过角色色。这里故意挑一个角色也用到的号（2 = cpp 行注释），
                    // 否则证明不了"覆盖真的盖在角色之上"，只能证明"各写各的"
                    npcc::edit::RoleStyle perStyle;
                    perStyle.fore = 0x00AAFF;
                    scheme.SetStyle(L"cpp", 2, L"行注释", perStyle);
                    view.ApplyLanguage(*cpp, &scheme);
                    Check(view.StyleForeColor(2) ==
                              static_cast<int32_t>(
                                  npcc::edit::StyleScheme::ToScintillaColour(0x00AAFF)),
                          "逐样式号的覆盖压过了角色色");
                    // 块注释（1）与行注释（2）同属 Comment 角色，所以它拿到的是角色色，
                    // 而不是上面那条只针对样式号 2 的覆盖——覆盖的作用范围是"这一个号"
                    Check(view.StyleForeColor(1) ==
                              static_cast<int32_t>(
                                  npcc::edit::StyleScheme::ToScintillaColour(0x1A2B3C)),
                          "同一个角色的别的样式号（块注释）只受角色色影响");
                    // 文件里有的样式号、目录里没有的（cpp 的运算符是 10）也要生效：
                    // 目录只影响"界面上列得出来"，文件才是最终话语权
                    npcc::edit::RoleStyle notInCatalogue;
                    notInCatalogue.fore = 0xFF8800;
                    scheme.SetStyle(L"cpp", 10, L"运算符", notInCatalogue);
                    view.ApplyLanguage(*cpp, &scheme);
                    Check(view.StyleForeColor(10) ==
                              static_cast<int32_t>(
                                  npcc::edit::StyleScheme::ToScintillaColour(0xFF8800)),
                          "目录里没有的样式号照样生效（文件才是权威）");
                }
            } else {
                Check(false, "创建隐藏宿主窗口或编辑控件失败");
            }
            if (host != nullptr) {
                ::DestroyWindow(host);
            }
        }
    }

    Section("语言定义（langs.xml）");
    {
        using npcc::edit::LanguageRegistry;

        npcc::infra::InitializePaths(true);
        const std::string extensionCases[][2] = {
            {"a.cpp", "cpp"}, {"B.CPP", "cpp"}, {"x.py", "py"}, {"y.JS", "js"},
            {"z.md", "md"}, {"w.sql", "sql"}, {"v.ps1", "ps1"},
            {"noext", ""}, {"dir.with.dots/plain", ""}};
        for (const auto& testCase : extensionCases) {
            const std::wstring wide(testCase[0].begin(), testCase[0].end());
            CheckEqual(LanguageRegistry::ExtensionOfFile(wide), testCase[1],
                       "从路径取扩展名（小写、不带点）");
        }

        {
            LanguageRegistry registry;
            std::wstring error;
            Check(registry.Load(&error), "没有 langs.xml 时加载成功（用内置表）");

            const npcc::edit::LanguageDef* cpp = registry.FindByExtension("cpp");
            Check(cpp != nullptr, "内置表认得出 cpp");
            Check(cpp != nullptr && cpp->lexer == "cpp", "cpp 用的是 cpp 词法器");
            Check(cpp != nullptr && cpp->lineComment == "//" &&
                      cpp->blockCommentStart == "/*" && cpp->blockCommentEnd == "*/",
                  "cpp 的注释符正确");
            Check(cpp != nullptr && !cpp->keywords.empty() && !cpp->keywords[0].empty(),
                  "cpp 带关键字表（没有关键字表等于没有关键字着色）");

            const npcc::edit::LanguageDef* js = registry.FindByExtension("js");
            Check(js != nullptr && js->lexer == "cpp",
                  "javascript 用 cpp 词法器（Lexilla 里没有 javascript 词法器）");
            const npcc::edit::LanguageDef* xml = registry.FindByExtension("xml");
            Check(xml != nullptr && xml->lineComment.empty() && xml->blockCommentStart == "<!--",
                  "xml 只有块注释符");
            Check(registry.FindByExtension("nope") == nullptr, "认不出的扩展名返回空");
            Check(registry.Find(L"sql") != nullptr, "按语言标识查得到");
        }

        // 导出 → 重新加载：往返之后应当与内置表等价
        {
            const std::filesystem::path path = LanguageRegistry::Path();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            LanguageRegistry exported;
            std::wstring error;
            Check(exported.Save(&error), "导出语言定义成功");
            Check(std::filesystem::exists(path), "导出的 langs.xml 落盘了");

            LanguageRegistry reloaded;
            Check(reloaded.Load(&error), "重新加载导出的语言定义成功");
            // 比内置表：All() 里还可能有 UDL（来自 userlangs.xml），而这里考的是
            // langs.xml 的往返，别把它跟另一份文件混在一起算
            Check(reloaded.Builtins().size() == exported.Builtins().size(), "语言条数往返一致");
            const npcc::edit::LanguageDef* cpp = reloaded.FindByExtension("cpp");
            Check(cpp != nullptr && cpp->lineComment == "//" && !cpp->keywords.empty(),
                  "往返后 cpp 的定义完整（含注释符与关键字）");
            const npcc::edit::LanguageDef* md = reloaded.FindByExtension("md");
            Check(md != nullptr && md->blockCommentStart == "<!--", "往返后 markdown 的定义完整");
        }

        // 文件里的语言覆盖内置同名语言；没提到的仍用内置值；新增的语言追加
        {
            const std::filesystem::path path = LanguageRegistry::Path();
            const std::string custom =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<Languages schemaVersion=\"1\">\n"
                "  <Language name=\"cpp\" display=\"C++ 自定义\" lexer=\"cpp\" ext=\"cpp ccx\""
                " foldable=\"yes\">\n"
                "    <LineComment prefix=\";;\"/>\n"
                "    <Keywords group=\"0\">int long</Keywords>\n"
                "  </Language>\n"
                "  <Language name=\"mylang\" display=\"我的语言\" lexer=\"props\" ext=\"mylang\""
                " foldable=\"no\">\n"
                "    <LineComment prefix=\"##\"/>\n"
                "  </Language>\n"
                "</Languages>\n";
            Check(WriteFileDirect(path, custom), "写入自定义 langs.xml");
            LanguageRegistry registry;
            std::wstring error;
            Check(registry.Load(&error), "加载自定义 langs.xml 成功");
            const npcc::edit::LanguageDef* cpp = registry.FindByExtension("cpp");
            Check(cpp != nullptr && cpp->lineComment == ";;", "文件里的行注释符覆盖了内置值");
            Check(cpp != nullptr && cpp->displayName == L"C++ 自定义", "显示名也被覆盖");
            Check(cpp != nullptr && cpp->keywords.size() == 1 && cpp->keywords[0] == "int long",
                  "关键字表被整体替换");
            Check(registry.FindByExtension("ccx") == cpp, "新增的扩展名指向同一个语言");
            Check(registry.FindByExtension("py") != nullptr,
                  "文件里没提到的语言仍用内置值");
            const npcc::edit::LanguageDef* mine = registry.Find(L"mylang");
            Check(mine != nullptr && mine->extensions.size() == 1 && !mine->foldable,
                  "文件里新增的语言被追加进来");
        }

        // 坏文件与高版本：报错并退回内置表，而不是让程序起不来
        {
            const std::filesystem::path path = LanguageRegistry::Path();
            Check(WriteFileDirect(path, "not xml at all"), "写入损坏的 langs.xml");
            LanguageRegistry registry;
            std::wstring error;
            Check(!registry.Load(&error), "损坏的 langs.xml 加载失败");
            Check(!error.empty(), "失败时给出原因");
            Check(registry.FindByExtension("cpp") != nullptr,
                  "失败之后仍然有内置表可用（不能因为配置坏了就没法编辑）");

            const std::string future =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<Languages schemaVersion=\"99\"><Language name=\"x\" lexer=\"null\"/></Languages>\n";
            Check(WriteFileDirect(path, future), "写入高版本 langs.xml");
            LanguageRegistry futureRegistry;
            Check(!futureRegistry.Load(&error), "高版本 langs.xml 被拒绝");

            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    // ---------------------------------------------------------------- UDL（用户自定义语言）
    // UDL 的词法器是我们自己写的（锁定的 Lexilla 里没有 container 词法器可复用，
    // 见设计说明书 7.6 末尾的改判），所以这一段既要测"定义解析得对"，也要测
    // "这份定义**真的**改变了着色、注释与折叠"——只测解析等于没测词法器。
    Section("UDL 用户自定义语言（userlangs.xml + 自研词法器）");
    {
        using npcc::edit::LanguageDef;
        using npcc::edit::LanguageRegistry;
        using npcc::edit::UdlDefinition;
        using npcc::edit::UdlLoader;

        npcc::infra::InitializePaths(true);

        const std::string sampleXml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<UserLangs schemaVersion=\"1\">\n"
            "  <UserLang name=\"mydsl\" displayName=\"My DSL\" ext=\"mydsl MDL\"\n"
            "            caseSensitive=\"yes\">\n"
            "    <LineComment prefix=\"//\"/>\n"
            "    <BlockComment start=\"(*\" end=\"*)\"/>\n"
            "    <Keywords group=\"0\">let fun match with type module end</Keywords>\n"
            "    <Keywords group=\"1\">true false null</Keywords>\n"
            "    <StringDelim start=\"&quot;\" end=\"&quot;\" escape=\"\\\"/>\n"
            "    <StringDelim start=\"'\"/>\n"
            "    <Operators>+ - * / = &lt; &gt; |</Operators>\n"
            "    <Fold openTokens=\"let fun match\" closeTokens=\"end\"/>\n"
            "  </UserLang>\n"
            "</UserLangs>\n";

        // 1) 解析：属性、扩展名、注释符、关键字分组、字符串、运算符、折叠词
        {
            std::vector<UdlDefinition> definitions;
            std::wstring error;
            std::vector<std::wstring> warnings;
            Check(UdlLoader::Parse(sampleXml, definitions, &error, &warnings), "解析 UDL 成功");
            Check(definitions.size() == 1 && warnings.empty(), "解析出一条定义且没有警告");
            if (!definitions.empty()) {
                const UdlDefinition& def = definitions[0];
                Check(def.name == L"mydsl" && def.displayName == L"My DSL", "name / displayName");
                Check(def.extensions == std::vector<std::string>{"mydsl", "mdl"},
                      "扩展名按空白切分并统一转小写");
                Check(def.caseSensitive, "caseSensitive=yes 生效");
                Check(def.lineComment == "//" && def.blockCommentStart == "(*" &&
                          def.blockCommentEnd == "*)",
                      "行注释与块注释符");
                Check(def.keywordGroups.size() == 2 &&
                          def.keywordGroups[0].find("let") != std::string::npos &&
                          def.keywordGroups[1] == "true false null",
                      "关键字分组按 group 归位");
                Check(def.strings.size() == 2, "两对字符串界定符");
                Check(!def.strings.empty() && def.strings[0].escape == "\\", "转义字符");
                Check(def.strings.size() > 1 && def.strings[1].end == "'",
                      "只写 start 时 end 缺省等于 start");
                Check(def.operators.find('+') != std::string::npos &&
                          def.operators.find('|') != std::string::npos,
                      "运算符按字符收");
                Check(def.operators.find(' ') == std::string::npos &&
                          def.operators.find('\t') == std::string::npos,
                      "运算符列表里的空格是分隔符，不算运算符字符");
                Check(def.foldable && def.foldOpenTokens.size() == 3 &&
                          def.foldCloseTokens.size() == 1,
                      "折叠词分开与收起两组");
            }
        }

        // 2) 坏输入：一条写坏不该把别的定义或内置语言带坏
        {
            std::vector<UdlDefinition> definitions;
            std::wstring error;
            std::vector<std::wstring> warnings;
            Check(!UdlLoader::Parse("<UserLangs><UserLang", definitions, &error, &warnings),
                  "XML 不闭合要报错");
            Check(!error.empty(), "报错带原因");
            Check(!UdlLoader::Parse("<Nope/>", definitions, &error, &warnings),
                  "根元素不认识要报错");

            const std::string messy =
                "<UserLangs>"
                "<UserLang ext=\"aaa\"/>"                                   // 没 name
                "<UserLang name=\"noext\"/>"                                // 没 ext
                "<UserLang name=\"badgroup\" ext=\"bbb\">"
                "<Keywords group=\"17\">x</Keywords></UserLang>"            // 组号越界
                "<UserLang name=\"halfblock\" ext=\"ccc\">"
                "<BlockComment start=\"(*\"/></UserLang>"                   // 块注释只写一半
                "<UserLang name=\"ok\" ext=\"ddd\"><LineComment prefix=\"#\"/>"
                "</UserLang>"
                "</UserLangs>";
            warnings.clear();
            Check(UdlLoader::Parse(messy, definitions, &error, &warnings),
                  "个别条目有问题不影响整份文件");
            Check(definitions.size() == 3, "三条能用的留下来（没 name 与没 ext 的被丢掉）");
            Check(warnings.size() >= 3, "每条问题都留了警告");
            for (const UdlDefinition& def : definitions) {
                if (def.name == L"halfblock") {
                    Check(def.blockCommentStart.empty() && def.blockCommentEnd.empty(),
                          "块注释只写一半时整对丢掉，而不是留下半个");
                }
                if (def.name == L"ok") {
                    Check(def.lineComment == "#", "正常那条照常解析");
                }
            }
        }

        // 3) 接上语言表：写成 userlangs.xml，注册表按扩展名认领，UDL 优先于内置
        {
            const std::filesystem::path path = UdlLoader::Path();
            std::error_code ec;
            std::filesystem::remove(path, ec);

            LanguageRegistry registry;
            std::wstring error;
            std::vector<std::wstring> warnings;
            Check(registry.Load(&error, &warnings), "没有 userlangs.xml 时加载照样成功");
            Check(registry.UserDefined().empty(), "没有文件时没有 UDL");
            Check(registry.FindByExtension("mydsl") == nullptr, "没定义的扩展名找不到语言");

            // 故意让第二条抢内置的 cpp 扩展名：UDL 应当赢，并且留下一条警告
            std::string withCpp = sampleXml;
            const std::size_t insertAt = withCpp.rfind("</UserLangs>");
            Check(insertAt != std::string::npos, "样例 XML 有闭合标签");
            withCpp.insert(insertAt,
                           "  <UserLang name=\"fakecpp\" ext=\"cpp\">"
                           "<LineComment prefix=\"#\"/></UserLang>\n");
            Check(WriteFileDirect(path, withCpp), "写入 userlangs.xml");
            LanguageRegistry loaded;
            warnings.clear();
            Check(loaded.Load(&error, &warnings), "带 UDL 的语言表加载成功");
            Check(loaded.UserDefined().size() == 2, "两条 UDL 都在");
            Check(loaded.Builtins().size() >= 13, "内置语言一条没少");
            Check(loaded.All().size() == loaded.Builtins().size() + loaded.UserDefined().size(),
                  "All() 是内置 + UDL");
            const LanguageDef* mine = loaded.FindByExtension("mydsl");
            Check(mine != nullptr && mine->udl != nullptr, "按扩展名认领到 UDL");
            Check(mine != nullptr && mine->lexer == "udl", "UDL 的语言定义标着自研词法器");
            Check(mine != nullptr && mine->lineComment == "//", "注释符来自 UDL 定义");
            Check(mine != nullptr && mine->foldable, "有折叠词就算可折叠");
            const LanguageDef* cpp = loaded.FindByExtension("cpp");
            Check(cpp != nullptr && cpp->languageName == L"fakecpp",
                  "扩展名撞车时 UDL 优先");
            bool sawClash = false;
            for (const std::wstring& warning : warnings) {
                if (warning.find(L"重叠") != std::wstring::npos) {
                    sawClash = true;
                }
            }
            Check(sawClash, "撞车留下警告（不然用户会以为定义没生效）");
            Check(loaded.Find(L"mydsl") != nullptr, "按名字也找得到");

            // 4)~6) 真的装上控件：着色、注释符、折叠
            const HWND host = CreateHiddenHost();
            npcc::edit::EditView view;
            if (host == nullptr) {
                Check(false, "创建隐藏宿主窗口失败");
            } else if (view.Create(host, 941)) {
                view.ApplyLightTheme();
                view.SetCodePage(npcc::edit::kCodePageUtf8);
                const std::string text =
                    "// 行注释\n"
                    "(* 块注释 let *)\n"
                    "let answer = 42\n"
                    "fun f x = \"字符串里的 let\"\n"
                    "let s = 'a' | 'b'\n"
                    "end\n";
                view.SetText(text);
                view.ApplyLanguageForPath(L"D:\\t\\a.mydsl", &loaded);

                // 4) 着色：每个 token 都要拿到"不是默认色"的前景色
                const int32_t defaultFore = view.DefaultForeColor();
                struct Needle {
                    const char* text;
                    const char* what;
                };
                const Needle needles[] = {
                    {"// 行注释", "行注释"},
                    {"(* ", "块注释"},
                    {"let ", "关键字（第 0 组）"},
                    {"answer", "标识符"},
                    {"42", "数字"},
                    {"\"字符串", "字符串（双引号）"},
                    {"'a'", "字符串（单引号）"},
                    {"= ", "运算符"},
                };
                for (const Needle& needle : needles) {
                    const std::size_t pos = text.find(needle.text);
                    if (pos == std::string::npos) {
                        Check(false, "测试文本里找不到要检查的 token");
                        continue;
                    }
                    const int32_t style = view.StyleAt(static_cast<int32_t>(pos));
                    const bool coloured =
                        style != 0 && view.StyleForeColor(style) != defaultFore;
                    if (!coloured) {
                        std::printf("    [FAIL] UDL 的 %s 没有拿到配色（样式号 %d）\n",
                                    needle.what, style);
                    }
                    Check(coloured, "UDL 的 token 拿到了与默认色不同的前景色");
                }
                // 注释里的 let 不该被当成关键字（样式必须是注释那个号）
                const std::size_t inComment = text.find("(* 块注释 let *)");
                Check(inComment != std::string::npos &&
                          view.StyleAt(static_cast<int32_t>(inComment + 10)) ==
                              npcc::edit::udl::kStyleCommentBlock,
                      "块注释里的关键字词不算关键字");
                // 第二组关键字（true/false/null）走的是另一组样式号
                view.SetText("true false let\n");
                view.ApplyLanguageForPath(L"D:\\t\\a.mydsl", &loaded);
                Check(view.StyleAt(0) == npcc::edit::udl::KeywordStyleOf(1),
                      "第 1 组关键字用的是第 1 组的样式号");
                Check(view.StyleAt(11) == npcc::edit::udl::KeywordStyleOf(0),
                      "第 0 组关键字用的是第 0 组的样式号");
                // 词与词之间的空格不该被染色（运算符列表里写了空格的那个 bug 就在这里）
                Check(view.StyleAt(4) == npcc::edit::udl::kStyleDefault,
                      "空格不算运算符");

                // 5) 注释/取消注释走 UDL 自己的注释符
                view.SetText("a\nb\n");
                view.ApplyLanguageForPath(L"D:\\t\\a.mydsl", &loaded);
                // LanguageName() 是稳定标识（给配置与注释用），显示名走 CurrentLanguageName()
                Check(view.LanguageName() == L"mydsl", "语言标识用的是 UDL 的 name");
                const wchar_t* shown = view.CurrentLanguageName();
                Check(shown != nullptr && std::wstring(shown) == L"My DSL",
                      "界面显示名来自 UDL 定义");
                const std::string commented =
                    npcc::edit::ToggleLineComment("a\nb\n", mine->lineComment);
                Check(commented == "// a\n// b\n", "Ctrl+Q 用的是 UDL 的行注释符");

                // 6) 折叠：折叠层级真的算出来了、折叠头也对
                const std::string folding =
                    "let f x =\n"
                    "  match x with\n"
                    "  | 1 -> 1\n"
                    "  | _ -> 0\n"
                    "end\n"
                    "z = 1\n";
                view.SetText(folding);
                view.ApplyLanguageForPath(L"D:\\t\\a.mydsl", &loaded);
                Check(view.FoldingEnabled(), "有折叠词的 UDL 会给折叠边距");
                Check(view.LineIsFoldHeader(0), "第 1 行（let）是折叠头");
                // FoldLevelOf 带着 SC_FOLDLEVELHEADERFLAG 一起返回，比大小要掩掉标志位
                Check((view.FoldLevelOf(1) & 0xFFF) > (view.FoldLevelOf(0) & 0xFFF),
                      "块内那一行的层级比折叠头高");
                Check(!view.LineIsFoldHeader(5), "块外的行不是折叠头");
                view.FoldAll();
                Check(view.VisibleLineCount() < view.LineCount(), "折叠全部之后屏幕上的行少了");
                view.UnfoldAll();
                Check(view.VisibleLineCount() == view.LineCount(), "展开回来一样多");
                // 注释里的折叠词不算折叠词
                view.SetText("// let 假折叠\nx\n");
                view.ApplyLanguageForPath(L"D:\\t\\a.mydsl", &loaded);
                Check(!view.LineIsFoldHeader(0), "注释里的折叠词不算折叠头");
                ::DestroyWindow(host);
            } else {
                Check(false, "创建编辑控件失败");
            }

            std::filesystem::remove(path, ec);
        }
    }

    Section("文件关联（只进打开方式列表，不抢默认关联）");
    {
        const std::wstring testRoot = L"Software\\notepadcc-selftest";
        const std::wstring classes = testRoot + L"\\Classes";
        ::RegDeleteTreeW(HKEY_CURRENT_USER, testRoot.c_str());   // 清掉上次跑剩下的

        Check(npcc::infra::CountRegistered(HKEY_CURRENT_USER, classes) == 0,
              "初始状态没有任何登记");
        std::wstring error;
        Check(npcc::infra::RegisterAssociations(HKEY_CURRENT_USER, classes,
                                                L"D:\\t\\notepadcc.exe", &error),
              "登记到打开方式列表成功");
        const int expected = static_cast<int>(npcc::infra::DefaultAssociations().size());
        Check(npcc::infra::CountRegistered(HKEY_CURRENT_USER, classes) == expected,
              "所有扩展名都登记上了");

        HKEY command = nullptr;
        const std::wstring commandPath = classes + L"\\notepadcc.txt\\shell\\open\\command";
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, commandPath.c_str(), 0, KEY_READ, &command) ==
            ERROR_SUCCESS) {
            wchar_t buffer[512] = {};
            DWORD size = sizeof(buffer);
            DWORD type = 0;
            const bool read = ::RegQueryValueExW(command, nullptr, nullptr, &type,
                                                 reinterpret_cast<BYTE*>(buffer), &size) ==
                              ERROR_SUCCESS;
            Check(read, "能读到打开命令");
            const std::wstring value(buffer);
            Check(value.find(L"\"%1\"") != std::wstring::npos,
                  "打开命令里的 %1 带引号（路径有空格时不会被拆成两个参数）");
            Check(value.find(L"D:\\t\\notepadcc.exe") != std::wstring::npos, "命令里是程序自身路径");
            ::RegCloseKey(command);
        } else {
            Check(false, "打开命令的注册表项不存在");
        }

        Check(npcc::infra::UnregisterAssociations(HKEY_CURRENT_USER, classes, &error),
              "从打开方式列表移除成功");
        Check(npcc::infra::CountRegistered(HKEY_CURRENT_USER, classes) == 0, "移除后没有登记");

        ::RegDeleteTreeW(HKEY_CURRENT_USER, testRoot.c_str());
        Check(::RegOpenKeyExW(HKEY_CURRENT_USER, testRoot.c_str(), 0, KEY_READ, &command) !=
                  ERROR_SUCCESS,
              "测试用的临时注册表键已删除（没有污染真实关联）");
    }

    Section("单实例交接（路径打包与解析）");
    {
        const std::vector<std::wstring> paths = {
            L"D:\\工程 目录\\含 空格 的文件.txt",
            L"C:\\a.cpp",
            L"\\\\server\\share\\x.md",
        };
        const std::string packed = npcc::infra::PackPaths(paths);
        const std::vector<std::wstring> unpacked = npcc::infra::UnpackPaths(packed);
        Check(unpacked.size() == paths.size(), "路径条数往返一致");
        Check(unpacked == paths, "路径内容逐条往返一致（空格、中文、UNC 都不出错）");
        Check(npcc::infra::UnpackPaths("").empty(), "空载荷解析出空列表");
        Check(npcc::infra::UnpackPaths(std::string("\0\0", 2)).empty(), "只有收尾符也算空");
        Check(npcc::infra::PackPaths({}).size() >= sizeof(wchar_t), "空列表也有收尾符");
    }

    Section("编码转换（转为 ANSI / UTF-8 / UTF-16）");
    {
        using npcc::edit::ConvertResult;
        using npcc::edit::UniMode;

        const std::string chinese = "\xE4\xB8\xAD\xE6\x96\x87";   // "中文" 的 UTF-8
        npcc::edit::EncodingState utf8;
        utf8.uniMode = UniMode::Utf8;
        utf8.codePage = npcc::edit::kCodePageUtf8;

        // UTF-8 → ANSI(936)：字节要真的变成 GBK，转回来要一模一样
        {
            const ConvertResult toAnsi = npcc::edit::ConvertTo(chinese, utf8, UniMode::Ansi, 936);
            Check(toAnsi.ok, "UTF-8 转 ANSI 成功");
            Check(toAnsi.editorBytes.size() == 4, "中文在 GBK 下是 4 字节");
            Check(static_cast<unsigned char>(toAnsi.editorBytes[0]) == 0xD6 &&
                      static_cast<unsigned char>(toAnsi.editorBytes[1]) == 0xD0,
                  "GBK 字节与预期一致");
            Check(toAnsi.encoding.uniMode == UniMode::Ansi && toAnsi.encoding.codePage == 936,
                  "转换后的编码状态正确");
            Check(!toAnsi.encoding.hasBom, "ANSI 不带 BOM");

            const ConvertResult back =
                npcc::edit::ConvertTo(toAnsi.editorBytes, toAnsi.encoding, UniMode::Utf8, 936);
            Check(back.ok && back.editorBytes == chinese, "ANSI 再转回 UTF-8 与原文一致");
        }

        // 表示不了的字符必须被**发现**：静默变成 '?' 是最坏的结果
        {
            const std::string withEmoji = "ok \xF0\x9F\x98\x80";   // U+1F600
            const ConvertResult failed =
                npcc::edit::ConvertTo(withEmoji, utf8, UniMode::Ansi, 936);
            Check(!failed.ok, "GBK 表示不了的字符会让转换失败而不是静默替换");
            Check(failed.problem.find(L"无法用代码页") != std::wstring::npos,
                  "失败原因说明是代码页表示不了");
            Check(failed.problem.find(L"\U0001F600") != std::wstring::npos ||
                      failed.problem.size() > 20,
                  "失败原因里列出具体字符");
        }

        // UTF-8 → UTF-8-BOM：缓冲区字节不变，只是保存时加 BOM
        {
            const ConvertResult toBom = npcc::edit::ConvertTo(chinese, utf8, UniMode::Utf8Bom, 936);
            Check(toBom.ok && toBom.editorBytes == chinese, "转 UTF-8-BOM 不改动缓冲区字节");
            Check(toBom.encoding.hasBom, "转 UTF-8-BOM 后带上 BOM 标记");
            const std::string saved = npcc::edit::EncodeForSave(toBom.editorBytes, toBom.encoding);
            Check(saved.size() == chinese.size() + 3 && saved.compare(0, 3, "\xEF\xBB\xBF") == 0,
                  "保存时才真的写出 BOM");
        }

        // UTF-8 → UTF-16LE：缓冲区同样是 UTF-8，保存时编成 UTF-16
        {
            const ConvertResult toUtf16 =
                npcc::edit::ConvertTo(chinese, utf8, UniMode::Utf16LE, 936);
            Check(toUtf16.ok && toUtf16.editorBytes == chinese, "转 UTF-16 不改动缓冲区字节");
            const std::string saved = npcc::edit::EncodeForSave(toUtf16.editorBytes,
                                                                toUtf16.encoding);
            Check(saved.size() == 2 + 2 * 2 && saved.compare(0, 2, "\xFF\xFE") == 0,
                  "保存时写出 UTF-16LE 的 BOM 与双字节内容");
            // 再转回来：UTF-16 形态（缓冲区是 UTF-8）转 ANSI 也要能走通
            const ConvertResult toAnsi =
                npcc::edit::ConvertTo(toUtf16.editorBytes, toUtf16.encoding, UniMode::Ansi, 936);
            Check(toAnsi.ok && toAnsi.editorBytes.size() == 4, "UTF-16 状态的内容转 ANSI 同样正确");
        }

        // 空内容不该出错
        {
            const ConvertResult empty = npcc::edit::ConvertTo("", utf8, UniMode::Ansi, 936);
            Check(empty.ok && empty.editorBytes.empty(), "空内容转 ANSI 成功且仍为空");
        }

        // 声明是 UTF-8 但字节不是合法 UTF-8：要报出来，而不是猜
        {
            const std::string broken("\xC3\x28", 2);
            const ConvertResult failed = npcc::edit::ConvertTo(broken, utf8, UniMode::Utf8, 936);
            Check(!failed.ok, "非法 UTF-8 字节被拒绝");
        }
    }

    Section("语法着色（配色确实落到样式号上）");
    {
        struct Sample {
            const wchar_t* path;
            const char*    text;
            const char*    needles[4];   // 至少要着色的 token（空串表示该项不适用）
        };
        const Sample samples[] = {
            {L"D:\\t\\a.cpp", "// c\nint x = 42;\nconst char* s = \"hi\";\n",
             {"// c", "int", "42", "hi"}},
            {L"D:\\t\\a.h", "#include <a>\nint x;\n", {"#include", "int", "", ""}},
            {L"D:\\t\\a.py", "def f():\n    # c\n    x = 42\n    y = \"hi\"\n",
             {"# c", "def", "42", "hi"}},
            {L"D:\\t\\a.js", "// c\nvar x = 42;\nvar s = \"hi\";\n", {"// c", "var", "42", "hi"}},
            {L"D:\\t\\a.ts", "// c\nlet x = 42;\n", {"// c", "let", "42", ""}},
            {L"D:\\t\\a.json", "{\n  \"k\": 1,\n  \"s\": \"hi\"\n}\n", {"\"k\"", "1", "hi", ""}},
            {L"D:\\t\\a.xml", "<!-- c -->\n<a b=\"hi\">\n", {"<!-- c", "a", "b", "hi"}},
            {L"D:\\t\\a.html", "<!-- c -->\n<p class=\"hi\">t</p>\n", {"<!-- c", "p", "class", "hi"}},
            {L"D:\\t\\a.css", "/* c */\n.a { color: red; }\n", {"/* c", "a", "red", ""}},
            {L"D:\\t\\a.md", "# head\n\nsee `code` here\n", {"# head", "code", "", ""}},
            {L"D:\\t\\a.sql", "-- c\nSELECT a FROM t WHERE b = 42 AND c = 'hi';\n",
             {"-- c", "SELECT", "42", "hi"}},
            {L"D:\\t\\a.ps1", "# c\n$x = 42\n$s = \"hi\"\n", {"# c", "$x", "42", "hi"}},
            {L"D:\\t\\a.bat", "REM c\nset x=42\n", {"REM c", "set", "", ""}},
            {L"D:\\t\\a.ini", "; c\n[sec]\nkey=value\n", {"; c", "[sec]", "key", ""}},
        };
        const HWND colourHost = CreateHiddenHost();
        npcc::edit::EditView view;
        if (colourHost == nullptr) {
            Check(false, "创建隐藏宿主窗口失败");
        } else if (view.Create(colourHost, 940)) {
            view.ApplyLightTheme();
            view.SetCodePage(npcc::edit::kCodePageUtf8);
            int checkedTokens = 0;
            for (const Sample& sample : samples) {
                view.SetText(sample.text);
                view.ApplyLanguageForPath(sample.path);
                const std::string text(sample.text);
                const int32_t defaultFore = view.DefaultForeColor();
                for (const char* needle : sample.needles) {
                    if (needle == nullptr || *needle == '\0') {
                        continue;
                    }
                    const std::size_t pos = text.find(needle);
                    ++checkedTokens;
                    if (pos == std::string::npos) {
                        Check(false, "测试文本里找不到要检查的 token");
                        continue;
                    }
                    const int32_t style = view.StyleAt(static_cast<int32_t>(pos));
                    // 样式号 0 是默认样式；颜色与默认前景色相同也等于"没上色"
                    const bool coloured =
                        style != 0 && view.StyleForeColor(style) != defaultFore;
                    if (!coloured) {
                        std::printf("    [FAIL] %ls 的 token 「%s」没有拿到配色（样式号 %d）\n",
                                    sample.path, needle, style);
                    }
                    Check(coloured, "token 拿到了与默认色不同的前景色");
                }
            }
            Check(checkedTokens >= 40, "检查的 token 数量够多（不是只测了一两个）");
            ::DestroyWindow(colourHost);
        } else {
            Check(false, "创建编辑控件失败");
        }
    }

    // ---------------------------------------------------------------- 括号匹配与摘要
    // 这一段钉的是"纯函数的结果真的接对了线"：FindBraceMatch / StatsOf 本身在上面
    // 行操作那一段里逐值核对过，这里核对的是**切片偏移 → 文档坐标**的换算，以及
    // "行数/字节数问 Scintilla、字符数/词数走纯函数"这条分工。
    Section("括号匹配与摘要统计（EditView 层）");
    {
        const HWND host = CreateHiddenHost();
        npcc::edit::EditView view;
        if (host == nullptr || !view.Create(host, 952)) {
            Check(false, "创建隐藏宿主窗口或编辑控件失败");
        } else {
            view.SetCodePage(npcc::edit::kCodePageUtf8);

            // 位置是文档坐标：括号前面垫一段中文（每个汉字 3 个字节）。
            // 切片偏移没换算回去的话，这里会差 15 个字节
            {
                const std::string text = "记住：中文(f)结尾";
                const int32_t open = static_cast<int32_t>(text.find('('));
                const int32_t close = static_cast<int32_t>(text.find(')'));
                Check(open == 15 && close == 17, "这段测试文本里括号就在第 15/17 个字节");
                view.SetText(text);
                view.SetCaretPosition(open);
                const npcc::edit::BraceMatch match = view.FindBraceNearCaret();
                Check(match.hasBrace && match.matched && match.bracePos == open &&
                          match.matchPos == close,
                      "多字节字符后面的括号：位置换算回文档坐标，没差 15 个字节");
            }

            // 窗口：默认 64 KB，100 KB 之外的括号按"没配上"处理（整篇扫描会把大文件拖死）
            {
                std::string spread = "(";
                spread.append(100 * 1024, 'a');
                spread += ")";
                view.SetText(spread);
                view.SetCaretPosition(1);
                Check(!view.FindBraceNearCaret().matched, "默认 64 KB 窗口外的括号不算配上");
                Check(view.FindBraceNearCaret(200 * 1024).matched, "窗口放到 200 KB 就配得上");
            }

            // Ctrl+B 走的那条路：跳到配对的括号；没配上要老实返回 false（外壳据此提示）
            {
                view.SetText("a(b)c");
                view.SetCaretPosition(1);
                Check(view.GotoMatchingBrace(), "跳到匹配括号：成功");
                Check(view.CaretPosition() == 3, "光标落在 ')' 那个字节上");
                view.SetText("a(b");
                view.SetCaretPosition(1);
                Check(!view.GotoMatchingBrace(), "没配上：返回 false");
                Check(view.CaretPosition() == 1, "没配上时光标原地不动");
            }

            // 摘要：行数与字节数由 Scintilla 给（全量），字符数与词数走纯函数
            {
                const auto shape = [](const npcc::edit::DocumentStats& stats) {
                    return std::to_string(stats.lines) + "/" + std::to_string(stats.chars) + "/" +
                           std::to_string(stats.words) + "/" + std::to_string(stats.bytes);
                };
                view.SetText("中a\nb");   // 3 + 1 + 1 + 1 = 6 字节
                Check(shape(view.Statistics()) == "2/4/2/6",
                      "摘要的四个数：2 行 / 4 字符（换行也算）/ 2 词 / 6 字节");
                Check(!view.Statistics().truncated, "小文档不算截断");

                // ANSI 文档按代码页解释缓冲区：0x93 / 0x94 是 CP1252 的弯引号，在 UTF-8 里
                // 它们是续接字节（算 0 个字符）——所以这条能抓住"忘了看代码页"。
                // 注意 Windows 版的 Scintilla 只认 0 / UTF-8 / DBCS 代码页
                // （ScintillaWin::ValidCodePage），别的值会被静默忽略，所以这里用的是
                // ANSI 的 0 与 DBCS 的 936
                view.SetCodePage(0);
                Check(view.CodePage() == 0, "代码页 0（ANSI）真的落到控件上了");
                view.SetText("\x93Hi\x94");
                Check(view.Statistics().chars == 4, "ANSI 文档：一个字节一个字符");

                // 应用给 ANSI 文档设的就是 936（简体中文 Windows 的默认代码页），
                // 所以这条路也要走一遍：GBK 的"你好"是 4 个字节
                view.SetCodePage(936);
                Check(view.CodePage() == 936, "DBCS 代码页 936 也落得下去");
                view.SetText("\xC4\xE3\xBA\xC3");
                const npcc::edit::DocumentStats gbk = view.Statistics();
                Check(gbk.bytes == 4 && gbk.chars == 4,
                      "ANSI(GBK) 文档：缓冲区里是 4 个代码页字节，就按 4 个字符算");
                view.SetCodePage(npcc::edit::kCodePageUtf8);

                // 超过 8 MB 只数前 8 MB：字节数是全量的、字符数不是——这件事必须看得出来
                const std::int64_t huge = 8 * 1024 * 1024 + 10;
                view.SetText(std::string(static_cast<std::size_t>(huge), 'a'));
                const npcc::edit::DocumentStats big = view.Statistics();
                Check(big.bytes == huge && big.chars == 8 * 1024 * 1024 && big.truncated,
                      "超大文档：字节数全量 8 MB+10，字符数只数前 8 MB 并置 truncated");
                Check(big.lines == 1 && big.words == 1, "8 MB 个 'a' 是一行一个词");
            }

            ::DestroyWindow(host);
        }
    }

    Section("JSON 结构树（解析 / 行号 / 上限）");
    {
        using npcc::core::JsonKind;
        using npcc::core::JsonNode;
        using npcc::core::ParseJsonTree;

        // ---- 结构、名字、值、行号 ----
        {
            const char* text = "{\n  \"name\": \"npp\",\n  \"nested\": {\n    \"n\": 1,\n"
                               "    \"arr\": [true, null, \"x\"]\n  },\n  \"empty\": {}\n}\n";
            const npcc::core::JsonTreeResult result = ParseJsonTree(text);
            Check(result.ok, "合法 JSON 应当解析成功");
            Check(result.root.kind == JsonKind::Object, "根是对象");
            Check(result.root.count == 3, "根有三个键");
            Check(result.root.line == 1, "根在第 1 行");
            if (result.root.children.size() == 3) {
                const JsonNode& name = result.root.children[0];
                Check(name.name == "name" && name.kind == JsonKind::String &&
                          name.value == "npp",
                      "第一个键是字符串，名字与值都对");
                Check(name.line == 2, "字符串在第 2 行（行号是**源文本**里的）");
                const JsonNode& nested = result.root.children[1];
                Check(nested.name == "nested" && nested.kind == JsonKind::Object &&
                          nested.count == 2,
                      "嵌套对象");
                Check(nested.line == 3, "嵌套对象在第 3 行");
                if (nested.children.size() == 2) {
                    Check(nested.children[0].kind == JsonKind::Number &&
                              nested.children[0].value == "1",
                          "数字原样保留");
                    const JsonNode& array = nested.children[1];
                    Check(array.kind == JsonKind::Array && array.count == 3, "数组三个元素");
                    Check(array.children[0].kind == JsonKind::Bool &&
                              array.children[0].value == "true",
                          "true 是布尔");
                    Check(array.children[1].kind == JsonKind::Null, "null 是空值");
                    Check(array.children[2].kind == JsonKind::String, "数组里的字符串");
                    Check(array.children[2].name.empty(), "数组元素没有名字");
                }
                const JsonNode& empty = result.root.children[2];
                Check(empty.kind == JsonKind::Object && empty.count == 0, "空对象");
            }
            // 节点数：根(1) + name(1) + nested(1) + n(1) + arr(1) + arr 的三个元素(3)
            //         + empty(1) = 9
            Check(result.nodeCount == 9, "节点数数得对");
            Check(!result.truncated, "这么小的文档不会被截断");
        }

        // ---- 转义原样保留（与格式化器一个态度：显示要忠实于文件里到底是什么） ----
        {
            const npcc::core::JsonTreeResult result =
                ParseJsonTree("{\"k\":\"a\\u4e2d\\tb\"}");
            Check(result.ok, "带转义的字符串合法");
            if (result.ok && result.root.children.size() == 1) {
                Check(result.root.children[0].value == "a\\u4e2d\\tb",
                      "转义写法原样留着（没有被解成真汉字与制表符）");
            }
        }

        // ---- 顶层标量 / 空容器 ----
        {
            const npcc::core::JsonTreeResult number = ParseJsonTree("42");
            Check(number.ok && number.root.kind == JsonKind::Number && number.root.value == "42",
                  "顶层数字");
            Check(number.root.line == 1 && number.nodeCount == 1, "只有一个节点");
            const npcc::core::JsonTreeResult array = ParseJsonTree("[]");
            Check(array.ok && array.root.kind == JsonKind::Array && array.root.count == 0,
                  "空数组");
            const npcc::core::JsonTreeResult object = ParseJsonTree("{}");
            Check(object.ok && object.root.kind == JsonKind::Object, "空对象");
        }

        // ---- 非法 JSON：错误信息直接来自格式化器（含行列），两边口径一致 ----
        {
            const npcc::core::JsonTreeResult result = ParseJsonTree("{a:1}");
            Check(!result.ok, "非法 JSON 解析失败");
            Check(result.error.find(L"第 1 行第 2 列") != std::wstring::npos,
                  "错误信息带行列（与格式化器同一套）");
            Check(result.nodeCount == 0, "失败时没有节点");
            Check(!ParseJsonTree("").ok, "空内容也失败");
        }

        // ---- 节点数上限：太大就只建前一部分并标明 ----
        {
            std::string big = "[";
            for (int index = 0; index < 21000; ++index) {
                if (index > 0) {
                    big += ",";
                }
                big += "1";
            }
            big += "]";
            const npcc::core::JsonTreeResult result = ParseJsonTree(big);
            Check(result.ok, "两万多个元素的数组仍然是合法 JSON");
            Check(result.truncated, "超过上限时标明被截断");
            Check(result.nodeCount <= npcc::core::kMaxJsonNodes + 1, "节点数停在上限附近");
            Check(result.root.children.size() < 21000, "子节点确实只建了一部分");
        }

        // ---- 深嵌套不炸栈 ----
        {
            std::string deep;
            for (int index = 0; index < 200; ++index) {
                deep += "[";
            }
            deep += "1";
            for (int index = 0; index < 200; ++index) {
                deep += "]";
            }
            const npcc::core::JsonTreeResult result = ParseJsonTree(deep);
            Check(result.ok, "200 层嵌套能解析");
            Check(result.nodeCount == 201, "层数数得对");
        }
    }

    Section("文本差异比较（行级 LCS / 配对 / 两栏渲染）");
    {
        using npcc::core::CompareRenderOptions;
        using npcc::core::ComputeLineDiff;
        using npcc::core::DiffKind;
        using npcc::core::DiffRow;
        using npcc::core::DiffStats;
        using npcc::core::DisplayWidth;
        using npcc::core::RenderComparison;
        using npcc::core::SplitLines;

        // ---- 显示宽度：中文两格 ----
        Check(DisplayWidth("abc") == 3, "ASCII 一格一个");
        Check(DisplayWidth("中文") == 4, "汉字两格一个");
        Check(DisplayWidth("a中b") == 4, "混排相加");
        Check(DisplayWidth("") == 0, "空串零格");

        // ---- 切行 ----
        {
            const std::vector<std::string> lines = SplitLines("a\r\nb\nc\rd");
            Check(lines.size() == 4, "三种行尾都切");
            Check(lines[0] == "a" && lines[1] == "b" && lines[2] == "c" && lines[3] == "d",
                  "切出来的内容不含行尾符");
            Check(SplitLines("").empty(), "空内容没有行");
            Check(SplitLines("one\n").size() == 1, "结尾的换行不产生空行");
        }

        // ---- 完全相同 ----
        {
            DiffStats stats;
            const std::vector<DiffRow> rows =
                ComputeLineDiff({"a", "b", "c"}, {"a", "b", "c"}, &stats);
            Check(rows.size() == 3, "三行对三行");
            bool allEqual = true;
            for (const DiffRow& row : rows) {
                allEqual = allEqual && row.kind == DiffKind::Equal;
            }
            Check(allEqual, "全都算相同");
            Check(stats.hunks == 0, "没有不同之处");
            Check(stats.equal == 3 && stats.leftLines == 3 && stats.rightLines == 3, "统计对得上");
        }

        // ---- 只有右边多一行 ----
        {
            DiffStats stats;
            const std::vector<DiffRow> rows =
                ComputeLineDiff({"a", "c"}, {"a", "b", "c"}, &stats);
            Check(rows.size() == 3, "左边两行、右边三行 → 三行结果");
            Check(rows[0].kind == DiffKind::Equal && rows[2].kind == DiffKind::Equal,
                  "首尾相同");
            Check(rows[1].kind == DiffKind::Inserted && rows[1].rightNumber == 2 &&
                      rows[1].leftNumber == 0,
                   "中间那行是「只有右边有」，且只带右边的行号");
            Check(rows[1].rightText == "b" && rows[1].leftText.empty(), "文本只落在右边");
            Check(stats.inserted == 1 && stats.hunks == 1, "统计：新增一处");
            // 相同的行必须**留在原位**（曾经把中间段的相同行先塞进结果，全挤到前面去了）
            Check(rows[2].leftNumber == 2 && rows[2].rightNumber == 3, "最后一行的行号仍然各算各的");
        }
        {
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff({"a", "b", "c"}, {"a", "c"}, &stats);
            Check(rows.size() == 3 && rows[1].kind == DiffKind::Deleted, "只有左边有一行");
            Check(rows[1].leftNumber == 2 && rows[1].rightNumber == 0, "删除行只有左边的行号");
            Check(stats.deleted == 1 && stats.inserted == 0, "统计：删除一处");
        }

        // ---- 一改一：配成"改动行"，而不是"删一行又加一行" ----
        {
            DiffStats stats;
            const std::vector<DiffRow> rows =
                ComputeLineDiff({"a", "old", "c"}, {"a", "new", "c"}, &stats);
            Check(rows.size() == 3, "行数不变");
            Check(rows[1].kind == DiffKind::Changed, "中间那行算「改动」");
            Check(rows[1].leftText == "old" && rows[1].rightText == "new", "两侧的文本各是各的");
            Check(rows[1].leftNumber == 2 && rows[1].rightNumber == 2, "两侧行号都在");
            Check(stats.changed == 1 && stats.hunks == 1 && stats.deleted == 0 &&
                      stats.inserted == 0,
                  "统计：改动一处（不算成删除+新增）");
        }
        {
            // 两行换成三行：前两行配对成改动，多出来的那行算新增
            DiffStats stats;
            const std::vector<DiffRow> rows =
                ComputeLineDiff({"a", "x1", "x2", "z"}, {"a", "y1", "y2", "y3", "z"}, &stats);
            Check(rows.size() == 5, "五行结果");
            Check(rows[1].kind == DiffKind::Changed && rows[2].kind == DiffKind::Changed,
                  "前两行配对成改动");
            Check(rows[3].kind == DiffKind::Inserted, "多出来的那行是新增");
            Check(stats.changed == 2 && stats.inserted == 1 && stats.hunks == 1,
                  "两处改动 + 一处新增算**一处**不同（连续的一段）");
        }

        // ---- 两处相距很远的不同 = 两处 ----
        {
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff(
                {"a", "b", "c", "d", "e", "f", "g"}, {"a", "B", "c", "d", "e", "f", "G"}, &stats);
            Check(stats.hunks == 2, "中间隔着相同的行就是两处");
            Check(stats.changed == 2, "两个改动行");
        }

        // ---- 边界 ----
        {
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff({}, {"a", "b"}, &stats);
            Check(rows.size() == 2 && rows[0].kind == DiffKind::Inserted, "左边空 = 全是新增");
            Check(stats.leftLines == 0 && stats.rightLines == 2, "行数统计对");
        }
        {
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff({"a", "b"}, {}, &stats);
            Check(rows.size() == 2 && rows[1].kind == DiffKind::Deleted, "右边空 = 全是删除");
        }
        {
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff({}, {}, &stats);
            Check(rows.empty(), "两边都空 → 没有行");
            Check(stats.hunks == 0, "也没有不同");
        }
        {
            // 超过 DP 上限时退化成"整块替换"：**必须给出结果且要标出来**，
            // 不能为了精确一直算下去
            std::vector<std::string> left;
            std::vector<std::string> right;
            for (int index = 0; index < 2100; ++index) {
                left.push_back("left-" + std::to_string(index));
                right.push_back("right-" + std::to_string(index));
            }
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff(left, right, &stats);
            Check(stats.coarse, "太大时标成粗略比较");
            Check(rows.size() == 2100, "仍然一行对一行");
            Check(rows[0].kind == DiffKind::Changed, "配成改动行");
        }

        // ---- 两栏渲染 ----
        {
            DiffStats stats;
            const std::vector<DiffRow> rows =
                ComputeLineDiff({"a", "old", "c"}, {"a", "new", "c"}, &stats);
            CompareRenderOptions options;
            options.columnWidth = 20;   // 下限就是 20（见 CompareRenderOptions 的说明）
            options.leftName = "left.txt";
            options.rightName = "right.txt";
            const std::string text = RenderComparison(rows, stats, options);
            const std::vector<std::string> lines = SplitLines(text);
            Check(lines.size() >= 3 + 3, "表头两行 + 分隔 + 三行正文");
            Check(lines[0] == "比较：left.txt  ↔  right.txt", "第一行写清谁跟谁比");
            Check(lines[1] == "左 3 行，右 3 行；不同 1 处（改动 1、删除 0、新增 0）",
                  "第二行是统计");
            const std::string separator = " \xE2\x94\x82 ";   // " │ "
            const std::string expectedChanged =
                "~" + std::string("    2 ") + "old" + std::string(17, ' ') + separator +
                std::string("    2 ") + "new" + std::string(17, ' ');
            CheckEqual(lines[4], expectedChanged, "改动行：两侧行号 + 两侧文本，按列对齐");
            const std::string expectedEqual =
                " " + std::string("    1 ") + "a" + std::string(19, ' ') + separator +
                std::string("    1 ") + "a" + std::string(19, ' ');
            CheckEqual(lines[3], expectedEqual, "相同行不写记号");
        }
        {
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff({"only-left"}, {}, &stats);
            CompareRenderOptions options;
            options.columnWidth = 20;
            options.leftName = "a";
            options.rightName = "b";
            const std::vector<std::string> lines = SplitLines(RenderComparison(rows, stats, options));
            const std::string separator = " \xE2\x94\x82 ";
            const std::string expected =
                "-" + std::string("    1 ") + "only-left" + std::string(11, ' ') + separator +
                std::string(6, ' ') + std::string(20, ' ');
            CheckEqual(lines[3], expected, "只有左边有的行：右边整栏留空（行号位置也留空）");
        }
        {
            // 太长的行要截断并留记号，而且两栏的宽度必须仍然一致
            DiffStats stats;
            const std::vector<DiffRow> rows =
                ComputeLineDiff({std::string(50, 'x')}, {std::string(50, 'y')}, &stats);
            CompareRenderOptions options;
            options.columnWidth = 20;
            const std::vector<std::string> lines = SplitLines(RenderComparison(rows, stats, options));
            const std::string body = lines[3];
            Check(body.find("\xE2\x80\xA6") != std::string::npos, "截断处有记号（…）");
            // 一行的显示宽度 = 记号 1 + 行号 6 + 左栏 + 分隔 3 + 行号 6 + 右栏
            Check(DisplayWidth(body) == 1 + 6 + 20 + 3 + 6 + 20, "两栏宽度各按列宽算");
        }
        {
            // 中文按两格对齐：截断与补齐都要按显示宽度走，不然两栏会歪
            DiffStats stats;
            const std::vector<DiffRow> rows = ComputeLineDiff({"中文一行"}, {"中文两行"}, &stats);
            CompareRenderOptions options;
            options.columnWidth = 14;   // 会被夹到下限 20
            const std::vector<std::string> lines = SplitLines(RenderComparison(rows, stats, options));
            Check(DisplayWidth(lines[3]) == 1 + 6 + 20 + 3 + 6 + 20,
                  "含中文的行宽度也算得对（列宽被夹到下限 20）");
        }
    }

    Section("Markdown 解析与渲染（预览面板与浏览器预览共用）");
    {
        using npcc::core::BlockPlainLines;
        using npcc::core::MarkdownBlock;
        using npcc::core::MarkdownBlockKind;
        using npcc::core::MarkdownSpanKind;
        using npcc::core::MarkdownToHtml;
        using npcc::core::ParseInline;
        using npcc::core::ParseMarkdown;

        // ---- 行内 ----
        {
            const std::vector<npcc::core::MarkdownSpan> spans = ParseInline("**粗**和*斜*和`码`");
            Check(spans.size() == 5, "粗体/斜体/行内代码之间被切成五段");
            Check(spans[0].kind == MarkdownSpanKind::Bold && spans[0].text == "粗", "**粗体**");
            Check(spans[2].kind == MarkdownSpanKind::Italic && spans[2].text == "斜", "*斜体*");
            Check(spans[4].kind == MarkdownSpanKind::Code && spans[4].text == "码", "行内代码");
        }
        {
            const std::vector<npcc::core::MarkdownSpan> spans =
                ParseInline("看[这里](https://example.com/a?b=1)吧");
            Check(spans.size() == 3, "链接前后各一段普通文本");
            Check(spans[1].kind == MarkdownSpanKind::Link && spans[1].text == "这里" &&
                      spans[1].target == "https://example.com/a?b=1",
                  "链接的文字与地址都取对了");
        }
        {
            const std::vector<npcc::core::MarkdownSpan> spans = ParseInline("\\*不是斜体\\*");
            Check(spans.size() == 1 && spans[0].kind == MarkdownSpanKind::Text &&
                      spans[0].text == "*不是斜体*",
                  "反斜杠转义之后是普通文本");
        }
        {
            // 没有收尾的定界符就当普通字符：不能把整行吞成强调
            const std::vector<npcc::core::MarkdownSpan> spans = ParseInline("**没有收尾");
            Check(spans.size() == 1 && spans[0].kind == MarkdownSpanKind::Text,
                  "没收尾的 ** 是普通文本");
            const std::vector<npcc::core::MarkdownSpan> empty = ParseInline("");
            Check(empty.empty(), "空行的行内解析结果是空的");
        }

        // ---- 块 ----
        {
            const std::vector<MarkdownBlock> blocks =
                ParseMarkdown("# 一级\n\n### 三级 ###\n\n#没有空格\n");
            Check(blocks.size() == 3, "三个块");
            Check(blocks[0].kind == MarkdownBlockKind::Heading && blocks[0].level == 1,
                  "# 是一级标题");
            Check(blocks[1].kind == MarkdownBlockKind::Heading && blocks[1].level == 3 &&
                      blocks[1].spans.size() == 1 && blocks[1].spans[0].text == "三级",
                  "结尾的 # 被去掉");
            Check(blocks[2].kind == MarkdownBlockKind::Paragraph,
                  "# 后面没有空格就不是标题");
        }
        {
            const std::vector<MarkdownBlock> blocks = ParseMarkdown("第一行\n第二行\n\n另一端\n");
            Check(blocks.size() == 2 && blocks[0].kind == MarkdownBlockKind::Paragraph,
                  "连续两行合成一段");
            Check(blocks[0].spans.size() == 1 && blocks[0].spans[0].text == "第一行 第二行",
                  "合并时中间补一个空格");
        }
        {
            const std::vector<MarkdownBlock> blocks =
                ParseMarkdown("```c\nint main(void) {\n  return 0;\n}\n```\n尾部\n");
            Check(blocks.size() == 2, "代码块之后还有一段");
            Check(blocks[0].kind == MarkdownBlockKind::Code, "围栏代码块");
            Check(blocks[0].code == "int main(void) {\n  return 0;\n}", "代码块内容原样保留");
            Check(blocks[1].kind == MarkdownBlockKind::Paragraph, "围栏之后的普通段落");
        }
        {
            const std::vector<MarkdownBlock> blocks = ParseMarkdown("~~~\ncode\n~~~\n");
            Check(blocks.size() == 1 && blocks[0].kind == MarkdownBlockKind::Code,
                  "波浪线围栏也认");
            // 没关的围栏：吃到文件结尾，不能丢内容
            const std::vector<MarkdownBlock> unclosed = ParseMarkdown("```\nabc\n");
            Check(unclosed.size() == 1 && unclosed[0].kind == MarkdownBlockKind::Code &&
                      unclosed[0].code == "abc",
                  "没有收尾的围栏吃到文件尾");
        }
        {
            const std::vector<MarkdownBlock> blocks =
                ParseMarkdown("- 一\n- 二\n  - 缩进\n1. 甲\n2) 乙\n");
            Check(blocks.size() == 5, "五个列表项");
            Check(blocks[0].kind == MarkdownBlockKind::List && !blocks[0].ordered &&
                      blocks[0].level == 0,
                  "无序列表");
            Check(blocks[2].level == 1, "缩进两格算一级");
            Check(blocks[3].ordered && blocks[3].number == 1, "有序列表的序号");
            Check(blocks[4].ordered && blocks[4].number == 2, "「2)」这种写法也认");
        }
        {
            const std::vector<MarkdownBlock> blocks = ParseMarkdown("> 引用一\n> 引用二\n");
            Check(blocks.size() == 2 && blocks[0].kind == MarkdownBlockKind::Quote &&
                      blocks[0].spans[0].text == "引用一",
                  "引用块去掉了 > 与前导空格");
        }
        {
            const std::vector<MarkdownBlock> blocks = ParseMarkdown("---\n\n***\n\n___\n\n- - -\n");
            Check(blocks.size() == 4, "四种写法的分隔线都是分隔线");
            for (const MarkdownBlock& block : blocks) {
                Check(block.kind == MarkdownBlockKind::Rule, "分隔线");
            }
            const std::vector<MarkdownBlock> notRule = ParseMarkdown("---abc\n");
            Check(notRule[0].kind == MarkdownBlockKind::Paragraph, "混了别的字符就不是分隔线");
        }
        Check(ParseMarkdown("").empty(), "空内容没有块");
        Check(ParseMarkdown("\n\n   \n").empty(), "只有空白的行不算块");

        // ---- 摊成纯文本（面板显示什么，这里就是什么） ----
        {
            const std::vector<MarkdownBlock> blocks = ParseMarkdown("- 项\n\n> 引\n\n`码`\n");
            Check(BlockPlainLines(blocks[0]).size() == 1 &&
                      BlockPlainLines(blocks[0])[0] == "\xE2\x80\xA2 项",
                  "无序列表项带「• 」前缀");
            Check(BlockPlainLines(blocks[1])[0] == "\xE2\x94\x82 引", "引用带「│ 」前缀");
            Check(BlockPlainLines(blocks[2])[0] == "码", "段落就是文字本身");
            const std::vector<MarkdownBlock> ordered = ParseMarkdown("7. 第七\n");
            Check(BlockPlainLines(ordered[0])[0] == "7. 第七", "有序列表项带序号前缀");
        }

        // ---- HTML ----
        {
            const std::string html = MarkdownToHtml(
                "# 标题\n\n一段 **粗** 与 `码` 与 [链](https://a.b)。\n\n- 项\n\n> 引一\n> 引二\n\n"
                "```\n代码\n```\n\n---\n\n<b>原文里的标签</b>\n");
            Check(html.find("<!DOCTYPE html>") == 0, "是一份独立的 HTML");
            Check(html.find("<meta charset=\"utf-8\">") != std::string::npos, "声明了 UTF-8");
            Check(html.find("<h1>标题</h1>") != std::string::npos, "一级标题渲染成 h1");
            Check(html.find("<strong>粗</strong>") != std::string::npos, "粗体渲染成 strong");
            Check(html.find("<code>码</code>") != std::string::npos, "行内代码渲染成 code");
            Check(html.find("<a href=\"https://a.b\">链</a>") != std::string::npos, "链接");
            Check(html.find("<ul>") != std::string::npos && html.find("<li") != std::string::npos,
                  "列表");
            Check(html.find("<pre><code>代码</code></pre>") != std::string::npos, "代码块");
            Check(html.find("<hr>") != std::string::npos, "分隔线");
            // 连续的引用合成一个 blockquote：一行一个会渲染成好几条互不相干的引文
            Check(html.find("<blockquote>") != std::string::npos, "引用");
            Check(html.find("<blockquote>") == html.rfind("<blockquote>"),
                  "连续两行引用只出一个 blockquote");
            Check(html.find("引一<br>") != std::string::npos, "引用内部换行用 <br>");
            // 正文里的尖括号必须转义，否则用户文档里的标签会被浏览器当标签执行
            Check(html.find("&lt;b&gt;原文里的标签&lt;/b&gt;") != std::string::npos,
                  "正文里的 < > 被转义");
            Check(html.find("<b>原文里的标签</b>") == std::string::npos,
                  "正文里的标签没有被当成 HTML");
        }
        {
            const std::string html = MarkdownToHtml("");
            Check(html.find("<body>") != std::string::npos && html.find("</html>") != std::string::npos,
                  "空文档也是一份完整的 HTML");
        }
    }

    Section("复制为富文本（选区样式文本 / 代码页转换 / RTF）");
    {
        using npcc::core::ExportOptions;
        using npcc::core::ExportStyle;
        using npcc::core::StyledByte;

        // ---- ReadStyledText：读回来的**字节数**必须正好是请求的那一段 ----
        // 这一条钉的是一个真实缺陷：SCI_GETSTYLEDTEXT 返回的是"写入的字节数"（每位置两个
        // 字节），按它直接当字符数循环会在每次读取的末尾多出一倍长度的 '\0'。导出 HTML/RTF
        // 的文件末尾一直拖着这些空字符，只是从来没有人核对过文件尾巴
        const HWND styledHost = CreateHiddenHost();
        npcc::edit::EditView view;
        if (styledHost == nullptr) {
            Check(false, "创建隐藏宿主窗口失败");
        } else if (view.Create(styledHost, 941)) {
            view.ApplyLightTheme();
            view.SetCodePage(npcc::edit::kCodePageUtf8);
            const char* source = "// 中文注释\nint value = 42;\n";
            view.SetText(source);
            view.ApplyLanguageForPath(L"D:\\t\\a.cpp");
            {
                std::string text;
                std::vector<int> styles;
                view.ReadStyledText(0, 6, text, styles);
                Check(text.size() == 6, "读回 6 个字节就是 6 个字节（中文一个字三字节）");
                CheckEqual(text, "// 中", "读回的正好是请求的那一段（6 个字节到「中」为止）");
                Check(styles.size() == text.size(), "样式号与字节一一对应");
            }
            {
                const int32_t length = static_cast<int32_t>(view.ByteLength());
                std::string text;
                std::vector<int> styles;
                view.ReadStyledText(0, length, text, styles);
                Check(text.size() == static_cast<std::size_t>(length),
                      "整篇读回来的长度与文档长度一致（末尾不多出空字符）");
                Check(text.find('\0') == std::string::npos, "正文里一个 NUL 都不该有");
                Check(text.size() > 0 && text.back() == '\n', "最后那个字节就是原文的换行");
                // 注释与代码的样式号应当不同：不同的话说明样式确实是按字节带回来的
                bool distinct = false;
                for (const int style : styles) {
                    if (style != styles[0]) {
                        distinct = true;
                        break;
                    }
                }
                Check(distinct, "注释与代码拿到了不同的样式号");
            }
            ::DestroyWindow(styledHost);
        } else {
            Check(false, "创建编辑控件失败");
        }

        // ---- 代码页转换：ANSI（GBK）文档的中文要变成正确的 UTF-8 ----
        {
            // "中文" 的 GBK 字节是 D6 D0 CE C4
            const std::vector<StyledByte> gbk = {
                {static_cast<char>(0xD6), 2}, {static_cast<char>(0xD0), 2},
                {static_cast<char>(0xCE), 2}, {static_cast<char>(0xC4), 2}};
            const std::vector<StyledByte> utf8 = npcc::core::StyledBytesAsUtf8(gbk, 936);
            Check(utf8.size() == 6, "一个汉字三字节（两个汉字六个字节）");
            Check(utf8[0].style == 2 && utf8[5].style == 2, "样式号跟着字符走");
            std::string text;
            for (const StyledByte& byte : utf8) {
                text.push_back(byte.ch);
            }
            CheckEqual(text, "中文", "GBK 的两个汉字解成了正确的 UTF-8");
            // 同一个汉字在 UTF-8 下正好也是这三字节，正好用来核对
            Check(static_cast<unsigned char>(utf8[0].ch) == 0xE4 &&
                      static_cast<unsigned char>(utf8[2].ch) == 0xAD,
                  "中 = E4 B8 AD");
        }
        {
            // ASCII 原样；UTF-8 的文档走直通路径
            const std::vector<StyledByte> ascii = {{'a', 1}, {'b', 1}, {' ', 0}};
            const std::vector<StyledByte> converted = npcc::core::StyledBytesAsUtf8(ascii, 936);
            Check(converted.size() == 3 && converted[0].ch == 'a' && converted[2].ch == ' ',
                  "纯 ASCII 的 ANSI 文档原样不动");
            const std::vector<StyledByte> direct = npcc::core::StyledBytesAsUtf8(ascii, 65001);
            Check(direct.size() == ascii.size() && direct[1].ch == 'b',
                  "UTF-8 的文档直通（不折腾）");
            Check(npcc::core::StyledBytesAsUtf8({}, 936).empty(), "空输入返回空");
        }
        {
            // 截断的前导字节不能把程序带崩：原样留着，至少不丢内容
            const std::vector<StyledByte> broken = {{'a', 0}, {static_cast<char>(0xD6), 0}};
            const std::vector<StyledByte> out = npcc::core::StyledBytesAsUtf8(broken, 936);
            Check(!out.empty(), "半截的序列不会让结果消失");
            Check(out[0].ch == 'a', "前面的 ASCII 照常转换");
        }
        {
            // 走完整条链：GBK 的"中文"经过转换之后，RTF 里应当是 \u20013?\u25991?
            std::vector<StyledByte> bytes = npcc::core::StyledBytesAsUtf8(
                {{static_cast<char>(0xD6), 1}, {static_cast<char>(0xD0), 1},
                 {static_cast<char>(0xCE), 1}, {static_cast<char>(0xC4), 1}},
                936);
            ExportOptions options;
            options.lineNumbers = false;
            const std::vector<ExportStyle> styles = {{1, 0x000000, false, false}};
            const std::string rtf = npcc::core::BuildRtf(bytes, styles, options);
            Check(rtf.find("\\u20013?") != std::string::npos,
                  "GBK 里的「中」在 RTF 里是正确的那个字符");
            Check(rtf.find("\\u25991?") != std::string::npos, "「文」也一样");
            Check(rtf.find("\\u0?") == std::string::npos, "RTF 里不该出现 NUL 字符");
        }
    }

    Section("十六进制视图的排版与解析（HexModel）");
    {
        using npcc::edit::HexBlockText;
        using npcc::edit::HexByteAtColumn;
        using npcc::edit::HexRowText;
        using npcc::edit::ParseHexJump;

        // ---- 偏移宽度 ----
        Check(npcc::edit::HexOffsetWidthFor(1024) == 8, "小文件的偏移是 8 位");
        Check(npcc::edit::HexOffsetWidthFor(0x100000000ull) == 16, "超过 4 GB 的偏移要 16 位");
        CheckEqual(npcc::edit::FormatHexOffset(0x1F), "0000001F", "偏移是大写定宽十六进制");
        CheckEqual(npcc::edit::FormatHexOffset(0x1F, 16), "000000000000001F", "可以指定宽度");

        // ---- 一整行：偏移 + 十六进制 + ASCII ----
        const std::uint8_t row[16] = {0x4A, 0x6F, 0x20, 0x62, 0x69, 0x6E, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
        const std::string line = HexRowText(row, 16, 0);
        CheckEqual(line,
                   "00000000  4A 6F 20 62 69 6E 00 00  00 00 00 00 00 00 00 FF  Jo bin..........",
                   "整行的排版");
        Check(line.size() == static_cast<std::size_t>(npcc::edit::kHexRowWidth),
              "整行的宽度就是 kHexRowWidth");

        // ---- 不满一行：十六进制列要用空格补齐，ASCII 仍然对得齐 ----
        const std::uint8_t tail[4] = {0x7F, 0x80, 't', 'a'};
        const std::string partial = HexRowText(tail, 4, 0x20);
        // 不满一行的行文本比整行短（后面没有多余的空白），但**列**仍然对得齐：
        // ASCII 一定从 kHexAsciiStart 那一列开始
        Check(partial.size() == static_cast<std::size_t>(npcc::edit::kHexAsciiStart) + 4,
              "不满一行时 ASCII 仍然落在同一列上");
        Check(npcc::edit::kHexRowWidth == npcc::edit::kHexAsciiStart + npcc::edit::kHexBytesPerRow,
              "整行宽度 = ASCII 起点 + 每行字节数");
        Check(partial.compare(0, 21, "00000020  7F 80 74 61") == 0, "末行的偏移与字节");
        CheckEqual(partial.substr(static_cast<std::size_t>(npcc::edit::kHexAsciiStart)), "..ta",
                   "末行的 ASCII 从头开始，不跟着字节数左移");
        Check(partial.find('\x7F') == std::string::npos, "0x7F 不原样输出");
        {
            const std::uint8_t edges[6] = {0x1F, 0x20, 0x7E, 0x7F, 0x80, 0xFF};
            const std::string text = HexRowText(edges, 6, 0);
            CheckEqual(text.substr(static_cast<std::size_t>(npcc::edit::kHexAsciiStart), 6),
                       ". ~...", "0x20 与 0x7E 是可打印的两端，两侧都替成点");
        }

        // ---- 多行拼块 ----
        std::uint8_t many[40] = {};
        for (int i = 0; i < 40; ++i) {
            many[i] = static_cast<std::uint8_t>(i + 1);
        }
        {
            const std::string block = HexBlockText(many, sizeof(many), 0, sizeof(many));
            Check(block.find("\r\n") != std::string::npos, "块里行间用 \\r\\n");
            const std::size_t lines =
                static_cast<std::size_t>(std::count(block.begin(), block.end(), '\n')) + 1;
            Check(lines == 3, "40 字节排成三行");
            Check(block.compare(0, 8, "00000000") == 0, "第一行从偏移 0 开始");
        }
        {
            // 从中间取一段：左边那一列要写真实偏移，而不是从 0 重新数
            const std::string block = HexBlockText(many, sizeof(many), 20, 8);
            Check(block.compare(0, 21, "00000014  15 16 17 18") == 0,
                  "从第 20 个字节开始（0x14），值是 21..24");
        }
        {
            // 要的比有的多：按实际长度截断，读不到文件尾之外
            const std::string block = HexBlockText(many, sizeof(many), 32, 1024);
            Check(block.compare(0, 8, "00000020") == 0, "从 0x20 开始");
            Check(std::count(block.begin(), block.end(), '\n') == 0, "只剩一行");
        }
        Check(HexBlockText(many, sizeof(many), 100, 16).empty(), "起点越界时返回空");
        Check(HexBlockText(nullptr, 0, 0, 16).empty(), "没有数据时返回空");

        // ---- 跳到偏移的输入 ----
        {
            std::uint64_t offset = 999;
            Check(ParseHexJump(L"0x1F", offset) && offset == 0x1F, "0x1F 是十六进制");
            Check(ParseHexJump(L"0X1f", offset) && offset == 0x1F, "大小写与 0X 都认");
            Check(ParseHexJump(L"  0x200  ", offset) && offset == 0x200, "首尾空白忽略");
            Check(ParseHexJump(L"512", offset) && offset == 512, "不带前缀的按十进制");
            Check(ParseHexJump(L"0", offset) && offset == 0, "0 合法");
            offset = 999;
            Check(!ParseHexJump(L"", offset), "空串不合法");
            Check(!ParseHexJump(L"   ", offset), "只有空白不合法");
            Check(!ParseHexJump(L"0x", offset), "只有前缀不合法");
            Check(!ParseHexJump(L"1F", offset), "不带 0x 的字母不当十六进制（免得「10」靠猜）");
            Check(!ParseHexJump(L"0x1G", offset), "非法字符不合法");
            Check(!ParseHexJump(L"12 34", offset), "中间有空格不合法");
            Check(!ParseHexJump(L"0xFFFFFFFFFFFFFFFFFF", offset), "溢出不合法");
            Check(offset == 999, "解析失败时不动 out");
        }

        // ---- 鼠标列 → 本行第几个字节 ----
        {
            int index = -1;
            Check(HexByteAtColumn(npcc::edit::kHexTextStart, 16, &index) && index == 0,
                  "十六进制区第一列是第 0 个字节");
            Check(HexByteAtColumn(npcc::edit::kHexTextStart + 13, 16, &index) && index == 4,
                  "每三个字符一个字节");
            Check(HexByteAtColumn(npcc::edit::kHexTextStart + 21, 16, &index) && index == 7,
                  "第 8 个字节");
            Check(HexByteAtColumn(npcc::edit::kHexTextStart + 25, 16, &index) && index == 8,
                  "空档之后整体挪了一列，换算要跟着挪");
            Check(HexByteAtColumn(npcc::edit::kHexTextStart + 46, 16, &index) && index == 15,
                  "最后一个字节");
            Check(HexByteAtColumn(npcc::edit::kHexAsciiStart, 16, &index) && index == 0,
                  "ASCII 区第一列也是第 0 个字节");
            Check(HexByteAtColumn(npcc::edit::kHexAsciiStart + 15, 16, &index) && index == 15,
                  "ASCII 区最后一列");
            Check(!HexByteAtColumn(0, 16, &index), "偏移那一列不是字节");
            Check(!HexByteAtColumn(npcc::edit::kHexTextStart - 1, 16, &index),
                  "偏移与十六进制之间的空格不算");
            Check(!HexByteAtColumn(npcc::edit::kHexAsciiStart + 17, 16, &index), "ASCII 右边不算");
            Check(HexByteAtColumn(npcc::edit::kHexTextStart + 3, 4, &index) && index == 1,
                  "末行里真实存在的字节照样能点");
            Check(!HexByteAtColumn(npcc::edit::kHexTextStart + 16, 4, &index),
                  "末行补齐的空档点不出字节");
            Check(!HexByteAtColumn(npcc::edit::kHexAsciiStart + 5, 4, &index),
                  "末行 ASCII 之后的位置也点不出来");
            Check(!HexByteAtColumn(npcc::edit::kHexTextStart, 0, &index), "没有字节的行点不出东西");
        }
    }

    Section("界面语言（语言包加载 / 回退链 / 覆盖自检）");
    {
        using npcc::core::LangEntry;
        using npcc::core::LangPack;
        using npcc::core::Localization;
        using npcc::core::ParseLangPack;

        // ---- 内置英文包：内容自检 ----
        {
            const std::vector<LangPack>& packs = npcc::core::BuiltinLangPacks();
            Check(!packs.empty(), "至少有一套内置语言包");
            Check(packs[0].locale == L"en-US", "内置的第一套是 en-US");
            Check(packs[0].entries.size() > 100, "内置英文包覆盖了上百条键");
            bool unique = true;
            bool wellFormed = true;
            for (std::size_t i = 0; i < packs[0].entries.size(); ++i) {
                const LangEntry& entry = packs[0].entries[i];
                if (entry.key.empty() || entry.value.empty()) {
                    wellFormed = false;
                }
                // 键的前缀就那么几种：写错前缀的键永远不会被查到（cat.* 是菜单类别、
                // cmd.* 是命令、prefs.* 是首选项对话框）
                const bool knownPrefix = entry.key.compare(0, 4, L"cmd.") == 0 ||
                                         entry.key.compare(0, 4, L"cat.") == 0 ||
                                         entry.key.compare(0, 6, L"prefs.") == 0 ||
                                         entry.key.compare(0, 8, L"compare.") == 0;
                if (!knownPrefix) {
                    wellFormed = false;
                }
                for (std::size_t j = i + 1; j < packs[0].entries.size(); ++j) {
                    if (packs[0].entries[j].key == entry.key) {
                        unique = false;
                    }
                }
            }
            Check(wellFormed, "内置英文包每条都有键有值、键前缀合法");
            Check(unique, "内置英文包没有重复键");
            // 助记符要么空、要么就是一个字母
            bool accelOk = true;
            for (const LangEntry& entry : packs[0].entries) {
                if (!entry.accel.empty() && entry.accel.size() != 1) {
                    accelOk = false;
                }
            }
            Check(accelOk, "助记符要么空要么一个字母");
        }

        // ---- 解析与容错 ----
        {
            npcc::infra::XmlDocument document;
            std::wstring error;
            const std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<LangPack locale=\"xx-XX\" name=\"测试语言\">\n"
                "  <Entry key=\"cmd.file.save\" value=\"Guardar\" accel=\"G\"/>\n"
                "  <Entry key=\"cmd.file.open\" value=\"Abrir\"/>\n"
                "  <Entry value=\"没有键\"/>\n"
                "  <Entry key=\"cmd.file.new\"/>\n"
                "  <Entry key=\"cmd.file.save\" value=\"Guardar2\"/>\n"
                "</LangPack>\n";
            Check(npcc::infra::XmlDocument::Parse(xml, document, &error), "解析语言包 XML");
            LangPack pack;
            std::vector<std::wstring> warnings;
            Check(ParseLangPack(*document.Root(), pack, &warnings), "解析语言包内容");
            Check(pack.locale == L"xx-XX" && pack.name == L"测试语言", "locale 与显示名读对了");
            Check(pack.entries.size() == 2, "缺键或缺值的条目被跳过");
            // 两条坏条目（缺键、缺值）各一条，再加"同一个键出现两次"那条，共三条
            Check(warnings.size() == 3, "坏条目与重复键各给一条警告");
            Check(pack.Find(L"cmd.file.new") == nullptr, "缺 value 的条目没有进包");
            if (const LangEntry* entry = pack.Find(L"cmd.file.save"); entry != nullptr) {
                CheckWide(entry->value, L"Guardar2", "同一个键出现多次以最后一条为准");
            } else {
                Check(false, "cmd.file.save 应当在包里");
            }
        }
        {
            // 根元素不对要返回失败，而不是一个空包
            npcc::infra::XmlDocument document;
            std::wstring error;
            Check(npcc::infra::XmlDocument::Parse("<NotLangPack/>", document, &error),
                  "解析测试 XML");
            LangPack pack;
            Check(!ParseLangPack(*document.Root(), pack, nullptr), "根元素不是 LangPack 时失败");
        }
        {
            // 往返：值里有引号、&、尖括号、中文，写出去再读回来必须一字不差
            LangPack pack;
            pack.locale = L"xx-XX";
            pack.name = L"往返测试";
            pack.entries = {{L"cmd.tool.sortAsc", L"Sort <A> & \"B\"", L"A"},
                            {L"cat.tools", L"工具 & Tools", L""}};
            const std::string xml = npcc::core::LangPackToXml(pack);
            npcc::infra::XmlDocument document;
            std::wstring error;
            Check(npcc::infra::XmlDocument::Parse(xml, document, &error), "导出的 XML 能被解析");
            LangPack back;
            std::vector<std::wstring> warnings;
            Check(ParseLangPack(*document.Root(), back, &warnings), "导出再读回来");
            Check(warnings.empty(), "往返不产生警告");
            Check(back.entries.size() == 2, "条目数一致");
            if (back.entries.size() == 2) {
                CheckWide(back.entries[0].value, L"Sort <A> & \"B\"", "带转义的值往返一致");
                CheckWide(back.entries[0].accel, L"A", "助记符往返一致");
                CheckWide(back.entries[1].value, L"工具 & Tools", "中文与 & 往返一致");
            }
        }

        // ---- 回退链与助记符拼装（用 Localization 真正装一次包） ----
        {
            Localization& ui = Localization::Ui();
            ui.Load(L"zh-CN", {}, nullptr);
            Check(ui.ChineseDefault(), "默认用代码里的简体中文");
            Check(!ui.Covers(L"cmd.file.save"), "中文这一档没有包，所以什么都不算被覆盖");
            Check(ui.ActivePack() == nullptr, "中文这一档没有活动包");
            CheckWide(ui.Tr(L"cmd.file.save", L"保存(&S)"), L"保存(&S)",
                      "包缺失时用代码里的默认文本");
            CheckWide(ui.Tr(L"msg.unknown", L""), L"msg.unknown",
                      "既没有包也没有默认文本时显示键本身");

            ui.Load(L"en-US", {}, nullptr);
            Check(ui.Locale() == L"en-US", "选中 en-US");
            CheckWide(ui.Tr(L"cmd.file.save", L"保存(&S)"), L"Save(&S)",
                      "包命中：值 + 助记符");
            CheckWide(ui.Tr(L"cmd.file.open", L"打开(&O)..."), L"Open(&O)...",
                      "助记符插在省略号前面");
            CheckWide(ui.Tr(L"cmd.format.eolCrlf", L"转为 Windows 行尾（CRLF）"),
                      L"Convert to Windows EOL (CRLF)", "没有助记符的条目只换文本");
            Check(ui.Covers(L"cmd.file.save"), "包里的键算被覆盖");
            Check(!ui.Covers(L"cmd.nonexistent"), "包外的键不算被覆盖");
            Check(!ui.ChineseDefault(), "en-US 不是中文默认");
            // 占位符：整句交给语言包，参数按 {0} {1} 替换
            CheckWide(ui.Tr(L"cmd.file.save", L"保存(&S)", {}), L"Save(&S)",
                      "不带参数的调用与单参版本一致");
            ui.Load(L"zh-CN", {}, nullptr);
            CheckWide(ui.Tr(L"prefs.error.range", L"「{0}」必须是 {1} 到 {2} 之间的整数，"
                                                L"当前输入「{3}」。",
                            {L"制表符宽度", L"1", L"64", L"0"}),
                      L"「制表符宽度」必须是 1 到 64 之间的整数，当前输入「0」。",
                      "中文默认文本里的占位符被替换");
            ui.Load(L"en-US", {}, nullptr);
            CheckWide(ui.Tr(L"prefs.error.range", L"「{0}」必须是 {1} 到 {2} 之间的整数。",
                            {L"Tab width", L"1", L"64", L"0"}),
                      L"\"Tab width\" must be a whole number between 1 and 64; the current "
                      L"input is \"0\".",
                      "英文包里的整句也照样替换（语序由译文决定）");
            CheckWide(ui.Tr(L"cmd.file.save", L"保存", {L"多余参数"}),
                      L"Save(&S)", "没有占位符时多余参数被忽略");
            CheckWide(ui.Tr(L"test.placeholder.only", L"{0}和{1}", {L"甲"}),
                      L"甲和{1}", "参数不够时没替换的占位符原样保留（看得见才好修）");
            // 类别名也走这里（菜单栏那一排）
            CheckWide(npcc::core::CategoryText(npcc::core::CmdCategory::File), L"File",
                      "en-US 下类别名是英文");
            Check(npcc::core::CategoryMnemonic(npcc::core::CmdCategory::File) == L'F',
                  "类别助记符不随语言变（本来就是拉丁字母）");
            ui.Load(L"zh-CN", {}, nullptr);
            CheckWide(npcc::core::CategoryText(npcc::core::CmdCategory::File), L"文件",
                      "切回中文之后类别名回到中文");
        }

        // ---- 外置语言包：覆盖内置、认不出来就回退 ----
        npcc::infra::InitializePaths(true);
        const std::filesystem::path langDir = npcc::infra::TempDir() / L"lang_test";
        std::error_code langEc;
        std::filesystem::create_directories(langDir, langEc);
        std::filesystem::remove(langDir / L"xx-XX.lang.xml", langEc);
        std::filesystem::remove(langDir / L"en-US.lang.xml", langEc);
        {
            const std::string custom =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<LangPack locale=\"xx-XX\" name=\"Esperanto\">\n"
                "  <Entry key=\"cmd.file.save\" value=\"Konservi\" accel=\"K\"/>\n"
                "  <Entry key=\"cat.file\" value=\"Dosiero\"/>\n"
                "</LangPack>\n";
            Check(WriteFileDirect(langDir / L"xx-XX.lang.xml", custom), "写入外置语言包");
            Localization& ui = Localization::Ui();
            std::vector<std::wstring> warnings;
            ui.Load(L"xx-XX", langDir, &warnings);
            Check(ui.Locale() == L"xx-XX", "选中外置语言包的语言");
            CheckWide(ui.Tr(L"cmd.file.save", L"保存(&S)"), L"Konservi(&K)", "外置包生效");
            CheckWide(ui.Tr(L"cmd.file.new", L"新建(&N)"), L"新建(&N)",
                      "外置包没写的键回退到简体中文");
            const auto available = ui.Available();
            Check(available.size() >= 3, "可选语言里有中文、内置英文与外置的这一套");
            Check(available[0].first == L"zh-CN", "第一项固定是简体中文");
            bool hasBuiltin = false;
            bool hasCustom = false;
            for (const auto& item : available) {
                if (item.first == L"en-US") {
                    hasBuiltin = true;
                }
                if (item.first == L"xx-XX" && item.second == L"Esperanto") {
                    hasCustom = true;
                }
            }
            Check(hasBuiltin, "内置英文在可选语言里");
            Check(hasCustom, "外置语言的显示名来自它自己的 name 属性");
        }
        {
            // 同 locale 的外置包覆盖内置包
            const std::string custom =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<LangPack locale=\"en-US\" name=\"English (mine)\">\n"
                "  <Entry key=\"cmd.file.save\" value=\"Store\"/>\n"
                "</LangPack>\n";
            Check(WriteFileDirect(langDir / L"en-US.lang.xml", custom), "写入覆盖 en-US 的包");
            Localization& ui = Localization::Ui();
            std::vector<std::wstring> warnings;
            ui.Load(L"en-US", langDir, &warnings);
            CheckWide(ui.Tr(L"cmd.file.save", L"保存(&S)"), L"Store", "外置的同名包盖住了内置那套");
            // 覆盖是**逐条**的：用户改过的行以他为准，没动过的行跟着程序升级走。
            // 整体替换的话，用户一旦留了一份旧模板，每个版本新增的键都会被它挡住
            CheckWide(ui.Tr(L"cmd.file.open", L"打开(&O)..."), L"Open(&O)...",
                      "没写在外置包里的键仍然来自内置包（逐条覆盖，不是整体替换）");
            CheckWide(ui.Tr(L"prefs.title", L"首选项"), L"Preferences",
                      "内置包里新增的键不会被旧的外置包挡住");
            // 缺键自报：真用到过、而当前语言包没盖住的键会被记下来
            ui.ClearMissedKeys();
            Check(ui.MissedKeys().empty(), "清空之后没有记录");
            (void)ui.Tr(L"prefs.not.translated.yet", L"还没翻的文案");
            Check(ui.MissedKeys().size() == 1 &&
                      ui.MissedKeys()[0] == L"prefs.not.translated.yet",
                  "包没盖住的键被记下来（英文界面里冒出中文时靠它定位）");
            (void)ui.Tr(L"prefs.not.translated.yet", L"还没翻的文案");
            Check(ui.MissedKeys().size() == 1, "同一个键只记一次");
            ui.Load(L"zh-CN", {}, nullptr);
            ui.ClearMissedKeys();
            (void)ui.Tr(L"prefs.whatever", L"中文默认文本");
            Check(ui.MissedKeys().empty(), "中文那一档没有包，也不该记成缺键");
            ui.Load(L"en-US", {}, nullptr);
            std::filesystem::remove(langDir / L"en-US.lang.xml", langEc);
        }
        {
            // 配置里的语言认不出来：回退中文并给警告
            Localization& ui = Localization::Ui();
            std::vector<std::wstring> warnings;
            ui.Load(L"ja-JP", langDir, &warnings);
            Check(ui.Locale() == L"zh-CN", "认不出来的语言回退简体中文");
            Check(!warnings.empty(), "回退时给一条警告");
            CheckWide(ui.Tr(L"cmd.file.save", L"保存(&S)"), L"保存(&S)", "回退后用的是默认文本");
        }
        {
            // 坏文件只跳过它自己
            Check(WriteFileDirect(langDir / L"broken.lang.xml", "这不是 XML"),
                  "写入损坏的语言包");
            Localization& ui = Localization::Ui();
            std::vector<std::wstring> warnings;
            ui.Load(L"xx-XX", langDir, &warnings);
            Check(ui.Locale() == L"xx-XX", "坏文件不影响别的包");
            Check(!warnings.empty(), "坏文件给一条警告");
            std::filesystem::remove(langDir / L"broken.lang.xml", langEc);
        }
        std::filesystem::remove(langDir / L"xx-XX.lang.xml", langEc);
        // 这一段动过全局的语言设置，收尾时必须还原：后面的段落（比如"快捷键"里那几条
        // 类别名断言）看到的应当是简体中文
        Localization::Ui().Load(L"zh-CN", {}, nullptr);
        Check(Localization::Ui().ChineseDefault(), "收尾：语言设置还原成简体中文");
    }

    Section("运行命令（变量展开 / UserDefinedCommands 持久化）");
    {
        using npcc::core::ExpandRunCommand;
        using npcc::core::ReadRunCommandsXml;
        using npcc::core::RunCommandDef;
        using npcc::core::RunContext;

        RunContext context;
        context.fullPath = L"D:\\work\\demo\\a.tar.gz";
        context.appDirectory = L"C:\\Tools\\notepadcc";
        context.currentWord = L"Widget";
        context.currentLine = 42;
        context.encodingName = L"UTF-8-BOM";
        context.eolName = L"CRLF";

        // ---- 变量展开 ----
        CheckWide(ExpandRunCommand(L"echo $(FULL_CURRENT_PATH)", context),
                  L"echo D:\\work\\demo\\a.tar.gz", "$(FULL_CURRENT_PATH) 是完整路径");
        CheckWide(ExpandRunCommand(L"$(CURRENT_DIRECTORY)", context), L"D:\\work\\demo",
                  "$(CURRENT_DIRECTORY) 不带结尾分隔符");
        CheckWide(ExpandRunCommand(L"$(FILE_NAME)", context), L"a.tar.gz",
                  "$(FILE_NAME) 带扩展名");
        CheckWide(ExpandRunCommand(L"$(NAME_PART)", context), L"a.tar",
                  "$(NAME_PART) 去掉最后一个扩展名");
        CheckWide(ExpandRunCommand(L"$(EXT_PART)", context), L"gz", "$(EXT_PART) 不带点");
        CheckWide(ExpandRunCommand(L"$(CURRENT_WORD)", context), L"Widget", "$(CURRENT_WORD)");
        CheckWide(ExpandRunCommand(L"$(CURRENT_LINE)", context), L"42",
                  "$(CURRENT_LINE) 是 1 基行号");
        CheckWide(ExpandRunCommand(L"$(NPP_DIRECTORY)", context), L"C:\\Tools\\notepadcc",
                  "$(NPP_DIRECTORY) 是程序目录");
        CheckWide(ExpandRunCommand(L"$(CURRENT_ENCODING)", context), L"UTF-8-BOM",
                  "$(CURRENT_ENCODING)");
        CheckWide(ExpandRunCommand(L"$(CURRENT_EOL)", context), L"CRLF", "$(CURRENT_EOL)");
        CheckWide(ExpandRunCommand(L"$(file_name)", context), L"a.tar.gz",
                  "变量名大小写不敏感");
        CheckWide(ExpandRunCommand(L"gcc \"$(FULL_CURRENT_PATH)\" -o out.exe", context),
                  L"gcc \"D:\\work\\demo\\a.tar.gz\" -o out.exe", "命令中间的变量照样展开");
        // 认不出来的变量原样留着：换成空串会让命令悄悄跑错目标，比报错难查得多
        CheckWide(ExpandRunCommand(L"$(UNKNOWN) $(FILE_NAME)", context), L"$(UNKNOWN) a.tar.gz",
                  "不认识的变量原样保留");
        CheckWide(ExpandRunCommand(L"echo $(FILE_NAME", context), L"echo $(FILE_NAME",
                  "少了右括号时原样保留");
        CheckWide(ExpandRunCommand(L"echo $HOME $$ $", context), L"echo $HOME $$ $",
                  "单个 $ 不动");
        CheckWide(ExpandRunCommand(L"", context), L"", "空命令行返回空");
        {
            // 未保存的文档：路径类变量全空（与对标产品一致）
            RunContext blank;
            blank.appDirectory = L"C:\\Tools\\notepadcc";
            CheckWide(ExpandRunCommand(L"[$(FULL_CURRENT_PATH)]", blank), L"[]",
                      "未保存的文档没有完整路径");
            CheckWide(ExpandRunCommand(L"[$(CURRENT_DIRECTORY)]", blank), L"[]",
                      "未保存的文档没有目录");
            CheckWide(ExpandRunCommand(L"[$(NAME_PART)]", blank), L"[]",
                      "没有文件名就没有名字部分");
            CheckWide(ExpandRunCommand(L"$(NPP_DIRECTORY)", blank), L"C:\\Tools\\notepadcc",
                      "没有文档也能拿到程序目录");
        }
        {
            RunContext plain;
            plain.fullPath = L"C:\\t\\README";
            CheckWide(ExpandRunCommand(L"$(NAME_PART)|$(EXT_PART)", plain), L"README|",
                      "没有扩展名时 NAME_PART 是整名、EXT_PART 为空");
            RunContext dotted;
            dotted.fullPath = L"C:\\t\\.gitignore";
            CheckWide(ExpandRunCommand(L"$(NAME_PART)|$(EXT_PART)", dotted), L".gitignore|",
                      "以点开头的文件名不切成空名字");
        }

        // ---- 写出去再读回来（走完整的转义路径） ----
        RunCommandDef sample;
        sample.name = L"编译并运行";
        sample.command = L"gcc \"$(FULL_CURRENT_PATH)\" -o run.exe && run.exe > out.txt";
        sample.modifiers = npcc::core::kModCtrl | npcc::core::kModAlt;
        sample.keyCode = 'B';
        sample.captureOutput = false;
        RunCommandDef plain;
        plain.name = L"打开所在目录";
        plain.command = L"explorer \"$(CURRENT_DIRECTORY)\"";
        {
            npcc::infra::XmlDocument document;
            npcc::infra::XmlElementPtr root = document.CreateRoot(L"NotepadPlusCompat");
            npcc::core::AppendRunCommandsXml(*root, {sample, plain});
            const std::string xml = document.ToUtf8();
            Check(xml.find("UserDefinedCommands") != std::string::npos,
                  "写出了 <UserDefinedCommands>");
            Check(xml.find("&amp;&amp;") != std::string::npos, "命令行里的 & 被转义");
            Check(xml.find("&gt;") != std::string::npos, "命令行里的 > 被转义");

            npcc::infra::XmlDocument parsed;
            std::wstring parseError;
            Check(npcc::infra::XmlDocument::Parse(xml, parsed, &parseError),
                  "写出去的 XML 能被自己读回来");
            std::vector<std::wstring> warnings;
            const std::vector<RunCommandDef> back = ReadRunCommandsXml(*parsed.Root(), warnings);
            Check(back.size() == 2, "两条命令都读回来了");
            Check(warnings.empty(), "正常内容不产生警告");
            if (back.size() == 2) {
                CheckWide(back[0].name, sample.name, "名字往返一致");
                CheckWide(back[0].command, sample.command, "命令行往返一致（含引号与重定向）");
                Check(back[0].modifiers == (npcc::core::kModCtrl | npcc::core::kModAlt),
                      "修饰键往返一致");
                Check(back[0].keyCode == 'B', "键码往返一致");
                Check(!back[0].captureOutput, "capture=no 往返一致");
                Check(back[1].keyCode == 0, "没绑快捷键时 Key 是 0");
                Check(back[1].captureOutput, "没写 capture 时按「捕获」处理");
            }
            // 一条都没有时不写空壳，免得文件里多出个没用的节
            npcc::infra::XmlDocument emptier;
            npcc::infra::XmlElementPtr emptyRoot = emptier.CreateRoot(L"NotepadPlusCompat");
            npcc::core::AppendRunCommandsXml(*emptyRoot, {});
            Check(emptier.ToUtf8().find("UserDefinedCommands") == std::string::npos,
                  "没有命令时不写空节");
        }
        {
            // 坏记录只跳过自己，不让整份文件失效
            const std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadPlusCompat>\n"
                "  <UserDefinedCommands>\n"
                "    <Command Ctrl=\"yes\" Key=\"66\">没有名字</Command>\n"
                "    <Command name=\"空的\"></Command>\n"
                "    <Command name=\"好的\">echo ok</Command>\n"
                "  </UserDefinedCommands>\n"
                "</NotepadPlusCompat>\n";
            npcc::infra::XmlDocument document;
            std::wstring parseError;
            Check(npcc::infra::XmlDocument::Parse(xml, document, &parseError), "解析手工写的文件");
            std::vector<std::wstring> warnings;
            const std::vector<RunCommandDef> back = ReadRunCommandsXml(*document.Root(), warnings);
            Check(back.size() == 1, "坏记录被跳过，好的留下");
            Check(warnings.size() == 2, "两条坏记录各给一条警告");
            if (back.size() == 1) {
                CheckWide(back[0].name, L"好的", "留下的是那一条好的");
            }
        }

        // ---- shortcuts.xml 端到端 ----
        npcc::infra::InitializePaths(true);
        const std::filesystem::path shortcutPath = npcc::core::ShortcutStore::Path();
        std::error_code shortcutEc;
        std::filesystem::remove(shortcutPath, shortcutEc);
        {
            const std::string custom =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadPlusCompat schemaVersion=\"1\">\n"
                "  <UserDefinedCommands>\n"
                "    <Command name=\"跑一下\" Ctrl=\"yes\" Alt=\"no\" Shift=\"no\" Key=\"66\" "
                "capture=\"no\">cmd /c echo hi</Command>\n"
                "  </UserDefinedCommands>\n"
                "</NotepadPlusCompat>\n";
            Check(WriteFileDirect(shortcutPath, custom), "写入带运行命令的 shortcuts.xml");
            npcc::core::ShortcutStore store;
            npcc::core::CommandRegistry registry;
            std::vector<std::wstring> warnings;
            std::wstring error;
            Check(store.Load(registry, &error, &warnings), "加载成功");
            Check(error.empty(), "正常文件不报错");
            Check(warnings.empty(), "正常文件不产生警告");
            Check(store.RunCommands().size() == 1, "读出一条运行命令");
            if (store.RunCommands().size() == 1) {
                const RunCommandDef& def = store.RunCommands()[0];
                CheckWide(def.name, L"跑一下", "名字读对了");
                CheckWide(def.command, L"cmd /c echo hi", "命令行读对了");
                Check(def.modifiers == npcc::core::kModCtrl, "修饰键读对了");
                Check(def.keyCode == 'B', "快捷键读对了");
                Check(!def.captureOutput, "capture=\"no\" 读对了");
            }
        }
        {
            // 保存：运行命令进 <UserDefinedCommands>，不进 <InternalCommands>；
            // 内置的"运行…"（没有 kCmdUserDefined）照旧进 <InternalCommands>
            npcc::core::ShortcutStore store;
            store.SetRunCommands({sample, plain});
            npcc::core::CommandRegistry registry;
            const npcc::core::CmdId first =
                npcc::core::MakeCmdId(static_cast<uint8_t>(npcc::core::CmdCategory::Run),
                                      static_cast<uint16_t>(npcc::core::kRunCommandBase));
            for (std::size_t index = 0; index < 2; ++index) {
                npcc::core::Command command;
                command.id = npcc::core::MakeCmdId(
                    static_cast<uint8_t>(npcc::core::CmdCategory::Run),
                    static_cast<uint16_t>(npcc::core::kRunCommandBase + index));
                command.category = npcc::core::CmdCategory::Run;
                command.key = L"run." + store.RunCommands()[index].name;
                command.text = store.RunCommands()[index].name;
                command.flags = npcc::core::kCmdUserDefined;
                registry.Register(command);
            }
            const npcc::core::CmdId dialogCommand =
                AddTestCommand(registry, npcc::core::CmdCategory::Run, L"cmd.run.dialog",
                               L"运行(&R)...");
            registry.AssignShortcut(dialogCommand, npcc::core::kModNone, VK_F5);
            // 与外壳一致：文件里写的快捷键在注册命令时绑进注册表
            registry.BindShortcut(first, sample.modifiers, sample.keyCode, true);
            store.SyncRunCommandShortcuts(registry);
            std::wstring error;
            Check(store.Save(registry, &error), "保存 shortcuts.xml 成功");
            const npcc::io::ReadResult read = npcc::io::ReadWholeFile(shortcutPath.wstring());
            Check(read.ok, "读回 shortcuts.xml");
            const std::string& text = read.bytes;
            Check(text.find("<UserDefinedCommands>") != std::string::npos,
                  "写出了 <UserDefinedCommands>");
            Check(text.find("run.编译并运行") == std::string::npos,
                  "用户定义的命令不进 <InternalCommands>");
            Check(text.find("cmd.run.dialog") != std::string::npos,
                  "内置的运行命令照旧进 <InternalCommands>");
            Check(text.find("Key=\"116\"") != std::string::npos, "F5(116) 写进了文件");
            Check(text.find("capture=\"yes\"") != std::string::npos, "capture 属性写出去了");
            Check(text.find("Ctrl=\"yes\" Alt=\"yes\"") != std::string::npos,
                  "命令的快捷键按 Ctrl/Alt/Shift 分开写");
        }
        {
            // 用户定义的命令如果（手工）出现在 <InternalCommands> 里，要忽略并给警告：
            // 真实启动时注册发生在读文件之后，那条记录根本认不出命令来，
            // 静默丢掉会让用户以为"快捷键没生效"是别的原因
            const std::string mixed =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NotepadPlusCompat schemaVersion=\"1\">\n"
                "  <InternalCommands>\n"
                "    <Shortcut name=\"cmd.run.dialog\" Ctrl=\"no\" Alt=\"no\" Shift=\"no\" "
                "Key=\"116\"/>\n"
                "    <Shortcut name=\"run.编译并运行\" Ctrl=\"yes\" Alt=\"no\" Shift=\"no\" "
                "Key=\"66\"/>\n"
                "  </InternalCommands>\n"
                "</NotepadPlusCompat>\n";
            Check(WriteFileDirect(shortcutPath, mixed), "写入混有用户命令的 shortcuts.xml");
            npcc::core::CommandRegistry registry;
            const npcc::core::CmdId dialogCommand =
                AddTestCommand(registry, npcc::core::CmdCategory::Run, L"cmd.run.dialog",
                               L"运行(&R)...");
            // 模拟外壳注册出来的用户命令（真实流程里它也是带这个标志的）
            npcc::core::Command userCommand;
            userCommand.id = npcc::core::MakeCmdId(
                static_cast<uint8_t>(npcc::core::CmdCategory::Run),
                static_cast<uint16_t>(npcc::core::kRunCommandBase));
            userCommand.category = npcc::core::CmdCategory::Run;
            userCommand.key = L"run.编译并运行";
            userCommand.text = L"编译并运行";
            userCommand.flags = npcc::core::kCmdUserDefined;
            registry.Register(userCommand);

            npcc::core::ShortcutStore store;
            std::vector<std::wstring> warnings;
            std::wstring error;
            Check(store.Load(registry, &error, &warnings), "加载成功");
            Check(!warnings.empty(), "用户定义的命令出现在 <InternalCommands> 里要给出警告");
            Check(registry.BindingsOf(userCommand.id).empty(), "那一条被忽略，没有建立绑定");
            Check(registry.ResolveShortcut(npcc::core::kModNone, VK_F5) == dialogCommand,
                  "同文件里的内置命令记录照常生效");
        }
        std::filesystem::remove(shortcutPath, shortcutEc);
    }

    Section("JSON / XML 格式化");
    {
        using npcc::core::FormatJson;
        using npcc::core::FormatXml;

        // ---- JSON：嵌套对象与数组 ----
        const auto nested = FormatJson("{\"a\":1,\"b\":[1,2],\"c\":{\"d\":true},\"e\":null}");
        Check(nested.ok, "合法 JSON 应当格式化成功");
        CheckEqual(nested.text,
                   "{\n"
                   "  \"a\": 1,\n"
                   "  \"b\": [\n"
                   "    1,\n"
                   "    2\n"
                   "  ],\n"
                   "  \"c\": {\n"
                   "    \"d\": true\n"
                   "  },\n"
                   "  \"e\": null\n"
                   "}\n",
                   "嵌套对象/数组按 2 空格缩进");
        const auto nestedAgain = FormatJson(nested.text);
        Check(nestedAgain.ok && nestedAgain.text == nested.text, "格式化是幂等的");
        const auto deep = FormatJson("{\"a\":[1]}", 4);
        CheckEqual(deep.text, "{\n    \"a\": [\n        1\n    ]\n}\n", "缩进宽度可按参数给");
        // 顶层标量也是合法 JSON（RFC 8259），别把它当错误
        const auto scalar = FormatJson("  42  ");
        Check(scalar.ok, "顶层标量不该被拒");
        CheckEqual(scalar.text, "42\n", "顶层标量原样输出");

        // ---- JSON：空对象 / 空数组 / 压缩 ----
        const auto empties = FormatJson("{\"a\":{},\"b\":[]}");
        CheckEqual(empties.text, "{\n  \"a\": {},\n  \"b\": []\n}\n", "空对象/空数组压成两个字符");
        const auto minified = FormatJson("{ \"a\" : [1, 2] ,\n  \"b\" : { } }", 2, true);
        CheckEqual(minified.text, "{\"a\":[1,2],\"b\":{}}\n", "压缩模式去掉结构间空白");

        // ---- JSON：字符串与数字**原样**保留 ----
        // 这条是刻意的：格式化只该动空白。把 \u4e2d 换成真汉字、把 1e3 算成 1000，
        // 都是在改用户的数据，不是"美化"。
        const auto kept = FormatJson(
            "{\"u\":\"\\u4e2d\\u6587\",\"n\":1.500,\"e\":1e3,\"z\":-0.0,\"q\":\"a\\\"b\\\\c\\/d\\t\"}");
        Check(kept.ok, "带转义与特殊数字写法的 JSON 应当合法");
        CheckEqual(kept.text,
                   "{\n"
                   "  \"u\": \"\\u4e2d\\u6587\",\n"
                   "  \"n\": 1.500,\n"
                   "  \"e\": 1e3,\n"
                   "  \"z\": -0.0,\n"
                   "  \"q\": \"a\\\"b\\\\c\\/d\\t\"\n"
                   "}\n",
                   "转义写法与数字字面量一个字节都没改");
        Check(kept.text.find("\\u4e2d") != std::string::npos, "\\uXXXX 没被换成真汉字");
        Check(kept.text.find("1.500") != std::string::npos, "1.500 没被写成 1.5");

        // ---- JSON：非法输入必须报错，且给得出行与列 ----
        struct JsonCase {
            const char*    text;
            const wchar_t* needle;   // 错误信息里必须出现的片段
        };
        const JsonCase jsonBad[] = {
            {"{\"a\":1,}", L"尾随逗号"},
            {"{\n  \"a\": [1, 2,]\n}", L"第 2 行第 14 列"},
            {"{a:1}", L"第 1 行第 2 列"},
            {"{\"a\": 1.}", L"小数点后面应当是数字"},
            {"{\"a\": tru}", L"应当是一个值"},
            {"{\"a\": \"x}", L"引号"},
            {"{\"a\": \"\\u12\"}", L"\\u 后面必须跟四个十六进制数字"},
            {"1 2", L"顶层值之后还有内容"},
            {"{\"a\":1", L"逗号或右花括号"},
            {"", L"内容为空"},
            {"   \n  ", L"内容为空"},
        };
        for (const JsonCase& bad : jsonBad) {
            const auto result = FormatJson(bad.text);
            Check(!result.ok, "非法 JSON 必须报错");
            Check(result.text.empty(), "失败时不返回半截文本");
            if (result.error.find(bad.needle) == std::wstring::npos) {
                std::printf("    [FAIL] JSON 错误信息里没有「%ls」：%s\n", bad.needle,
                            QuoteForLog(ToUtf8(result.error)).c_str());
            }
            Check(result.error.find(bad.needle) != std::wstring::npos, "错误信息对得上");
        }
        {
            // 字符串里夹了裸的控制字符：合法 JSON 必须写成 \t，不能直接放一个制表符
            const std::string rawTab = std::string("{\"a\":\"x") + '\t' + "y\"}";
            const auto result = FormatJson(rawTab);
            Check(!result.ok, "字符串里的裸控制字符应当被拒绝");
        }

        // ---- XML：嵌套缩进 ----
        const auto xmlNested = FormatXml("<root><a>1</a><b><c/></b></root>");
        Check(xmlNested.ok, "合法 XML 应当美化成功");
        CheckEqual(xmlNested.text, "<root>\n  <a>1</a>\n  <b>\n    <c/>\n  </b>\n</root>\n",
                   "嵌套元素逐层缩进（叶子元素带文字不影响祖先重排）");
        const auto xmlAgain = FormatXml(xmlNested.text);
        Check(xmlAgain.ok && xmlAgain.text == xmlNested.text, "XML 美化是幂等的");

        // ---- XML：混合内容整段不动 ----
        const auto mixed = FormatXml("<p>hello <b>world</b></p>");
        CheckEqual(mixed.text, "<p>hello <b>world</b></p>\n",
                   "混合内容整段原样输出，一个空格都没动");
        const auto mixedNested = FormatXml("<div><p>a <b>c</b></p></div>");
        CheckEqual(mixedNested.text, "<div>\n  <p>a <b>c</b></p>\n</div>\n",
                   "外层纯元素照常缩进，里层混合内容整段搬运");

        // ---- XML：注释 / CDATA / 处理指令 / DOCTYPE 原样 ----
        const std::string xmlDoc = "<?xml version=\"1.0\"?>\n"
                                   "<!DOCTYPE note [<!ENTITY x \"y\">]>\n"
                                   "<!-- head -->\n"
                                   "<root>\n"
                                   "<!-- inner -->\n"
                                   "<a><![CDATA[<raw>]]></a>\n"
                                   "</root>\n";
        const auto doc = FormatXml(xmlDoc);
        Check(doc.ok, "带 DOCTYPE / 注释 / CDATA 的文档应当美化成功");
        CheckEqual(doc.text,
                   "<?xml version=\"1.0\"?>\n"
                   "<!DOCTYPE note [<!ENTITY x \"y\">]>\n"
                   "<!-- head -->\n"
                   "<root>\n"
                   "  <!-- inner -->\n"
                   "  <a><![CDATA[<raw>]]></a>\n"
                   "</root>\n",
                   "DOCTYPE 的内部子集、注释、CDATA 全部原样，缩进只加在元素之间");
        Check(doc.text.find("<!ENTITY x \"y\">") != std::string::npos,
              "DOCTYPE 内部子集里的 > 没被当成 DOCTYPE 的结尾");
        Check(doc.text.find("<a><![CDATA[<raw>]]></a>") != std::string::npos,
              "带 CDATA 的元素整段搬运（CDATA 里装的是字符数据）");

        // ---- XML：自闭合与真的空元素 ----
        const auto selfClosing = FormatXml("<a><b/><b /></a>");
        CheckEqual(selfClosing.text, "<a>\n  <b/>\n  <b />\n</a>\n", "自闭合标签保留原写法");
        const auto emptyTag = FormatXml("<r><a></a></r>");
        CheckEqual(emptyTag.text, "<r>\n  <a></a>\n</r>\n", "一对标签之间什么都没有才压成一行");
        const auto blankContent = FormatXml("<r><a>   </a></r>");
        CheckEqual(blankContent.text, "<r>\n  <a>\n  </a>\n</r>\n",
                   "中间的空白是字符数据：换成标准缩进，但不当成空元素压掉");

        // ---- XML：非法输入必须报错，且给得出行与列 ----
        struct XmlCase {
            const char*    text;
            const wchar_t* needle;
        };
        const XmlCase xmlBad[] = {
            {"<a><b></a></b>", L"不匹配"},
            {"<a>\n<b>\n</c>", L"第 3 行第 1 列"},
            {"<a><b></b>", L"没有对应的结束标签"},
            {"</a>", L"多了一个结束标签"},
            {"<a><b", L"标签没有收尾"},
            {"<a><!-- x </a>", L"注释没有收尾"},
            {"<a><![CDATA[x</a>", L"CDATA 没有收尾"},
            {"", L"内容为空"},
            {"   \n  ", L"内容为空"},
        };
        for (const XmlCase& bad : xmlBad) {
            const auto result = FormatXml(bad.text);
            Check(!result.ok, "非法 XML 必须报错");
            Check(result.text.empty(), "失败时不返回半截文本");
            if (result.error.find(bad.needle) == std::wstring::npos) {
                std::printf("    [FAIL] XML 错误信息里没有「%ls」：%s\n", bad.needle,
                            QuoteForLog(ToUtf8(result.error)).c_str());
            }
            Check(result.error.find(bad.needle) != std::wstring::npos, "错误信息对得上");
        }
    }

    FinishSections();
    std::printf("\n结果: %d 项检查, %d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
