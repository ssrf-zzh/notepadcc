#include "edit/StyleScheme.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/XmlFile.h"

namespace npcc::edit {
namespace {

constexpr const wchar_t* kRootName = L"NotepadccStyles";
constexpr const wchar_t* kThemeName = L"Theme";
constexpr int            kSchemaVersion = 2;   // 2 = 引入主题（1 的扁平写法仍读得进来）

// 内置默认配色。这里与原来硬编码在 EditView 里的表格是同一份值——搬过来的目的是让它成为
// **唯一来源**：用户改不了的地方，至少要让它只有一处定义。
constexpr std::uint32_t kDefaultFore = 0x1E1E1E;
constexpr std::uint32_t kDefaultBack = 0xFFFFFF;

struct RoleEntry {
    StyleRole      role;
    const char*    key;
    const wchar_t* display;
    std::uint32_t  fore;
    bool           bold;
    bool           italic;
};

// 浅色（原来那套）
const RoleEntry kRolesLight[] = {
    {StyleRole::Comment,   "comment",   L"注释",   0x2E7D32, false, true},
    {StyleRole::Keyword,   "keyword",   L"关键字", 0x0000C0, true,  false},
    {StyleRole::Number,    "number",    L"数字",   0x986500, false, false},
    {StyleRole::String,    "string",    L"字符串", 0xA31515, false, false},
    {StyleRole::Highlight, "highlight", L"强调项", 0x800080, false, false},
    {StyleRole::Extra,     "extra",     L"次要项", 0x266B9E, false, false},
};

// 深色：不是"把浅色反过来"，而是照常见深色配色（编辑器社区的惯例）各给一个够亮的颜色——
// 深底上的关键字要够蓝、字符串要够橙，压暗的绿/红在深底上根本读不清。
// 角色名与 key 必须与浅色表逐条一致（界面上是同一组切片，只是取值不同）。
const RoleEntry kRolesDark[] = {
    {StyleRole::Comment,   "comment",   L"注释",   0x6A9955, false, true},
    {StyleRole::Keyword,   "keyword",   L"关键字", 0x569CD6, true,  false},
    {StyleRole::Number,    "number",    L"数字",   0xB5CEA8, false, false},
    {StyleRole::String,    "string",    L"字符串", 0xCE9178, false, false},
    {StyleRole::Highlight, "highlight", L"强调项", 0xC586C0, false, false},
    {StyleRole::Extra,     "extra",     L"次要项", 0x4FC1FF, false, false},
};

constexpr std::uint32_t kDarkFore = 0xD4D4D4;
constexpr std::uint32_t kDarkBack = 0x1E1E1E;

const RoleEntry* RoleTable(bool dark) {
    return dark ? kRolesDark : kRolesLight;
}

const RoleEntry* FindRole(bool dark, StyleRole role) {
    const RoleEntry* table = RoleTable(dark);
    for (int index = 0; index < kStyleRoleCount; ++index) {
        if (table[index].role == role) {
            return &table[index];
        }
    }
    return nullptr;
}

RoleStyle DefaultRoleStyle(bool dark, StyleRole role) {
    RoleStyle value;
    if (const RoleEntry* entry = FindRole(dark, role); entry != nullptr) {
        value.fore = entry->fore;
        value.bold = entry->bold;
        value.italic = entry->italic;
    }
    return value;
}

std::wstring WidenAscii(const char* text) {
    std::wstring out;
    for (const char* cursor = text; cursor != nullptr && *cursor != '\0'; ++cursor) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
    }
    return out;
}

std::string NarrowAscii(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

}  // namespace

StyleScheme::Theme StyleScheme::BuiltinLight() {
    Theme theme;
    theme.name = L"light";
    theme.dark = false;
    theme.fore = kDefaultFore;
    theme.back = kDefaultBack;
    theme.fontFace = L"Consolas";
    theme.fontSize = 11;
    return theme;
}

StyleScheme::Theme StyleScheme::BuiltinDark() {
    Theme theme;
    theme.name = L"dark";
    theme.dark = true;
    theme.fore = kDarkFore;
    theme.back = kDarkBack;
    theme.fontFace = L"Consolas";
    theme.fontSize = 11;
    return theme;
}

StyleScheme StyleScheme::Defaults() {
    StyleScheme scheme;
    scheme.themes_ = {BuiltinLight(), BuiltinDark()};
    scheme.active_ = 0;
    return scheme;
}

std::filesystem::path StyleScheme::Path() {
    return infra::ConfigRoot() / L"stylers.xml";
}

std::vector<std::wstring> StyleScheme::BuiltinThemeNames() {
    return {L"light", L"dark"};
}

StyleScheme::Theme& StyleScheme::Active() {
    if (active_ >= themes_.size()) {
        active_ = 0;
    }
    return themes_[active_];
}

const StyleScheme::Theme& StyleScheme::Active() const {
    if (active_ >= themes_.size()) {
        return themes_.front();   // 构造之后永远至少有一套主题
    }
    return themes_[active_];
}

StyleScheme::Theme* StyleScheme::Find(std::wstring_view name) {
    for (Theme& theme : themes_) {
        if (theme.name == name) {
            return &theme;
        }
    }
    return nullptr;
}

std::vector<std::wstring> StyleScheme::ThemeNames() const {
    std::vector<std::wstring> names;
    names.reserve(themes_.size());
    for (const Theme& theme : themes_) {
        names.push_back(theme.name);
    }
    return names;
}

const std::wstring& StyleScheme::ActiveTheme() const {
    return Active().name;
}

bool StyleScheme::IsDark() const {
    return Active().dark;
}

bool StyleScheme::SetActiveTheme(std::wstring_view name) {
    for (std::size_t index = 0; index < themes_.size(); ++index) {
        if (themes_[index].name == name) {
            active_ = index;
            return true;
        }
    }
    return false;
}

const char* StyleScheme::RoleKey(StyleRole role) {
    const RoleEntry* entry = FindRole(false, role);
    return entry != nullptr ? entry->key : "";
}

bool StyleScheme::ParseRoleKey(std::string_view key, StyleRole& out) {
    for (int index = 0; index < kStyleRoleCount; ++index) {
        if (key == kRolesLight[index].key) {
            out = kRolesLight[index].role;
            return true;
        }
    }
    return false;
}

std::wstring StyleScheme::RoleDisplayName(StyleRole role) {
    const RoleEntry* entry = FindRole(false, role);
    return entry != nullptr ? std::wstring(entry->display) : std::wstring(L"未知");
}

std::wstring StyleScheme::ColourText(std::uint32_t colour) {
    wchar_t buffer[16] = {};
    ::swprintf_s(buffer, L"#%06X", colour & 0xFFFFFFu);
    return buffer;
}

std::uint32_t StyleScheme::ToScintillaColour(std::uint32_t rrggbb) {
    // R/B 互换，其余不动（COLORREF 是 0x00BBGGRR）
    return ((rrggbb & 0x0000FFu) << 16) | (rrggbb & 0x00FF00u) | ((rrggbb & 0xFF0000u) >> 16);
}

std::uint32_t StyleScheme::FromScintillaColour(std::uint32_t colour) {
    return ToScintillaColour(colour);   // 这变换是自己的逆
}

bool StyleScheme::ParseColour(std::wstring_view text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    std::wstring_view body = text;
    if (body.front() == L'#') {
        body.remove_prefix(1);
    }
    if (body.size() != 6) {
        return false;
    }
    std::uint32_t value = 0;
    for (const wchar_t ch : body) {
        std::uint32_t digit = 0;
        if (ch >= L'0' && ch <= L'9') {
            digit = static_cast<std::uint32_t>(ch - L'0');
        } else if (ch >= L'a' && ch <= L'f') {
            digit = static_cast<std::uint32_t>(ch - L'a' + 10);
        } else if (ch >= L'A' && ch <= L'F') {
            digit = static_cast<std::uint32_t>(ch - L'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

void StyleScheme::SetRole(const std::wstring& language, StyleRole role, const RoleStyle& style) {
    if (language.empty() || role == StyleRole::Count) {
        return;
    }
    Theme& theme = Active();
    for (LanguageEntry& entry : theme.overrides) {
        if (entry.name == language) {
            entry.roles[static_cast<std::size_t>(role)] = style;
            return;
        }
    }
    LanguageEntry entry;
    entry.name = language;
    // 新条目先铺一份本主题的默认值，再改指定角色：这样界面上"没动过的角色"也有确定的值
    for (int index = 0; index < kStyleRoleCount; ++index) {
        const auto kind = static_cast<StyleRole>(index);
        entry.roles[static_cast<std::size_t>(index)] = DefaultRoleStyle(theme.dark, kind);
    }
    entry.roles[static_cast<std::size_t>(role)] = style;
    theme.overrides.push_back(std::move(entry));
}

RoleStyle StyleScheme::Role(const std::wstring& language, StyleRole role) const {
    if (role == StyleRole::Count) {
        return {};
    }
    const Theme& theme = Active();
    for (const LanguageEntry& entry : theme.overrides) {
        if (entry.name == language) {
            return entry.roles[static_cast<std::size_t>(role)];
        }
    }
    return DefaultRoleStyle(theme.dark, role);
}

void StyleScheme::ResetLanguage(const std::wstring& language) {
    Theme& theme = Active();
    theme.overrides.erase(std::remove_if(theme.overrides.begin(), theme.overrides.end(),
                                         [&language](const LanguageEntry& entry) {
                                             return entry.name == language;
                                         }),
                          theme.overrides.end());
}

const std::vector<StyleOverride>& StyleScheme::Styles(const std::wstring& language) const {
    static const std::vector<StyleOverride> empty;
    const Theme& theme = Active();
    for (const LanguageEntry& entry : theme.overrides) {
        if (entry.name == language) {
            return entry.styles;
        }
    }
    return empty;
}

void StyleScheme::SetStyle(const std::wstring& language, int number, std::wstring name,
                           const RoleStyle& style) {
    // 样式号是 Scintilla 的 STYLE_MAX（255）以内；越界的值写进文件也上不了色，
    // 不如当场拒掉，免得"配置写对了却没反应"这种最难查的现象
    if (language.empty() || number < 0 || number > 255) {
        return;
    }
    Theme& theme = Active();
    for (LanguageEntry& entry : theme.overrides) {
        if (entry.name != language) {
            continue;
        }
        for (StyleOverride& item : entry.styles) {
            if (item.number == number) {
                item.name = std::move(name);
                item.value = style;
                return;
            }
        }
        entry.styles.push_back(StyleOverride{number, std::move(name), style});
        return;
    }
    LanguageEntry entry;
    entry.name = language;
    for (int index = 0; index < kStyleRoleCount; ++index) {
        const auto kind = static_cast<StyleRole>(index);
        entry.roles[static_cast<std::size_t>(index)] = DefaultRoleStyle(theme.dark, kind);
    }
    entry.styles.push_back(StyleOverride{number, std::move(name), style});
    theme.overrides.push_back(std::move(entry));
}

void StyleScheme::ClearStyle(const std::wstring& language, int number) {
    Theme& theme = Active();
    for (LanguageEntry& entry : theme.overrides) {
        if (entry.name != language) {
            continue;
        }
        entry.styles.erase(std::remove_if(entry.styles.begin(), entry.styles.end(),
                                          [number](const StyleOverride& item) {
                                              return item.number == number;
                                          }),
                           entry.styles.end());
        return;
    }
}

bool StyleScheme::HasOverride(const std::wstring& language) const {
    const Theme& theme = Active();
    return std::any_of(theme.overrides.begin(), theme.overrides.end(),
                       [&language](const LanguageEntry& entry) {
                           return entry.name == language;
                       });
}

void StyleScheme::SetDefault(std::uint32_t fore, std::uint32_t back, std::wstring fontFace,
                             int fontSize) {
    Theme& theme = Active();
    theme.fore = fore & 0xFFFFFFu;
    theme.back = back & 0xFFFFFFu;
    theme.fontFace = std::move(fontFace);
    theme.fontSize = std::max(6, std::min(72, fontSize));
}

bool StyleScheme::Load(std::wstring* error, std::vector<std::wstring>* warnings) {
    *this = Defaults();
    std::vector<std::wstring> localWarnings;
    std::vector<std::wstring>& sink = warnings != nullptr ? *warnings : localWarnings;

    const std::filesystem::path path = Path();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;   // 没有文件 = 全用默认配色
    }
    infra::XmlDocument document;
    std::wstring parseError;
    if (!io::LoadXmlFile(path.wstring(), document, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        infra::LogWarn(L"styles", L"样式方案加载失败，改用默认配色: " + parseError);
        return false;
    }
    const infra::XmlElement* root = document.Root();
    if (root == nullptr || root->name != kRootName) {
        if (error != nullptr) {
            *error = L"stylers.xml 的根元素不是 " + std::wstring(kRootName);
        }
        return false;
    }
    if (root->AttributeInt(L"schemaVersion", 0) > kSchemaVersion) {
        if (error != nullptr) {
            *error = L"stylers.xml 来自更新的版本，已忽略";
        }
        return false;
    }

    // 两代写法都要认：
    //   旧（schemaVersion 1）：根下直接是 <Default> 与 <Language>，那就是浅色主题
    //   新（schemaVersion 2）：根下是若干 <Theme name="...">，每套里再放 Default/Language
    // 文件里出现哪套主题就覆盖哪套，没出现的仍用内置值（与 langs.xml 同一条规则）。
    const std::vector<const infra::XmlElement*> themeNodes = root->Children(kThemeName);
    if (themeNodes.empty()) {
        SetActiveTheme(L"light");
        ReadThemeBody(*root, *this, sink);
    } else {
        for (const infra::XmlElement* node : themeNodes) {
            if (node == nullptr) {
                continue;
            }
            const std::wstring name = node->AttributeOr(L"name", L"");
            if (name.empty()) {
                sink.push_back(L"stylers.xml 里有一套没有 name 的主题，已跳过");
                continue;
            }
            Theme* theme = Find(name);
            if (theme == nullptr) {
                // 文件里可以出现我们不认识的主题名（用户自己加一套），照样据实读进来
                Theme extra;
                extra.name = name;
                extra.dark = name == L"dark";
                themes_.push_back(std::move(extra));
            }
            SetActiveTheme(name);   // 按名字激活：不拿 push_back 之后的指针去算下标
            ReadThemeBody(*node, *this, sink);
        }
        SetActiveTheme(L"light");
    }

    // 内置的两套主题永远在：文件里没提到 dark 时，用户切过去也该有一套像样的深色，
    // 而不是"切了没反应"
    for (const std::wstring& name : BuiltinThemeNames()) {
        if (Find(name) == nullptr) {
            themes_.push_back(name == L"dark" ? BuiltinDark() : BuiltinLight());
        }
    }
    SetActiveTheme(L"light");
    return true;
}

void StyleScheme::ReadThemeBody(const infra::XmlElement& node, StyleScheme& scheme,
                                std::vector<std::wstring>& sink) {
    const StyleScheme::Theme& active = scheme.Active();
    if (const infra::XmlElement* base = node.Child(L"Default")) {
        std::uint32_t fore = active.fore;
        std::uint32_t back = active.back;
        if (!StyleScheme::ParseColour(base->AttributeOr(L"fore", L""), fore)) {
            sink.push_back(L"Default.fore 不是 #RRGGBB，已用默认值");
        }
        if (!StyleScheme::ParseColour(base->AttributeOr(L"back", L""), back)) {
            sink.push_back(L"Default.back 不是 #RRGGBB，已用默认值");
        }
        const std::wstring font = base->AttributeOr(L"font", active.fontFace);
        const int size = base->AttributeInt(L"size", active.fontSize);
        scheme.SetDefault(fore, back, font, size);
    }

    for (const infra::XmlElement* language : node.Children(L"Language")) {
        const std::wstring name = language->AttributeOr(L"name", L"");
        if (name.empty()) {
            sink.push_back(L"stylers.xml 里有一条没有 name 的语言条目，已跳过");
            continue;
        }
        for (const infra::XmlElement* style : language->Children(L"Style")) {
            StyleRole role = StyleRole::Comment;
            const std::wstring key = style->AttributeOr(L"role", L"");
            if (!StyleScheme::ParseRoleKey(NarrowAscii(key), role)) {
                sink.push_back(name + L"：认不出的样式角色 " + key + L"，已跳过");
                continue;
            }
            RoleStyle value = scheme.Role(name, role);
            if (!StyleScheme::ParseColour(style->AttributeOr(L"fore", L""), value.fore)) {
                sink.push_back(name + L"." + key + L"：颜色不是 #RRGGBB，已跳过");
                continue;
            }
            value.bold = style->AttributeBool(L"bold", value.bold);
            value.italic = style->AttributeBool(L"italic", value.italic);
            scheme.SetRole(name, role, value);
        }
        // 逐样式号覆盖：颜色必须写全（没有"角色默认"可以继承——这个样式号可能是
        // 目录里都没有的，程序此刻并不知道它该是什么色）
        for (const infra::XmlElement* style : language->Children(L"StyleNumber")) {
            const int number = style->AttributeInt(L"number", -1);
            if (number < 0 || number > 255) {
                sink.push_back(name + L"：样式号越界（应当 0~255），已跳过");
                continue;
            }
            RoleStyle value;
            if (!StyleScheme::ParseColour(style->AttributeOr(L"fore", L""), value.fore)) {
                sink.push_back(name + L"：样式号 " + std::to_wstring(number) +
                               L" 没写颜色或颜色不是 #RRGGBB，已跳过");
                continue;
            }
            value.bold = style->AttributeBool(L"bold", false);
            value.italic = style->AttributeBool(L"italic", false);
            scheme.SetStyle(name, number, style->AttributeOr(L"name", L""), value);
        }
    }
}

bool StyleScheme::Save(std::wstring* error) const {
    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(kRootName);
    root->SetAttributeInt(L"schemaVersion", kSchemaVersion);

    for (const Theme& theme : themes_) {
        infra::XmlElementPtr node = root->AddChild(kThemeName);
        node->SetAttribute(L"name", theme.name);
        infra::XmlElementPtr base = node->AddChild(L"Default");
        base->SetAttribute(L"fore", ColourText(theme.fore));
        base->SetAttribute(L"back", ColourText(theme.back));
        base->SetAttribute(L"font", theme.fontFace);
        base->SetAttributeInt(L"size", theme.fontSize);

        for (const LanguageEntry& entry : theme.overrides) {
            infra::XmlElementPtr language = node->AddChild(L"Language");
            language->SetAttribute(L"name", entry.name);
            for (std::size_t index = 0; index < entry.roles.size(); ++index) {
                const RoleStyle& value = entry.roles[index];
                infra::XmlElementPtr style = language->AddChild(L"Style");
                style->SetAttribute(L"role", WidenAscii(RoleKey(static_cast<StyleRole>(index))));
                style->SetAttribute(L"fore", ColourText(value.fore));
                style->SetAttributeBool(L"bold", value.bold);
                style->SetAttributeBool(L"italic", value.italic);
            }
            // 逐样式号的覆盖跟在角色后面，一行一个样式号
            for (const StyleOverride& item : entry.styles) {
                infra::XmlElementPtr style = language->AddChild(L"StyleNumber");
                style->SetAttributeInt(L"number", item.number);
                if (!item.name.empty()) {
                    // 名字只为让人看得懂这一行是什么；程序不用它
                    style->SetAttribute(L"name", item.name);
                }
                style->SetAttribute(L"fore", ColourText(item.value.fore));
                style->SetAttributeBool(L"bold", item.value.bold);
                style->SetAttributeBool(L"italic", item.value.italic);
            }
        }
    }
    return io::SaveXmlFile(Path().wstring(), document, error);
}

}  // namespace npcc::edit
