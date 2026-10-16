#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/MarkdownRender.h"

namespace npcc::shell {

// Markdown 内置预览面板（设计文档 2.13 第 4 条）。
//
// **为什么是自绘而不是塞一个浏览器控件**：Windows 上要嵌一个真正的浏览器引擎，
// 要么用 IE 的 WebBrowser 控件（要实现 IOleClientSite / IOleInPlaceSite 一整套
// COM 站点，几百行样板），要么用 WebView2（依赖外部运行库，便携版不能假设它装了）。
// 自绘的代价是渲染能力有限——**没有图片、表格、内嵌 HTML**，只有 Markdown 的结构、
// 字体与配色；换来的是零依赖、跟着主题走、而且"面板上到底显示成什么样"可以被
// 跨进程读回来核对（WM_GETTEXT 返回此刻渲染的那几行）。
// 需要完整排版时用"在浏览器中打开预览"——那条路上走的是**同一份**解析结果。
//
// 等宽排版：整块面板用一个等宽字体（与十六进制视图同一个理由——按列算位置最简单也最稳），
// 折行按字符算，中文一个字算一格。
class MarkdownPreview {
public:
    static void RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    HWND Handle() const noexcept { return hwnd_; }

    // 换一份内容（UTF-8）。太长的内容只渲染前一段并说明原因，免得一次重排把界面卡住
    void SetMarkdown(std::string utf8);
    void Clear();

    void SetVisible(bool visible) const;
    void Resize(const RECT& area) const;
    void SetDark(bool dark);

    // 此刻渲染出来的那几行纯文本（WM_GETTEXT 也返回它，界面回归靠它核对结构）
    std::string VisibleText() const;
    int  LineCount() const noexcept { return static_cast<int>(lines_.size()); }

private:
    struct Segment {
        core::MarkdownSpanKind kind = core::MarkdownSpanKind::Text;
        std::string            text;
    };
    struct Line {
        core::MarkdownBlockKind block = core::MarkdownBlockKind::Paragraph;
        int                     level = 0;
        bool                    rule = false;
        std::vector<Segment>    segments;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void Rebuild();                 // 解析 + 折行（换内容或换宽度时都要走一遍）
    void AppendBlockLines(const core::MarkdownBlock& block);
    void WrapInto(core::MarkdownBlockKind block, int level,
                  const std::vector<core::MarkdownSpan>& spans, const std::string& prefix);
    void UpdateScrollBar() const;
    void ScrollTo(int topLine);
    int  VisibleLineCount() const;
    void Paint(HDC dc, const RECT& client);
    std::string LineText(const Line& line) const;

    HWND                  hwnd_ = nullptr;
    HFONT                 font_ = nullptr;
    HFONT                 boldFont_ = nullptr;
    HFONT                 italicFont_ = nullptr;
    int                   charWidth_ = 8;
    int                   charHeight_ = 16;
    int                   dpi_ = 96;
    bool                  dark_ = false;
    std::vector<Line>     lines_;
    int                   topLine_ = 0;
    std::string           source_;      // 存着：换宽度时要重新折行
    std::string           notice_;      // 顶部的说明（内容太长之类）
};

}  // namespace npcc::shell
