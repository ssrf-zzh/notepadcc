#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::infra {
struct XmlElement;
}

namespace npcc::edit {

// 样式方案（设计文档 6.4 / 7.5 的 stylers.xml）。
//
// 配色是按**语义角色**给的，不是按 Lexilla 的样式号：样式号是词法器内部的事
// （同一个 lexer 给不同语言用，样式号也不同），而用户想调的是"注释用什么颜色"。
// 角色到样式号的映射仍然在 EditView 的样式表里，这一层只管"角色的颜色是什么"。
enum class StyleRole : int {
    Comment = 0,    // 注释（行注释与块注释共用）
    Keyword,
    Number,
    String,
    Highlight,      // 语言特有的强调项：预处理、变量、属性名…
    Extra,          // 次要项：实体、值、键名…
    Count,
};

inline constexpr int kStyleRoleCount = static_cast<int>(StyleRole::Count);

struct RoleStyle {
    std::uint32_t fore = 0;
    bool          bold = false;
    bool          italic = false;
};

inline bool operator==(const RoleStyle& a, const RoleStyle& b) {
    return a.fore == b.fore && a.bold == b.bold && a.italic == b.italic;
}
inline bool operator!=(const RoleStyle& a, const RoleStyle& b) { return !(a == b); }

// 逐样式号的覆盖（设计文档 2.6 第 4 条的后半段）。
//
// 角色是"粗调"（注释用什么颜色），这是"细调"（注释里第 3 个样式号单独一个颜色）。
// 名字存下来是为了让文件自己可读：手改 stylers.xml 的人看到
// `number="10" name="关键字 1"` 才知道这一行是什么。
struct StyleOverride {
    int          number = 0;
    std::wstring name;
    RoleStyle    value;
};

// 整套方案的默认值：前景/背景/字体字号 + 每个语言的角色配色。
// 某语言没有条目 = 用内置默认配色（也就是这里 Defaults() 里那份）。
class StyleScheme {
public:
    static StyleScheme Defaults();
    static std::filesystem::path Path();   // <配置根>\stylers.xml

    static const char* RoleKey(StyleRole role);         // 持久化用的稳定标识
    static bool ParseRoleKey(std::string_view key, StyleRole& out);
    static std::wstring RoleDisplayName(StyleRole role);   // 界面显示

    // 文件不存在不算失败（用默认配色）
    bool Load(std::wstring* error = nullptr, std::vector<std::wstring>* warnings = nullptr);
    bool Save(std::wstring* error = nullptr) const;

    // ---- 主题（设计文档 2.6 第 4 条：暗色模式）----
    // 一份方案里可以有多套主题，每套都是"完整的配色"（默认前景/背景/字体 + 每个语言的
    // 角色与逐样式号条目）。下面所有读写接口都作用于**当前主题**，所以样式配置器改的是
    // "现在这一套"，不会串到另一套去。
    //
    // 内置两套：light（原来那套浅色）与 dark。stylers.xml 里出现哪套就覆盖哪套，
    // 没出现的仍用内置值——与 langs.xml 同一条规则。
    static std::vector<std::wstring> BuiltinThemeNames();
    std::vector<std::wstring> ThemeNames() const;
    const std::wstring& ActiveTheme() const;
    bool IsDark() const;
    // 名字不认识时返回 false 且不改动当前主题
    bool SetActiveTheme(std::wstring_view name);

    RoleStyle Role(const std::wstring& language, StyleRole role) const;
    void      SetRole(const std::wstring& language, StyleRole role, const RoleStyle& style);
    // 逐样式号覆盖：按样式号升序；没有则返回空表
    const std::vector<StyleOverride>& Styles(const std::wstring& language) const;
    void SetStyle(const std::wstring& language, int number, std::wstring name,
                  const RoleStyle& style);
    void ClearStyle(const std::wstring& language, int number);
    // 把某个语言恢复成默认配色（删掉条目即可）
    void ResetLanguage(const std::wstring& language);
    bool HasOverride(const std::wstring& language) const;

    std::uint32_t Fore() const noexcept { return Active().fore; }
    std::uint32_t Back() const noexcept { return Active().back; }
    const std::wstring& FontFace() const noexcept { return Active().fontFace; }
    int FontSize() const noexcept { return Active().fontSize; }
    void SetDefault(std::uint32_t fore, std::uint32_t back, std::wstring fontFace, int fontSize);

    // 颜色文本 "#RRGGBB" 与数值互转（配置文件里写文本，人可读）
    static std::wstring ColourText(std::uint32_t colour);
    static bool ParseColour(std::wstring_view text, std::uint32_t& out);

    // 0xRRGGBB ↔ Scintilla 用的 COLORREF（0x00BBGGRR）。
    //
    // 配置文件里写的是 #RRGGBB，而 SCI_STYLESETFORE / SETBACK 收的是 COLORREF，
    // **红蓝是反的**。转换必须发生在交给控件的那一刻——踩过的坑：不转换时"关键字深蓝"
    // 会画成红色、"字符串暗红"会画成蓝色，而所有断言都在比对"存进去的值"与
    // "从控件读回来的值"（两边一致），于是没有一个测试看得出来（见附录 B）。
    static std::uint32_t ToScintillaColour(std::uint32_t rrggbb);
    static std::uint32_t FromScintillaColour(std::uint32_t colour);

private:
    struct LanguageEntry {
        std::wstring                  name;
        std::array<RoleStyle, kStyleRoleCount> roles{};
        std::vector<StyleOverride>    styles;
    };

    // 一套完整配色
    struct Theme {
        std::wstring               name;
        bool                       dark = false;
        std::uint32_t              fore = 0x1E1E1E;
        std::uint32_t              back = 0xFFFFFF;
        std::wstring               fontFace = L"Consolas";
        int                        fontSize = 11;
        std::vector<LanguageEntry> overrides;
    };

    Theme&       Active();
    const Theme& Active() const;
    static Theme BuiltinLight();
    static Theme BuiltinDark();
    // 找到（或建立）指定名字的主题；返回 nullptr 表示这个名字不接受
    Theme* Find(std::wstring_view name);
    // 读一套主题的内容（<Theme> 里那部分；旧版扁平写法也走这里）
    static void ReadThemeBody(const infra::XmlElement& node, StyleScheme& scheme,
                              std::vector<std::wstring>& sink);

    std::vector<Theme> themes_;
    std::size_t        active_ = 0;
};

}  // namespace npcc::edit
