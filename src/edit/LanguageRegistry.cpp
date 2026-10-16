#include "edit/LanguageRegistry.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/XmlFile.h"

namespace npcc::edit {
namespace {

constexpr const wchar_t* kRootName = L"Languages";
constexpr int            kSchemaVersion = 1;

// 关键词表按组给出，下标就是 Lexilla 的关键词组号。
// 写成常量是为了让同族的扩展名共用一份，而不是各抄一遍——抄错的表现是
// "某个扩展名没有关键字着色"，而且不会报错（真的踩过）。
constexpr const char* kCppKeywords =
    "alignas alignof auto bool break case catch char class const constexpr continue default "
    "delete do double else enum explicit extern false float for friend goto if inline int long "
    "mutable namespace new noexcept nullptr operator private protected public register return "
    "short signed sizeof static struct switch template this thread_local throw true try typedef "
    "typename union unsigned using virtual void volatile while wchar_t";
constexpr const char* kJsKeywords =
    "async await break case catch class const continue debugger default delete do else export "
    "extends false finally for function if import in instanceof let new null return super switch "
    "this throw true try typeof undefined var void while with yield";
constexpr const char* kTypeScriptKeywords =
    "abstract any as async await boolean break case catch class const constructor continue "
    "declare default delete do else enum export extends false finally for from function if "
    "implements import in instanceof interface let module namespace new null number object "
    "private protected public readonly return static string super switch this throw true try "
    "type typeof undefined var void while with yield";
constexpr const char* kPythonKeywords =
    "and as assert async await break class continue def del elif else except False finally for "
    "from global if import in is lambda None nonlocal not or pass raise return True try while "
    "with yield";
constexpr const char* kSqlKeywords =
    "select from where insert into values update set delete create table database index view "
    "drop alter join left right inner outer on group by order having union all distinct as";

LanguageDef CppFamily(const wchar_t* name, const wchar_t* display,
                      std::vector<std::string> extensions) {
    LanguageDef def;
    def.languageName = name;
    def.displayName = display;
    def.lexer = "cpp";
    def.foldable = true;
    def.extensions = std::move(extensions);
    def.keywords = {kCppKeywords};
    def.lineComment = "//";
    def.blockCommentStart = "/*";
    def.blockCommentEnd = "*/";
    return def;
}

}  // namespace

const std::vector<LanguageDef>& LanguageRegistry::Defaults() {
    static const std::vector<LanguageDef> defaults = [] {
        std::vector<LanguageDef> list;

        list.push_back(CppFamily(L"cpp", L"C/C++", {"cpp", "cxx", "cc", "c", "h", "hpp", "hxx"}));

        LanguageDef python;
        python.languageName = L"python";
        python.displayName = L"Python";
        python.lexer = "python";
        python.extensions = {"py", "pyw"};
        python.keywords = {kPythonKeywords};
        python.lineComment = "#";
        list.push_back(std::move(python));

        LanguageDef js;
        js.languageName = L"javascript";
        js.displayName = L"JavaScript";
        // Lexilla 里**没有** javascript 词法器，用 cpp 配一套 JS 关键字（通行做法）
        js.lexer = "cpp";
        js.extensions = {"js", "mjs", "jsx"};
        js.keywords = {kJsKeywords};
        js.lineComment = "//";
        js.blockCommentStart = "/*";
        js.blockCommentEnd = "*/";
        list.push_back(std::move(js));

        LanguageDef ts;
        ts.languageName = L"typescript";
        ts.displayName = L"TypeScript";
        ts.lexer = "cpp";
        ts.extensions = {"ts", "tsx"};
        ts.keywords = {kTypeScriptKeywords};
        ts.lineComment = "//";
        ts.blockCommentStart = "/*";
        ts.blockCommentEnd = "*/";
        list.push_back(std::move(ts));

        LanguageDef json;
        json.languageName = L"json";
        json.displayName = L"JSON";
        json.lexer = "json";
        json.extensions = {"json"};
        json.foldable = true;
        list.push_back(std::move(json));

        LanguageDef xml;
        xml.languageName = L"xml";
        xml.displayName = L"XML";
        xml.lexer = "xml";
        xml.extensions = {"xml", "xsd", "xsl", "svg"};
        xml.blockCommentStart = "<!--";
        xml.blockCommentEnd = "-->";
        list.push_back(std::move(xml));

        LanguageDef html;
        html.languageName = L"html";
        html.displayName = L"HTML";
        html.lexer = "hypertext";
        html.extensions = {"html", "htm", "xhtml"};
        html.blockCommentStart = "<!--";
        html.blockCommentEnd = "-->";
        list.push_back(std::move(html));

        LanguageDef css;
        css.languageName = L"css";
        css.displayName = L"CSS";
        css.lexer = "css";
        css.extensions = {"css", "scss"};
        css.blockCommentStart = "/*";
        css.blockCommentEnd = "*/";
        list.push_back(std::move(css));

        LanguageDef markdown;
        markdown.languageName = L"markdown";
        markdown.displayName = L"Markdown";
        markdown.lexer = "markdown";
        markdown.extensions = {"md", "markdown"};
        markdown.blockCommentStart = "<!--";
        markdown.blockCommentEnd = "-->";
        list.push_back(std::move(markdown));

        LanguageDef sql;
        sql.languageName = L"sql";
        sql.displayName = L"SQL";
        sql.lexer = "sql";
        sql.extensions = {"sql"};
        sql.keywords = {kSqlKeywords};
        sql.lineComment = "--";
        sql.blockCommentStart = "/*";
        sql.blockCommentEnd = "*/";
        list.push_back(std::move(sql));

        LanguageDef batch;
        batch.languageName = L"batch";
        batch.displayName = L"Batch";
        batch.lexer = "batch";
        batch.foldable = false;   // 这个词法器不产生折叠层级
        batch.extensions = {"bat", "cmd"};
        batch.lineComment = "REM ";
        list.push_back(std::move(batch));

        LanguageDef powershell;
        powershell.languageName = L"powershell";
        powershell.displayName = L"PowerShell";
        powershell.lexer = "powershell";
        powershell.extensions = {"ps1", "psm1"};
        powershell.lineComment = "#";
        powershell.blockCommentStart = "<#";
        powershell.blockCommentEnd = "#>";
        list.push_back(std::move(powershell));

        LanguageDef props;
        props.languageName = L"props";
        props.displayName = L"配置文件";
        props.lexer = "props";
        props.foldable = false;
        props.extensions = {"ini", "cfg", "conf", "properties"};
        props.lineComment = ";";
        list.push_back(std::move(props));

        LanguageDef plain;
        plain.languageName = L"null";
        plain.displayName = L"纯文本";
        plain.lexer = "null";
        plain.foldable = false;
        list.push_back(std::move(plain));

        return list;
    }();
    return defaults;
}

std::filesystem::path LanguageRegistry::Path() {
    return infra::ConfigRoot() / L"langs.xml";
}

namespace {

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

std::wstring Widen(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::vector<std::string> SplitList(const std::wstring& text) {
    std::vector<std::string> out;
    std::wstring current;
    auto flush = [&out, &current]() {
        if (!current.empty()) {
            out.push_back(Narrow(current));
            current.clear();
        }
    };
    for (const wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t' || ch == L',' || ch == L';') {
            flush();
            continue;
        }
        const wchar_t lower = static_cast<wchar_t>(std::towlower(ch));
        current.push_back(lower);
    }
    flush();
    return out;
}

std::wstring JoinList(const std::vector<std::string>& items) {
    std::wstring out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += L' ';
        }
        out += Widen(item);
    }
    return out;
}

// 用文件里的语言定义覆盖内置的同名语言；文件里新增的语言追加在后面。
void ApplyOverride(std::vector<LanguageDef>& target, const LanguageDef& override_) {
    for (LanguageDef& existing : target) {
        if (existing.languageName == override_.languageName) {
            existing = override_;
            return;
        }
    }
    target.push_back(override_);
}

}  // namespace

bool LanguageRegistry::Load(std::wstring* error, std::vector<std::wstring>* warnings) {
    languages_ = Defaults();
    udlLanguages_.clear();
    std::vector<std::wstring> localWarnings;
    std::vector<std::wstring>& sink = warnings != nullptr ? *warnings : localWarnings;

    // langs.xml 与 userlangs.xml 互不依赖：一个坏了不影响另一个。
    // 这里先把两份都读进来，最后统一记警告再返回状态。
    bool ok = true;
    const std::filesystem::path path = Path();
    infra::XmlDocument document;
    const infra::XmlElement* root = nullptr;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 没有文件 = 全用内置表，这不是错误
    } else if (std::wstring parseError;
               !io::LoadXmlFile(path.wstring(), document, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        sink.push_back(L"langs.xml 加载失败，改用内置表：" + parseError);
        ok = false;
    } else if ((root = document.Root()) == nullptr || root->name != kRootName) {
        if (error != nullptr) {
            *error = L"langs.xml 的根元素不是 " + std::wstring(kRootName);
        }
        sink.push_back(L"langs.xml 的根元素不对，改用内置表");
        ok = false;
    } else if (root->AttributeInt(L"schemaVersion", 0) > kSchemaVersion) {
        if (error != nullptr) {
            *error = L"langs.xml 来自更新的版本，已忽略";
        }
        sink.push_back(L"langs.xml 来自更新的版本，已忽略");
        ok = false;
    }

    std::vector<const infra::XmlElement*> nodes;
    if (ok && root != nullptr) {
        nodes = root->Children(L"Language");
    }
    for (const infra::XmlElement* node : nodes) {
        if (node == nullptr) {
            continue;
        }
        LanguageDef def;
        def.languageName = node->AttributeOr(L"name", L"");
        if (def.languageName.empty()) {
            sink.push_back(L"langs.xml 里有一条没有 name 的语言定义，已跳过");
            continue;
        }
        def.displayName = node->AttributeOr(L"display", def.languageName);
        def.lexer = Narrow(node->AttributeOr(L"lexer", L"null"));
        def.foldable = node->AttributeBool(L"foldable", true);
        def.extensions = SplitList(node->AttributeOr(L"ext", L""));
        if (const infra::XmlElement* line = node->Child(L"LineComment"); line != nullptr) {
            def.lineComment = Narrow(line->AttributeOr(L"prefix", L""));
        }
        if (const infra::XmlElement* block = node->Child(L"BlockComment"); block != nullptr) {
            def.blockCommentStart = Narrow(block->AttributeOr(L"start", L""));
            def.blockCommentEnd = Narrow(block->AttributeOr(L"end", L""));
        }
        for (const infra::XmlElement* keywords : node->Children(L"Keywords")) {
            const int group = keywords->AttributeInt(L"group", 0);
            if (group < 0 || group > 8) {
                sink.push_back(def.languageName + L"：关键词组号越界，已忽略");
                continue;
            }
            if (def.keywords.size() <= static_cast<std::size_t>(group)) {
                def.keywords.resize(static_cast<std::size_t>(group) + 1);
            }
            def.keywords[static_cast<std::size_t>(group)] = Narrow(keywords->text);
        }
        if (def.lexer.empty()) {
            sink.push_back(def.languageName + L"：lexer 为空，已跳过");
            continue;
        }
        ApplyOverride(languages_, def);
    }

    LoadUserDefined(&sink);

    for (const std::wstring& warning : sink) {
        infra::LogWarn(L"langs", L"语言定义: " + warning);
    }
    return ok;
}

void LanguageRegistry::LoadUserDefined(std::vector<std::wstring>* warnings) {
    std::vector<UdlDefinition> definitions;
    std::wstring error;
    std::vector<std::wstring> localWarnings;
    std::vector<std::wstring>& sink = warnings != nullptr ? *warnings : localWarnings;
    if (!UdlLoader::Load(definitions, &error, &sink)) {
        // UDL 读坏了不该让内置语言一起失效：记一条警告，继续用内置表
        sink.push_back(L"userlangs.xml 加载失败，UDL 全部停用：" + error);
        return;
    }
    for (const UdlDefinition& definition : definitions) {
        LanguageDef def;
        def.languageName = definition.name;
        def.displayName = definition.displayName;
        // lexer 字段留成 "udl"：它只是给人看的标识，真正装上去的是自研词法器
        // （EditView 看到 udl 非空就不会去叫 CreateLexer）
        def.lexer = "udl";
        def.foldable = definition.foldable;
        def.extensions = definition.extensions;
        def.keywords = definition.keywordGroups;
        def.lineComment = definition.lineComment;
        def.blockCommentStart = definition.blockCommentStart;
        def.blockCommentEnd = definition.blockCommentEnd;

        // 扩展名撞车要说清楚：UDL 优先（用户自己写的东西应当能盖过内置归属），
        // 但内置语言如果已经被别处引用，这个扩展名会"看起来没生效"，得留一条线索。
        for (const std::string& extension : def.extensions) {
            for (const LanguageDef& builtin : languages_) {
                if (std::find(builtin.extensions.begin(), builtin.extensions.end(), extension) !=
                    builtin.extensions.end()) {
                    sink.push_back(def.languageName + L"：扩展名 " +
                                   std::wstring(extension.begin(), extension.end()) +
                                   L" 与内置语言 " + builtin.languageName +
                                   L" 重叠，UDL 优先");
                }
            }
        }
        def.udl = std::make_shared<const UdlDefinition>(definition);
        udlLanguages_.push_back(std::move(def));
    }
}

bool LanguageRegistry::Save(std::wstring* error) const {
    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(kRootName);
    root->SetAttributeInt(L"schemaVersion", kSchemaVersion);
    root->SetAttribute(L"comment",
                       L"语言定义：文件里出现的语言覆盖内置同名语言，没出现的仍用内置值");

    for (const LanguageDef& def : languages_) {
        infra::XmlElementPtr node = root->AddChild(L"Language");
        node->SetAttribute(L"name", def.languageName);
        node->SetAttribute(L"display", def.displayName);
        node->SetAttribute(L"lexer", Widen(def.lexer));
        node->SetAttribute(L"ext", JoinList(def.extensions));
        node->SetAttributeBool(L"foldable", def.foldable);
        if (!def.lineComment.empty()) {
            node->AddChild(L"LineComment")->SetAttribute(L"prefix", Widen(def.lineComment));
        }
        if (!def.blockCommentStart.empty() && !def.blockCommentEnd.empty()) {
            infra::XmlElementPtr block = node->AddChild(L"BlockComment");
            block->SetAttribute(L"start", Widen(def.blockCommentStart));
            block->SetAttribute(L"end", Widen(def.blockCommentEnd));
        }
        for (std::size_t group = 0; group < def.keywords.size(); ++group) {
            if (def.keywords[group].empty()) {
                continue;
            }
            infra::XmlElementPtr keywords = node->AddChild(L"Keywords");
            keywords->SetAttributeInt(L"group", static_cast<int>(group));
            keywords->text = Widen(def.keywords[group]);
        }
    }
    return io::SaveXmlFile(Path().wstring(), document, error);
}

const LanguageDef* LanguageRegistry::FindByExtension(std::string_view extension) const {
    if (extension.empty()) {
        return nullptr;
    }
    std::string lower(extension);
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    // UDL 优先：用户自己写的定义能盖过内置的扩展名归属（撞车时 Load 会记一条警告）
    for (const LanguageDef& def : udlLanguages_) {
        if (std::find(def.extensions.begin(), def.extensions.end(), lower) !=
            def.extensions.end()) {
            return &def;
        }
    }
    for (const LanguageDef& def : languages_) {
        if (std::find(def.extensions.begin(), def.extensions.end(), lower) !=
            def.extensions.end()) {
            return &def;
        }
    }
    return nullptr;
}

std::string LanguageRegistry::ExtensionOfFile(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) {
        return {};
    }
    // 扩展名里可能混着路径分隔符（比如 "a.b/c"），只取最后一个点之后且不含分隔符的部分
    const std::wstring tail = path.substr(dot + 1);
    if (tail.find_first_of(L"\\/") != std::wstring::npos) {
        return {};
    }
    std::string out;
    out.reserve(tail.size());
    for (const wchar_t ch : tail) {
        if (ch >= 0x80) {
            return {};   // 扩展名不会是中文，出现异常字符就当没扩展名
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

const LanguageDef* LanguageRegistry::Find(std::wstring_view languageName) const {
    for (const LanguageDef& def : udlLanguages_) {
        if (def.languageName == languageName) {
            return &def;
        }
    }
    for (const LanguageDef& def : languages_) {
        if (def.languageName == languageName) {
            return &def;
        }
    }
    return nullptr;
}

std::vector<const LanguageDef*> LanguageRegistry::All() const {
    std::vector<const LanguageDef*> all;
    all.reserve(languages_.size() + udlLanguages_.size());
    for (const LanguageDef& def : languages_) {
        all.push_back(&def);
    }
    for (const LanguageDef& def : udlLanguages_) {
        all.push_back(&def);
    }
    return all;
}

}  // namespace npcc::edit
