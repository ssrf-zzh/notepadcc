#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/XmlTree.h"

namespace npcc::core {

// XPath 查询（M3"内置 XML 工具"树 / XPath / 校验 三件里的 XPath 一件）。
//
// 支持的子集（这一行就是权威定义，对话框提示与设计文档都照这份说法写）：
//   路径：/a/b 绝对、a/b 相对（上下文是文档根，两者等价）、//a 任意深度；
//   节点测试：元素名、*、text()、@属性名、@*、.（自身）；
//   谓词：[n] 序号（1 基）、[@attr]、[@attr='v']、[@attr!='v']、
//         [child]、[child='v']、[text()='v']（child 指直接子元素，比较用它的
//         直接文本节点拼接；text() 指当前元素的直接文本节点拼接）。
// 不支持的写法报"表达式第 N 列：……"，而不是猜一个结果：父轴 ..、函数
// （count()/contains()/last()）、命名空间解析、| 联合、比较与算术运算符。
//
// 求值前先走 ParseXmlTree（它内部先 FormatXml 校验），因此"合法 XML"只有一处
// 定义，查询、树视图、校验 XML 三件工具对同一份文档的判定不可能不一致；
// 文档不合法时返回同一条错误（含行列）。

enum class XPathMatchKind { Element, Attribute, Text };

struct XPathMatch {
    XPathMatchKind kind = XPathMatchKind::Element;
    std::string    name;     // 元素 / 属性名；文本节点为空
    std::string    value;    // 属性值 / 文本节点的原文；元素为空（UTF-8）
    int            line = 1;
    std::wstring   preview;  // FormatXPathNodePreview 的结果（对话框列表直接用）
};

struct XPathResult {
    bool                    ok = false;
    std::wstring            error;  // 表达式错误或 XML 不合法（含行列）
    std::vector<XPathMatch> matches;
    bool                    truncated = false;      // 命中数超过上限，只回前一部分
    bool                    treeTruncated = false;  // 文档太大，结构树只建了前一部分
};

// 命中数上限：界面回归要能一次跑完，也防病态查询（//*）把内存放大。
// 截断时置 truncated，由界面说明"只列了前 N 条"。
inline constexpr std::size_t kMaxXPathMatches = 5000;

// 一个节点在列表里的样子（对话框与单测共用同一份格式约定，不再各写一份）：
//   元素  <name attr="v" ...>（无属性则 <name>）
//   属性  @name="value"
//   文本  #text="原文"
// 三种都按宽字符截到 60 并加 …——列表是索引，原文在文档里。
// 只有元素 / 属性 / 文本（含 CDATA）三类节点能进结果：文档根、注释、PI、
// DOCTYPE 没有"跳到那一行"的用法，一律不进。
std::wstring FormatXPathNodePreview(const XmlNode& node);

XPathResult XPathQuery(std::string_view xml, std::wstring_view expression);

}  // namespace npcc::core
