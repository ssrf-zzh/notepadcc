#include "edit/UdlDefinition.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

#include "edit/UdlStyles.h"
#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/FileReader.h"

namespace npcc::edit {
namespace {

constexpr const wchar_t* kRootName = L"UserLangs";
constexpr int            kSchemaVersion = 1;
// 一份 UDL 定义不该有几百 KB；超过这个数就当文件坏了，别把内存吃光
constexpr std::uint64_t  kMaxFileBytes = 4ull * 1024 * 1024;

std::string Narrow(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        // 词法器要的是字节；UDL 的这些记号本来就该是 ASCII（中文界定符没法按字节比对）
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

// 与 infra/Xml.cpp 里那份同一件事：写文件要的是 UTF-8 字节。UDL 示例里有中文注释，
// 不能靠"窄字符直接截"糊过去。
std::string Utf8FromWide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          need, nullptr, nullptr);
    return out;
}

// 空白分隔的词表；顺手去重（同一个词写两遍没有意义）
std::vector<std::string> SplitList(std::wstring_view text, bool lowercase = true) {
    std::vector<std::string> items;
    std::string current;
    const auto flush = [&]() {
        if (current.empty()) {
            return;
        }
        std::string word = current;
        if (lowercase) {
            for (char& ch : word) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
        }
        if (std::find(items.begin(), items.end(), word) == items.end()) {
            items.push_back(word);
        }
        current.clear();
    };
    for (const wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == L',') {
            flush();
            continue;
        }
        current.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    flush();
    return items;
}

bool ParseOne(const infra::XmlElement& node, UdlDefinition& out, std::wstring& why) {
    out.name = node.AttributeOr(L"name", L"");
    if (out.name.empty()) {
        why = L"有一条定义没写 name，已跳过";
        return false;
    }
    out.displayName = node.AttributeOr(L"displayName", out.name);
    out.extensions = SplitList(node.AttributeOr(L"ext", L""));
    if (out.extensions.empty()) {
        why = out.name + L"：没写 ext，按扩展名永远匹配不到，已跳过";
        return false;
    }
    out.caseSensitive = node.AttributeBool(L"caseSensitive", false);

    if (const infra::XmlElement* line = node.Child(L"LineComment")) {
        out.lineComment = Narrow(line->AttributeOr(L"prefix", L""));
    }
    if (const infra::XmlElement* block = node.Child(L"BlockComment")) {
        out.blockCommentStart = Narrow(block->AttributeOr(L"start", L""));
        out.blockCommentEnd = Narrow(block->AttributeOr(L"end", L""));
        out.blockCommentNested = block->AttributeBool(L"nested", false);
        if (out.blockCommentStart.empty() || out.blockCommentEnd.empty()) {
            why = out.name + L"：块注释起止符要成对写，缺一个就当没有块注释";
            out.blockCommentStart.clear();
            out.blockCommentEnd.clear();
        }
    }

    for (const infra::XmlElement* keywords : node.Children(L"Keywords")) {
        const int group = keywords->AttributeInt(L"group", 0);
        if (group < 0 || group >= udl::kKeywordGroupCount) {
            why = out.name + L"：关键字组号只能在 0~" +
                  std::to_wstring(udl::kKeywordGroupCount - 1) + L" 之间，这一组已忽略";
            continue;
        }
        if (out.keywordGroups.size() <= static_cast<std::size_t>(group)) {
            out.keywordGroups.resize(static_cast<std::size_t>(group) + 1);
        }
        // 关键字表由词法器自己去大小写（见 UdlLexer::WordListSet），这里保持原样
        out.keywordGroups[static_cast<std::size_t>(group)] = Narrow(keywords->text);
    }

    for (const infra::XmlElement* delim : node.Children(L"StringDelim")) {
        UdlStringDelim parsed;
        parsed.start = Narrow(delim->AttributeOr(L"start", L""));
        parsed.end = Narrow(delim->AttributeOr(L"end", L""));
        parsed.escape = Narrow(delim->AttributeOr(L"escape", L""));
        if (parsed.start.empty()) {
            why = out.name + L"：有一条 StringDelim 没写 start，已忽略";
            continue;
        }
        if (parsed.end.empty()) {
            parsed.end = parsed.start;   // 只写 start 表示"两端一样"
        }
        out.strings.push_back(parsed);
    }

    if (const infra::XmlElement* operators = node.Child(L"Operators")) {
        // 运算符是**字符集**，而人写的时候一定会用空格分开（"+ - * / ="）。空格是分隔符
        // 不是运算符——不剔掉的话，源码里每个空格都会被染成运算符色（真踩过）。
        // 顺便去重：同一个字符写两遍没有意义，比对时还得多查一次。
        for (const char ch : Narrow(operators->text)) {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                continue;
            }
            if (out.operators.find(ch) == std::string::npos) {
                out.operators.push_back(ch);
            }
        }
    }

    out.foldable = false;
    if (const infra::XmlElement* fold = node.Child(L"Fold")) {
        out.foldOpenTokens = SplitList(fold->AttributeOr(L"openTokens", L""));
        out.foldCloseTokens = SplitList(fold->AttributeOr(L"closeTokens", L""));
        // 只给了收起词也能用（比如只看 end），所以两边都不空才叫"没规则"
        out.foldable = !(out.foldOpenTokens.empty() && out.foldCloseTokens.empty());
    }
    return true;
}

}  // namespace

std::filesystem::path UdlLoader::Path() {
    return infra::ConfigRoot() / L"userlangs.xml";
}

bool UdlLoader::Parse(std::string_view utf8, std::vector<UdlDefinition>& out,
                      std::wstring* error, std::vector<std::wstring>* warnings) {
    std::vector<UdlDefinition> parsed;
    std::vector<std::wstring> localWarnings;
    std::vector<std::wstring>& sink = warnings != nullptr ? *warnings : localWarnings;

    infra::XmlDocument document;
    if (!infra::XmlDocument::Parse(utf8, document, error)) {
        return false;
    }
    const infra::XmlElement* root = document.Root();
    if (root == nullptr) {
        if (error != nullptr) {
            *error = L"userlangs.xml 里没有根元素";
        }
        return false;
    }
    if (root->AttributeInt(L"schemaVersion", kSchemaVersion) > kSchemaVersion) {
        if (error != nullptr) {
            *error = L"userlangs.xml 来自更新的版本，已忽略";
        }
        return false;
    }

    // 两种写法都收：<UserLangs> 里放若干条，或者压根只有一条 <UserLang>
    std::vector<const infra::XmlElement*> nodes;
    if (root->name == kRootName) {
        nodes = root->Children(L"UserLang");
    } else if (root->name == L"UserLang") {
        nodes.push_back(root);
    } else {
        if (error != nullptr) {
            *error = L"userlangs.xml 的根元素应当是 UserLangs 或 UserLang，实际是 " + root->name;
        }
        return false;
    }

    for (const infra::XmlElement* node : nodes) {
        if (node == nullptr) {
            continue;
        }
        UdlDefinition definition;
        std::wstring why;
        if (!ParseOne(*node, definition, why)) {
            sink.push_back(why);
            continue;
        }
        if (!why.empty()) {
            sink.push_back(why);
        }
        const auto same = [&](const UdlDefinition& other) {
            return other.name == definition.name;
        };
        if (const auto existing = std::find_if(parsed.begin(), parsed.end(), same);
            existing != parsed.end()) {
            const std::wstring duplicated = definition.name;
            *existing = std::move(definition);   // 同名以最后一条为准
            sink.push_back(duplicated + L"：同名的 UDL 出现了多次，以最后一条为准");
            continue;
        }
        parsed.push_back(std::move(definition));
    }
    out = std::move(parsed);
    return true;
}

bool UdlLoader::Load(std::vector<UdlDefinition>& out, std::wstring* error,
                     std::vector<std::wstring>* warnings) {
    const std::filesystem::path path = Path();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.clear();
        return true;   // 没有文件 = 没有 UDL，这不是错误
    }
    const io::ReadResult read = io::ReadWholeFile(path.wstring(), kMaxFileBytes);
    if (!read.ok) {
        if (error != nullptr) {
            *error = L"读不了 " + path.wstring() + L"：" + read.message;
        }
        return false;
    }
    return Parse(read.bytes, out, error, warnings);
}

std::string UdlLoader::SampleXml() {
    // 写成模板而不是"导出当前生效的定义"：UDL 本来就是让用户从零写一份自己的语言，
    // 一份带注释的最小例子比一份空文件有用得多。
    return Utf8FromWide(
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<!-- notepadcc 用户自定义语言（UDL）。\n"
        L"     改完重启程序生效；文件里出现的定义按扩展名认领文件。\n"
        L"     详细的字段说明见设计说明书 7.6 节。 -->\n"
        L"<UserLangs schemaVersion=\"1\">\n"
        L"  <UserLang name=\"mydsl\" displayName=\"My DSL\" ext=\"mydsl mdl\"\n"
        L"            caseSensitive=\"yes\">\n"
        L"    <!-- 注释符：留空表示这种语言没有行注释（Ctrl+Q 会明确提示） -->\n"
        L"    <LineComment prefix=\"//\"/>\n"
        L"    <BlockComment start=\"(*\" end=\"*)\" nested=\"no\"/>\n"
        L"\n"
        L"    <!-- 关键字：group 0~8 共 9 组，空白分隔。0/1/2… 循环映射到\n"
        L"         关键字 / 强调项 / 次要项三个配色角色（将来可逐组单配） -->\n"
        L"    <Keywords group=\"0\">let fun match with type module end</Keywords>\n"
        L"    <Keywords group=\"1\">true false null</Keywords>\n"
        L"\n"
        L"    <!-- 字符串：end 省略表示与 start 相同；escape 留空表示不认转义。\n"
        L"         字符串不跨行，撞到行尾就收尾 -->\n"
        L"    <StringDelim start=\"&quot;\" end=\"&quot;\" escape=\"\\\"/>\n"
        L"    <StringDelim start=\"'\"/>\n"
        L"\n"
        L"    <!-- 运算符按**字符**看：下面这串表示这些字符各自都算运算符 -->\n"
        L"    <Operators>+ - * / = &lt; &gt; | &amp;</Operators>\n"
        L"\n"
        L"    <!-- 折叠：出现在这些词处的行成为折叠头。注释与字符串里的不算 -->\n"
        L"    <Fold openTokens=\"let fun match\" closeTokens=\"end\"/>\n"
        L"  </UserLang>\n"
        L"</UserLangs>\n");
}

}  // namespace npcc::edit
