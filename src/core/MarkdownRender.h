#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// Markdown 的解析与渲染（设计文档 2.13 第 4 条：内置预览面板 + 在浏览器里打开）。
//
// 两件事共用**一次解析**：解析成"块 → 行内片段"之后，一边渲染成 HTML（给浏览器看），
// 一边给自绘的预览面板当排版输入。分成两份解析器的话，"面板里看到的"和"浏览器里看到的"
// 迟早会不一致——而这种不一致最难被发现（两边各自都"看着对"）。
//
// 支持的 Markdown 子集（如实列出，没支持的写在下面）：
//   标题（# ~ ######）、段落、围栏代码块（``` 或 ~~~）、无序/有序列表、引用（>）、
//   分隔线（--- *** ___）、行内 **粗体**／*斜体*／`行内代码`／[链接](地址)／反斜杠转义。
// **没有**支持：表格、脚注、任务列表、内嵌 HTML、图片、自动链接、代码块里的语法高亮。
// 挑这一档是因为它们覆盖了写文档时绝大多数用法，而每多一条都要多一套边界规则；
// 真需要完整 Markdown 的话，在浏览器里打开预览那条路走的是同一份解析结果，
// 也就是说"缺什么"是能一眼看出来的，不会两处不一致。

enum class MarkdownBlockKind { Heading, Paragraph, Code, List, Quote, Rule };
enum class MarkdownSpanKind { Text, Bold, Italic, Code, Link };

struct MarkdownSpan {
    MarkdownSpanKind kind = MarkdownSpanKind::Text;
    std::string      text;
    std::string      target;   // 只有 Link 用
};

struct MarkdownBlock {
    MarkdownBlockKind          kind = MarkdownBlockKind::Paragraph;
    int                        level = 0;      // 标题 1~6；列表缩进层级；引用层级
    bool                       ordered = false;// 列表：有序还是无序
    int                        number = 1;     // 有序列表里这一项的序号
    std::vector<MarkdownSpan>  spans;          // 标题／段落／列表项／引用的行内内容
    std::string                code;           // 代码块的原文（其它块为空）
};

std::vector<MarkdownBlock> ParseMarkdown(std::string_view text);
// 单独暴露：面板按行排版时要用同一套行内规则
std::vector<MarkdownSpan> ParseInline(std::string_view line);

// 一段行内片段的纯文本（预览面板与"读回来核对"都用它）
std::string SpanPlainText(const MarkdownSpan& span);
// 一整块摊成几行纯文本：标题/段落/列表/引用是一行，代码块按原样多行。
// 有序列表会带上 "1. " 这样的序号前缀，无序列表带 "• "——**前缀也算文本**，
// 因为面板上就是这么显示的，读回来核对时两边必须一致
std::vector<std::string> BlockPlainLines(const MarkdownBlock& block);

// 生成一份**独立的** HTML（带一小段内嵌样式），给浏览器看
std::string MarkdownToHtml(std::string_view text);

}  // namespace npcc::core
