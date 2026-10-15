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
#include "edit/DocBuffer.h"
#include "edit/ColumnEdit.h"
#include "edit/ColumnText.h"
#include "edit/Encoding.h"
#include "edit/FileTier.h"
#include "edit/EditView.h"
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

void Section(const char* name) {
    std::printf("  %s\n", name);
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
        snapshot.documents.push_back(npcc::core::SessionDocument{
            L"D:\\工程\\main.cpp", 128, 30, 2, false, {}});
        snapshot.documents.push_back(npcc::core::SessionDocument{
            L"D:\\工程\\含中文 与空格.txt", 7, 0, 0, true, {}});
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
        {
            std::lock_guard<std::mutex> guard(mutex);
            events.clear();
        }
        watcher.Unwatch(target.wstring());
        Check(watcher.WatchedCount() == 0, "取消监视后计数归零");
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

    std::printf("\n结果: %d 项检查, %d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
