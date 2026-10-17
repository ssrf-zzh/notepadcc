#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace npcc::shell {

// 四段式状态栏（M3 第十四块改成自绘）：① 光标位置与选中字节数 ② 编码与行尾
// ③ 语言与修改状态 ④ 宏录制指示；空闲时显示作者标识，单击打开“关于”。
//
// 文本仍然存进控件本身（SB_SETTEXTW / SB_GETTEXTW 的语义一点没动，界面回归
// 读回走的就是官方这条路），只是"画"这一层接了过来：平底、细分区线、右下角
// 自己画缩放角标——系统主题那套立体压边跟现代 chrome 摆在一起太出戏。
class StatusBar {
public:
    ~StatusBar();

    bool Create(HWND parent, int controlId);
    void Resize(const RECT& client) const;

    // 现代 chrome 的配色（与工具栏 / 标签栏同一套，见 ChromeStyle.h）
    void SetTheme(bool dark, bool eyeCare);

    void SetPosition(int32_t line, int32_t column, int32_t selectedBytes,
                     bool overwrite = false) const;
    // 临时消息：写进第一段，下一次 UpdateStatus 会被正常内容覆盖
    void SetMessage(const std::wstring& text) const;
    void SetDocumentInfo(const std::wstring& encoding, const std::wstring& eol,
                         const std::wstring& language, bool modified) const;
    // 宏录制指示（设计文档 2.9 第 2 条）：录制期间第 4 段显示 ● REC
    void SetMacroRecording(bool recording);
    // 第 4 段空闲时可点击的作者标识；tooltip 在悬停时显示完整作者信息。
    void SetAuthor(const std::wstring& text, const std::wstring& tooltip);

    HWND Handle() const noexcept { return hwnd_; }
    int  Height() const;

private:
    static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR subclassId,
                                         DWORD_PTR refData);
    void Paint(HDC dc) const;
    void SetText(int part, const std::wstring& text) const;

    HWND  hwnd_ = nullptr;
    HFONT font_ = nullptr;
    bool  dark_ = false;
    bool  eyeCare_ = false;
    std::wstring authorText_;
    std::wstring authorTooltip_;
    bool recording_ = false;
};

}  // namespace npcc::shell
