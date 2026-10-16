#include "edit/StyleCatalogue.h"

#include <algorithm>
#include <string>

#include "edit/LanguageRegistry.h"
#include "edit/UdlStyles.h"

namespace npcc::edit {
namespace {

// 9 组关键字按 组号 % 3 映射到三个语义角色：组 0/3/6 是"关键字"，1/4/7 是"强调项"，
// 2/5/8 是"次要项"。配色角色只有这三个够像关键字，所以按角色调色做不到 9 组各自一色
// ——想分开就把某几组逐项调（这正是本目录存在的意义）。上色与界面都读这张表，
// 所以这条映射只写在这里。
StyleRole KeywordRole(int group) {
    switch (group % 3) {
        case 1:
            return StyleRole::Highlight;
        case 2:
            return StyleRole::Extra;
        default:
            return StyleRole::Keyword;
    }
}

// UDL：样式号是我们自己定的（UdlStyles.h），目录就是它的全表——除了样式 0（普通文本）。
// 样式 0 是"默认样式"，改它等于改全局默认前景色，那是 Default 那一节的事，
// 不该混进"逐项调整"里。
std::vector<StyleEntry> UdlCatalogue() {
    std::vector<StyleEntry> list = {
        {udl::kStyleCommentLine, L"行注释", StyleRole::Comment},
        {udl::kStyleCommentBlock, L"块注释", StyleRole::Comment},
        {udl::kStyleNumber, L"数字", StyleRole::Number},
        {udl::kStyleString, L"字符串", StyleRole::String},
        {udl::kStyleOperator, L"运算符", StyleRole::Highlight},
        {udl::kStyleIdentifier, L"标识符", StyleRole::Extra},
    };
    for (int group = 0; group < udl::kKeywordGroupCount; ++group) {
        list.push_back({udl::KeywordStyleOf(group),
                        L"关键字 " + std::to_wstring(group + 1), KeywordRole(group)});
    }
    return list;
}

// 内置语言：这一份就是原先写在 EditView 里的那张"样式号对应关系"表，搬到这里并补上
// 名字——它现在同时是"上色用哪几个样式号"与"界面上列哪几项"的唯一来源。样式号取自
// Lexilla 的 SCE_*（third_party/lexilla/include/SciLexer.h），不靠记忆写数字：
// 写错的表现是"某个语言没颜色"，而且不报任何错。
struct BuiltinMap {
    const char* lexer;
    int         commentLine;    // -1 表示该语言没有这一类
    int         commentBlock;
    int         keyword;
    int         number;
    int         string;
    int         highlight;      // 语言特有的强调项：预处理、变量、属性名、键…
    int         extra;
};

constexpr BuiltinMap kBuiltinMaps[] = {
    {"cpp", 2, 1, 5, 4, 6, 9, 7},      // 字符字面量（7）归到次要项
    {"python", 1, -1, 5, 2, 3, -1, -1},
    {"json", 6, 7, -1, 1, 2, 4, -1},
    {"xml", 9, -1, 1, 5, 6, 3, 10},
    {"hypertext", 9, -1, 1, 5, 6, 3, 10},
    {"css", 9, -1, 1, -1, -1, 2, 8},
    {"markdown", -1, -1, 6, -1, 19, 2, 18},
    {"sql", 2, 1, 5, 4, 7, -1, -1},
    {"powershell", 1, 13, 8, 4, 2, 5, 9},
    {"batch", 1, -1, 2, -1, -1, -1, 5},
    {"props", 1, -1, 2, -1, -1, 5, 4},
};

std::vector<StyleEntry> BuiltinCatalogue(const BuiltinMap& map) {
    std::vector<StyleEntry> list;
    const auto add = [&list](int number, const wchar_t* name, StyleRole role) {
        if (number >= 0) {
            list.push_back({number, name, role});
        }
    };
    add(map.commentLine, L"行注释", StyleRole::Comment);
    add(map.commentBlock, L"块注释", StyleRole::Comment);
    add(map.keyword, L"关键字", StyleRole::Keyword);
    add(map.number, L"数字", StyleRole::Number);
    add(map.string, L"字符串", StyleRole::String);
    add(map.highlight, L"强调项", StyleRole::Highlight);
    add(map.extra, L"次要项", StyleRole::Extra);
    return list;
}

}  // namespace

std::vector<StyleEntry> StylesFor(const LanguageDef& language) {
    std::vector<StyleEntry> list;
    if (language.udl != nullptr) {
        list = UdlCatalogue();
    } else {
        for (const BuiltinMap& map : kBuiltinMaps) {
            if (language.lexer == map.lexer) {
                list = BuiltinCatalogue(map);
                break;
            }
        }
    }
    // 排序放在这里，而不是让每张表各自保证：数据是从别的模块搬过来的，
    // "调用方拿到的表总是按样式号有序"这种约定不该依赖搬运时的细心。
    std::sort(list.begin(), list.end(),
              [](const StyleEntry& a, const StyleEntry& b) { return a.number < b.number; });
    return list;
}

}  // namespace npcc::edit
