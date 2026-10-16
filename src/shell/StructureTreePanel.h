#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/JsonTree.h"
#include "core/XmlTree.h"

namespace npcc::shell {

// 结构树面板（M3 的"内置 JSON 能力"与"内置 XML 工具"共用）：同一套自绘机制
// 显示两种树——显示行、折叠集合、点击跳转、WM_GETTEXT 读回。
//
// 自绘（与十六进制视图、Markdown 预览同一套路），理由也一样：树控件的项句柄跨进程传不了、
// 自绘的却能靠 WM_GETTEXT 把"此刻渲染的那几行"读回来核对。这里比另两块更需要的还有一点：
// **折起来的状态是面板自己的事**——用系统树控件要靠 TVM_EXPAND 驱动，用自绘就是一个
// 折叠集合。
//
// 一行一个节点：容器带折叠记号（v 展开 / > 折叠）；JSON 的标量显示 `"名字": 值`、
// XML 的元素显示 `<名字>`、属性显示 `@名字 = "值"`、文本显示 `"内容"`；点容器那一行是
// 折叠/展开，点标量 / 属性 / 文本是**跳到源码里对应的那一行**（树视图真正的用处：
// 在几千行的文件里找到那一项）。
class StructureTreePanel {
public:
    static void RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    HWND Handle() const noexcept { return hwnd_; }

    // 换一份内容。不是合法 JSON / XML 时面板里会说明原因（错误信息来自各自的格式化器，含行列）
    void SetJson(std::string utf8);
    void SetXml(std::string utf8);
    void Clear();

    void SetVisible(bool visible) const;
    void Resize(const RECT& area) const;
    void SetDark(bool dark);
    void Focus() const;

    // 点了某个标量 → 回调它的行号（1 基），外壳据此把光标挪过去
    void SetJumpCallback(std::function<void(int)> callback) { jumpCallback_ = std::move(callback); }

    std::string VisibleText() const;
    int  NodeCount() const noexcept { return nodeCount_; }
    bool Valid() const noexcept { return valid_; }

private:
    struct Row {
        const core::JsonNode* json = nullptr;
        const core::XmlNode*  xml = nullptr;
        int                   depth = 0;
    };

    struct Colours {
        COLORREF text = 0;
        COLORREF name = 0;
        COLORREF string = 0;
        COLORREF number = 0;
        COLORREF dim = 0;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void Rebuild();
    void FlattenJson(const core::JsonNode& node, int depth);
    void FlattenXml(const core::XmlNode& node, int depth, bool rootDocument);
    void UpdateScrollBar() const;
    void ScrollTo(int topLine);
    int  VisibleLineCount() const;
    bool ContainerOf(const Row& row) const;
    bool Collapsed(const Row& row) const;
    void Toggle(const Row& row);
    std::string RowText(const Row& row) const;
    std::string JsonRowText(const Row& row) const;
    std::string XmlRowText(const Row& row) const;
    int  DrawSegment(HDC dc, int x, int y, const std::string& text, COLORREF colour,
                     HFONT font) const;
    void PaintRowJson(HDC dc, const Row& row, int y, const Colours& colours) const;
    void PaintRowXml(HDC dc, const Row& row, int y, const Colours& colours) const;
    void Paint(HDC dc, const RECT& client);

    HWND                      hwnd_ = nullptr;
    HFONT                     font_ = nullptr;
    HFONT                     boldFont_ = nullptr;
    int                       charWidth_ = 8;
    int                       charHeight_ = 16;
    int                       dpi_ = 96;
    bool                      dark_ = false;
    bool                      isXml_ = false;   // 当前这一份内容是 XML 还是 JSON
    std::string               source_;
    core::JsonTreeResult      tree_;
    core::XmlTreeResult       xmlTree_;
    bool                      valid_ = false;
    int                       nodeCount_ = 0;
    std::vector<Row>          rows_;
    std::vector<const core::JsonNode*> collapsedJson_;
    std::vector<const core::XmlNode*>  collapsedXml_;
    int                       topLine_ = 0;
    std::function<void(int)>  jumpCallback_;
};

}  // namespace npcc::shell
