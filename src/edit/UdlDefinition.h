#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::edit {

// 一对字符串界定符（设计文档 7.6 的 <StringDelim>）。
// 字符串**不跨行**：见到行尾就收掉。这条限制是有意的——它把"从文档中途开始着色时
// 不知道该按哪种界定符收尾"这个歧义彻底消掉（Scintilla 增量着色会带一个 initStyle
// 进来，跨行字符串那时无法判断用的是哪个界定符）。
struct UdlStringDelim {
    std::string start;    // 起始界定符（" 或 """）
    std::string end;      // 结束界定符（缺省与 start 相同）
    std::string escape;   // 转义字符（"\ "），空 = 不识别转义
};

// 一份 UDL 定义在内存里的样子。解析是纯函数（给字节、返回结构），
// 所以单元测试不需要窗口，也不需要磁盘。
struct UdlDefinition {
    std::wstring name;         // 稳定标识（也是 stylers.xml 里的语言键）
    std::wstring displayName;  // 界面显示名
    std::vector<std::string> extensions;   // 不带点，小写
    bool caseSensitive = false;            // 关键字是否区分大小写
    bool foldable = false;                 // 有折叠词才有折叠

    std::string lineComment;        // 行注释符（"# ），空 = 没有
    std::string blockCommentStart;  // 块注释起（"(*"）
    std::string blockCommentEnd;    // 块注释止（"*)"）
    bool        blockCommentNested = false;

    std::vector<std::string> keywordGroups;   // 下标即组号（0..8），空格分隔的词表
    std::vector<UdlStringDelim> strings;
    std::string operators;                    // 运算符**字符集**（"+-*/" 里的每个字符都算）

    std::vector<std::string> foldOpenTokens;   // 折叠开始词（"let fun match"）
    std::vector<std::string> foldCloseTokens;  // 折叠结束词（"end"）
};

// <配置目录>\userlangs.xml 的读写（设计文档 7.6）。
class UdlLoader {
public:
    static std::filesystem::path Path();

    // 一份带注释的示例（UTF-8 字节），供菜单"导出 UDL 示例到配置目录"写出去当模板
    static std::string SampleXml();

    // 解析 UTF-8 字节。整份文件读不动（XML 坏了、根元素不对）时返回 false 并填 error；
    // 单条定义有问题时**跳过这一条**并往 warnings 里记一句——一条写坏的 UDL 不该让
    // 其他 UDL 和内置语言一起失效。
    static bool Parse(std::string_view utf8, std::vector<UdlDefinition>& out,
                      std::wstring* error, std::vector<std::wstring>* warnings);

    // 文件不存在不算失败（首次运行就是这样）。
    static bool Load(std::vector<UdlDefinition>& out, std::wstring* error,
                     std::vector<std::wstring>* warnings);
};

}  // namespace npcc::edit
