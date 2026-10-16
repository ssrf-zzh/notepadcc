#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// XML 结构树（M3 的"内置 XML 工具"之一：树视图的数据来源）。
//
// 分工与 JSON 树完全一致：**校验交给 XmlFormat**（它那套错误信息已经写得够细，含行列），
// 这里先 FormatXml 一遍确认合法，再在"已经确认合法"的输入上走一遍、把结构与行号记下来。
// 这样"什么叫合法 XML"只有一处定义，不会出现"美化说不行、树视图说行"这种事。

enum class XmlNodeKind { Document, Element, Attribute, Text, CData, Comment, PI, Doctype };

struct XmlNode {
    XmlNodeKind          kind = XmlNodeKind::Element;
    std::string          name;      // 元素 / 属性 / 处理指令的名字（其余为空）
    std::string          value;     // 属性值（引号内原文）；文本 / CDATA / 注释 / 处理指令 / DOCTYPE 的原文
    std::vector<XmlNode> children;  // 元素：属性排最前、之后是子内容；文档根：全部顶层内容
    int                  line = 1;  // 在源文本里的行号（1 基），点击跳转要用
};

struct XmlTreeResult {
    bool         ok = false;
    XmlNode      root;              // kind == Document（文档根不是"元素"，是顶层内容的容器）
    std::wstring error;             // 失败原因（来自 XmlFormat，含行列）
    int          nodeCount = 0;
    bool         truncated = false; // 节点太多，只建了前面一部分
};

// 节点数上限：与 JSON 树同一个数量级，超过就不再建、并在面板里说明
inline constexpr int kMaxXmlNodes = 20000;

// 纯空白文本不建节点：XML 里元素之间到处是换行与缩进，全都列出来等于一片空白行。
XmlTreeResult ParseXmlTree(std::string_view text);

}  // namespace npcc::core
