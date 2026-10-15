// notepadcc 单元测试：只覆盖不依赖窗口的纯逻辑（设计文档 9.1 的单元测试层）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
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

    std::printf("\n结果: %d 项检查, %d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
