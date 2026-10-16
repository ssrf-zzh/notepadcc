// notepadcc 单元测试：只覆盖不依赖窗口的纯逻辑（设计文档 9.1 的单元测试层）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "core/Command.h"
#include "core/CommandRegistry.h"
#include "core/CommandText.h"
#include "core/ConfigCenter.h"
#include "core/DocumentManager.h"
#include "core/SessionManager.h"
#include "core/ShortcutStore.h"
#include "core/Macro.h"
#include "core/SnapshotStore.h"
#include "infra/SingleInstance.h"
#include "infra/FileAssociation.h"
#include "edit/DocBuffer.h"
#include "edit/ColumnEdit.h"
#include "edit/ColumnText.h"
#include "edit/Encoding.h"
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

    FinishSections();
    std::printf("\n结果: %d 项检查, %d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
