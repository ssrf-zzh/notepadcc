// notepadcc 单元测试：只覆盖不依赖窗口的纯逻辑（设计文档 9.1 的单元测试层）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "core/Command.h"
#include "core/CommandRegistry.h"
#include "core/DocumentManager.h"
#include "core/SessionManager.h"
#include "edit/DocBuffer.h"
#include "edit/Encoding.h"
#include "edit/EditView.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"
#include "io/XmlFile.h"
#include "search/EscapeSequence.h"
#include "search/CrossFileSearch.h"
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
            L"D:\\工程\\main.cpp", 128, 30, 2, false});
        snapshot.documents.push_back(npcc::core::SessionDocument{
            L"D:\\工程\\含中文 与空格.txt", 7, 0, 0, true});
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

    std::printf("\n结果: %d 项检查, %d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
