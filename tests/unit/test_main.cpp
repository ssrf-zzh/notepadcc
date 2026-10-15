// notepadcc 单元测试：只覆盖不依赖窗口的纯逻辑（设计文档 9.1 的单元测试层）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/Command.h"
#include "core/CommandRegistry.h"
#include "core/DocumentManager.h"
#include "edit/DocBuffer.h"
#include "edit/Encoding.h"
#include "edit/EditView.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"

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

void CheckEqual(const std::string& actual, const std::string& expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("    [FAIL] %s (实际 %u 字节, 期望 %u 字节)\n", what,
                    static_cast<unsigned>(actual.size()),
                    static_cast<unsigned>(expected.size()));
    }
}

void Section(const char* name) {
    std::printf("  %s\n", name);
}

LRESULT CALLBACK TestHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
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

    std::printf("\n结果: %d 项检查, %d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
