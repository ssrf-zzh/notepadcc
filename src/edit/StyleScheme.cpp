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
constexpr int            kSchemaVersion = 1;

// 内置默认配色。这里与原来的硬编码表格是同一份值——搬过来的目的是让它成为
// **唯一来源**：用户改不了的地方，至少要让它只有一处定义。
constexpr std::uint32_t kDefaultFore = 0x1E1E1E;
constexpr std::uint32_t kDefaultBack = 0xFFFFFF;
constexpr std::uint32_t kCommentColour = 0x2E7D32;    // 注释：绿
constexpr std::uint32_t kKeywordColour = 0x0000C0;    // 关键字：深蓝
constexpr std::uint32_t kNumberColour = 0x986500;     // 数字：棕
constexpr std::uint32_t kStringColour = 0xA31515;     // 字符串：暗红
constexpr std::uint32_t kHighlightColour = 0x800080;  // 强调项：紫
constexpr std::uint32_t kExtraColour = 0x266B9E;      // 次要项：蓝

struct RoleEntry {
    StyleRole      role;
    const char*    key;
    const wchar_t* display;
    std::uint32_t  fore;
    bool           bold;
    bool           italic;
};

const RoleEntry kRoles[] = {
    {StyleRole::Comment,   "comment",   L"注释",     kCommentColour,   false, true},
    {StyleRole::Keyword,   "keyword",   L"关键字",   kKeywordColour,   true,  false},
    {StyleRole::Number,    "number",    L"数字",     kNumberColour,    false, false},
    {StyleRole::String,    "string",    L"字符串",   kStringColour,    false, false},
    {StyleRole::Highlight, "highlight", L"强调项",   kHighlightColour, false, false},
    {StyleRole::Extra,     "extra",     L"次要项",   kExtraColour,     false, false},
};

const RoleEntry* FindRole(StyleRole role) {
    for (const RoleEntry& entry : kRoles) {
        if (entry.role == role) {
            return &entry;
        }
    }
    return nullptr;
}

std::wstring WidenAscii(const char* text) {
    std::wstring out;
    for (const char* cursor = text; cursor != nullptr && *cursor != '\0'; ++cursor) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
    }
    return out;
}

}  // namespace

StyleScheme StyleScheme::Defaults() {
    StyleScheme scheme;
    scheme.fore_ = kDefaultFore;
    scheme.back_ = kDefaultBack;
    scheme.fontFace_ = L"Consolas";
    scheme.fontSize_ = 11;
    scheme.overrides_.clear();   // 空 = 每个语言都用 kRoles 里那份默认
    return scheme;
}

std::filesystem::path StyleScheme::Path() {
    return infra::ConfigRoot() / L"stylers.xml";
}

const char* StyleScheme::RoleKey(StyleRole role) {
    const RoleEntry* entry = FindRole(role);
    return entry != nullptr ? entry->key : "";
}

bool StyleScheme::ParseRoleKey(std::string_view key, StyleRole& out) {
    for (const RoleEntry& entry : kRoles) {
        if (key == entry.key) {
            out = entry.role;
            return true;
        }
    }
    return false;
}

std::wstring StyleScheme::RoleDisplayName(StyleRole role) {
    const RoleEntry* entry = FindRole(role);
    return entry != nullptr ? std::wstring(entry->display) : std::wstring(L"未知");
}

std::wstring StyleScheme::ColourText(std::uint32_t colour) {
    wchar_t buffer[16] = {};
    ::swprintf_s(buffer, L"#%06X", colour & 0xFFFFFFu);
    return buffer;
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
    for (LanguageEntry& entry : overrides_) {
        if (entry.name == language) {
            entry.roles[static_cast<std::size_t>(role)] = style;
            return;
        }
    }
    LanguageEntry entry;
    entry.name = language;
    // 新条目先铺一份默认值，再改指定角色：这样界面上"没动过的角色"也有确定的值
    for (const RoleEntry& roleEntry : kRoles) {
        RoleStyle value;
        value.fore = roleEntry.fore;
        value.bold = roleEntry.bold;
        value.italic = roleEntry.italic;
        entry.roles[static_cast<std::size_t>(roleEntry.role)] = value;
    }
    entry.roles[static_cast<std::size_t>(role)] = style;
    overrides_.push_back(std::move(entry));
}

RoleStyle StyleScheme::Role(const std::wstring& language, StyleRole role) const {
    if (role == StyleRole::Count) {
        return {};
    }
    for (const LanguageEntry& entry : overrides_) {
        if (entry.name == language) {
            return entry.roles[static_cast<std::size_t>(role)];
        }
    }
    RoleStyle value;
    if (const RoleEntry* roleEntry = FindRole(role); roleEntry != nullptr) {
        value.fore = roleEntry->fore;
        value.bold = roleEntry->bold;
        value.italic = roleEntry->italic;
    }
    return value;
}

void StyleScheme::ResetLanguage(const std::wstring& language) {
    overrides_.erase(std::remove_if(overrides_.begin(), overrides_.end(),
                                    [&language](const LanguageEntry& entry) {
                                        return entry.name == language;
                                    }),
                     overrides_.end());
}

const std::vector<StyleOverride>& StyleScheme::Styles(const std::wstring& language) const {
    static const std::vector<StyleOverride> empty;
    for (const LanguageEntry& entry : overrides_) {
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
    for (LanguageEntry& entry : overrides_) {
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
    // 这个语言还没有条目：先按默认值铺一份角色，再挂上这条覆盖
    LanguageEntry entry;
    entry.name = language;
    for (const RoleEntry& roleEntry : kRoles) {
        RoleStyle value;
        value.fore = roleEntry.fore;
        value.bold = roleEntry.bold;
        value.italic = roleEntry.italic;
        entry.roles[static_cast<std::size_t>(roleEntry.role)] = value;
    }
    entry.styles.push_back(StyleOverride{number, std::move(name), style});
    overrides_.push_back(std::move(entry));
}

void StyleScheme::ClearStyle(const std::wstring& language, int number) {
    for (LanguageEntry& entry : overrides_) {
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
    return std::any_of(overrides_.begin(), overrides_.end(),
                       [&language](const LanguageEntry& entry) { return entry.name == language; });
}

void StyleScheme::SetDefault(std::uint32_t fore, std::uint32_t back, std::wstring fontFace,
                             int fontSize) {
    fore_ = fore & 0xFFFFFFu;
    back_ = back & 0xFFFFFFu;
    fontFace_ = std::move(fontFace);
    fontSize_ = std::max(6, std::min(72, fontSize));
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

    if (const infra::XmlElement* base = root->Child(L"Default"); base != nullptr) {
        std::uint32_t fore = fore_;
        std::uint32_t back = back_;
        if (ParseColour(base->AttributeOr(L"fore", L""), fore) == false) {
            sink.push_back(L"Default.fore 不是 #RRGGBB，已用默认值");
        }
        if (ParseColour(base->AttributeOr(L"back", L""), back) == false) {
            sink.push_back(L"Default.back 不是 #RRGGBB，已用默认值");
        }
        const std::wstring font = base->AttributeOr(L"font", fontFace_);
        const int size = base->AttributeInt(L"size", fontSize_);
        SetDefault(fore, back, font, size);
    }

    for (const infra::XmlElement* node : root->Children(L"Language")) {
        const std::wstring name = node->AttributeOr(L"name", L"");
        if (name.empty()) {
            sink.push_back(L"stylers.xml 里有一条没有 name 的语言条目，已跳过");
            continue;
        }
        for (const infra::XmlElement* style : node->Children(L"Style")) {
            StyleRole role = StyleRole::Comment;
            const std::wstring key = style->AttributeOr(L"role", L"");
            std::string narrow;
            narrow.reserve(key.size());
            for (const wchar_t ch : key) {
                narrow.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
            }
            if (!ParseRoleKey(narrow, role)) {
                sink.push_back(name + L"：认不出的样式角色 " + key + L"，已跳过");
                continue;
            }
            RoleStyle value = Role(name, role);
            if (!ParseColour(style->AttributeOr(L"fore", L""), value.fore)) {
                sink.push_back(name + L"." + key + L"：颜色不是 #RRGGBB，已跳过");
                continue;
            }
            value.bold = style->AttributeBool(L"bold", value.bold);
            value.italic = style->AttributeBool(L"italic", value.italic);
            SetRole(name, role, value);
        }
        // 逐样式号覆盖：颜色必须写全（没有"角色默认"可以继承——这个样式号可能是
        // 目录里都没有的，程序此刻并不知道它该是什么色）
        for (const infra::XmlElement* style : node->Children(L"StyleNumber")) {
            const int number = style->AttributeInt(L"number", -1);
            if (number < 0 || number > 255) {
                sink.push_back(name + L"：样式号越界（应当 0~255），已跳过");
                continue;
            }
            RoleStyle value;
            if (!ParseColour(style->AttributeOr(L"fore", L""), value.fore)) {
                sink.push_back(name + L"：样式号 " + std::to_wstring(number) +
                               L" 没写颜色或颜色不是 #RRGGBB，已跳过");
                continue;
            }
            value.bold = style->AttributeBool(L"bold", false);
            value.italic = style->AttributeBool(L"italic", false);
            SetStyle(name, number, style->AttributeOr(L"name", L""), value);
        }
    }

    for (const std::wstring& warning : sink) {
        infra::LogWarn(L"styles", L"样式方案: " + warning);
    }
    return true;
}

bool StyleScheme::Save(std::wstring* error) const {
    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(kRootName);
    root->SetAttributeInt(L"schemaVersion", kSchemaVersion);

    infra::XmlElementPtr base = root->AddChild(L"Default");
    base->SetAttribute(L"fore", ColourText(fore_));
    base->SetAttribute(L"back", ColourText(back_));
    base->SetAttribute(L"font", fontFace_);
    base->SetAttributeInt(L"size", fontSize_);

    for (const LanguageEntry& entry : overrides_) {
        infra::XmlElementPtr node = root->AddChild(L"Language");
        node->SetAttribute(L"name", entry.name);
        for (std::size_t index = 0; index < entry.roles.size(); ++index) {
            const RoleStyle& value = entry.roles[index];
            infra::XmlElementPtr style = node->AddChild(L"Style");
            style->SetAttribute(L"role", WidenAscii(RoleKey(static_cast<StyleRole>(index))));
            style->SetAttribute(L"fore", ColourText(value.fore));
            style->SetAttributeBool(L"bold", value.bold);
            style->SetAttributeBool(L"italic", value.italic);
        }
        // 逐样式号的覆盖跟在角色后面，一行一个样式号
        for (const StyleOverride& item : entry.styles) {
            infra::XmlElementPtr style = node->AddChild(L"StyleNumber");
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
    return io::SaveXmlFile(Path().wstring(), document, error);
}

}  // namespace npcc::edit
