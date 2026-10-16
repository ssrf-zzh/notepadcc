#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/JsonTree.h"

namespace npcc::shell {

// JSON 树面板（M3 的"内置 JSON 能力"之一）。
//
// 自绘（与十六进制视图、Markdown 预览同一套路），理由也一样：树控件的项句柄跨进程传不了、
// 自绘的却能靠 WM_GETTEXT 把"此刻渲染的那几行"读回来核对。这里比另两块更需要的还有一点：
// **折起来的状态是面板自己的事**——用系统树控件要靠 TVM_EXPAND 驱动，用自绘就是一个
// 折叠集合。
//
// 一行一个节点：容器带折叠记号（▾ 展开 / ▸ 折叠），标量显示 `名字: 值`；
// 点容器那一行是折叠/展开，点标量是**跳到源码里对应的那一行**（这是树视图真正的用处：
// 在几千行的 JSON 里找到那一项）。
class JsonTreePanel {
public:
    static void RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    HWND Handle() const noexcept { return hwnd_; }

    // 换一份内容。不是合法 JSON 时面板里会说明原因（错误信息来自 JsonFormat，含行列）
    void SetJson(std::string utf8);
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
        const core::JsonNode* node = nullptr;
        int                   depth = 0;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void Rebuild();
    void Flatten(const core::JsonNode& node, int depth);
    void UpdateScrollBar() const;
    void ScrollTo(int topLine);
    int  VisibleLineCount() const;
    bool Collapsed(const core::JsonNode* node) const;
    void Toggle(const core::JsonNode* node);
    std::string RowText(const Row& row) const;
    void Paint(HDC dc, const RECT& client);

    HWND                      hwnd_ = nullptr;
    HFONT                     font_ = nullptr;
    HFONT                     boldFont_ = nullptr;
    int                       charWidth_ = 8;
    int                       charHeight_ = 16;
    int                       dpi_ = 96;
    bool                      dark_ = false;
    std::string               source_;
    core::JsonTreeResult      tree_;
    bool                      valid_ = false;
    int                       nodeCount_ = 0;
    std::vector<Row>          rows_;
    std::vector<const core::JsonNode*> collapsed_;
    int                       topLine_ = 0;
    std::function<void(int)>  jumpCallback_;
};

}  // namespace npcc::shell
