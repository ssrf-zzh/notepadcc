#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "edit/UdlDefinition.h"

namespace npcc::edit {

// 一种语言的定义（设计文档 7.5 的 langs.xml）。
//
// 这是"这段文字是什么语言、怎么着色、怎么折叠、怎么加注释"的权威来源。
// 内置一份默认表，用户可以把默认表导出成 <配置目录>\langs.xml 再改：
// 新增语言不需要改代码。
struct LanguageDef {
    std::wstring languageName;      // 稳定标识，用于配置与日志（"cpp"）
    std::wstring displayName;       // 界面显示（"C/C++"）
    std::string  lexer;             // Lexilla 词法器名（必须存在，否则等于没接上）
    bool         foldable = true;
    std::vector<std::string> extensions;      // 不带点，小写
    std::vector<std::string> keywords;        // 下标即关键词组号
    std::string  lineComment;                 // 行注释符（"//"），空 = 该语言没有
    std::string  blockCommentStart;           // 块注释起（"/*"），空 = 没有
    std::string  blockCommentEnd;             // 块注释止（"*/"）

    // 非空 = 这是 UDL（用户自定义语言，来自 userlangs.xml），词法器是我们自研的那个。
    // 内置语言把这个字段留空，走 Lexilla 的 CreateLexer(lexer)。
    std::shared_ptr<const UdlDefinition> udl;
};

class LanguageRegistry {
public:
    // 代码里的内置表。文件缺失、解析失败或用户改了不认识的东西时都用它兜底。
    static const std::vector<LanguageDef>& Defaults();

    // <配置根>\langs.xml
    static std::filesystem::path Path();

    // 读配置文件覆盖内置表：文件里出现的语言覆盖同名内置语言，没出现的保留内置。
    // 文件不存在不算失败（首次运行就是这样）。
    bool Load(std::wstring* error = nullptr, std::vector<std::wstring>* warnings = nullptr);

    // 把当前生效的定义整体写出（首次导出即"内置表的副本"，用户在此基础上改）
    bool Save(std::wstring* error = nullptr) const;

    const LanguageDef* FindByExtension(std::string_view extension) const;
    const LanguageDef* Find(std::wstring_view languageName) const;
    // 内置语言（含 langs.xml 覆盖）在前，UDL 在后。菜单与样式配置器按这个顺序列。
    std::vector<const LanguageDef*> All() const;

    const std::vector<LanguageDef>& Builtins() const noexcept { return languages_; }
    const std::vector<LanguageDef>& UserDefined() const noexcept { return udlLanguages_; }

    // 从路径取小写扩展名（不带点）。放在这里是为了让"按扩展名找语言"这条路上
    // 只有一份实现——两处各写一份，早晚会有一处忘了转小写。
    static std::string ExtensionOfFile(const std::wstring& path);

private:
    // 读 userlangs.xml，把每条 UDL 转成一份 LanguageDef（udl 字段非空）
    void LoadUserDefined(std::vector<std::wstring>* warnings);

    std::vector<LanguageDef> languages_ = Defaults();
    // UDL 单独放一份而不是混进 languages_：按扩展名找语言时 UDL **优先**
    // （用户自己写的定义应当能盖过内置归属），而 Save() 又不该把它们写回 langs.xml。
    std::vector<LanguageDef> udlLanguages_;
};

}  // namespace npcc::edit
