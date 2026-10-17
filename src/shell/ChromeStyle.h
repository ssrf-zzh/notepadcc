#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace npcc::shell {

// 现代 chrome 的一套配色（M3 第十四块）：工具栏 / 标签栏 / 状态栏共用。
// 三档跟编辑区一致——浅色 / 深色 / 护眼（护眼再跟深浅成四套）。
//
// 为什么和 EditorPalette 分开：编辑区的颜色被界面回归按色值钉得很死
// （暗色模式、护眼各自核对过具体 RGB），外壳这些颜色只服务观感，改动它不该碰编辑区。
struct ChromeColours {
    COLORREF menuBar = 0;        // 菜单栏的底（比工具栏略亮 / 略深一档）
    COLORREF strip = 0;          // 工具栏 / 标签栏 / 状态栏的底色
    COLORREF tabActive = 0;      // 当前标签的底（与编辑区同色，视觉上连成一片）
    COLORREF tabHover = 0;       // 悬停标签的底
    COLORREF text = 0;           // 主要文本 / 图标
    COLORREF textMuted = 0;      // 次要文本（未选中标签、状态栏）
    COLORREF line = 0;           // 细分隔线（底边、分区界线）
    COLORREF accent = 0;         // 强调色（当前标签下划线、勾选图标）
    COLORREF accentSoft = 0;     // 强调色的浅底（勾上的工具按钮）
    COLORREF buttonHot = 0;      // 工具按钮悬停底
    COLORREF buttonPressed = 0;  // 工具按钮按下底
    COLORREF iconDisabled = 0;   // 灰掉的图标
    COLORREF danger = 0;         // 关闭按钮悬停时的叉
    COLORREF dangerSoft = 0;     // 关闭按钮悬停时的底
    COLORREF statusText = 0;     // 状态栏正文
};

ChromeColours ChromeColoursFor(bool dark, bool eyeCare);

// 当前进程的 chrome 主题。主窗口在每次切换主题时更新一次；模态业务对话框在
// 构造完成后调用 ApplyDialogChrome，就能拿到与主窗口一致的配色。
void SetChromeThemeState(bool dark, bool eyeCare);
ChromeColours CurrentChromeColours();

// 让系统控件（编辑框 / 组合框 / 列表 / 滚动条）跟上深色主题。
// 按钮与复选框不走这里：它们由查找栏、搜索面板自己画，保证四套主题的圆角与文字颜色一致。
// darkTheme 传 L"DarkMode_CFD" 适合输入框/组合框，L"DarkMode_Explorer" 适合列表/滚动条。
void ApplyControlTheme(HWND control, bool dark,
                       const wchar_t* darkTheme = L"DarkMode_Explorer");

// 外壳统一字体：优先 Segoe UI Variable（Win11），退到 Segoe UI；中文交给系统字体链接。
// pointTenths 是"十分之一点"（95 = 9.5 pt），semibold 用于当前标签与菜单项。
HFONT CreateChromeFont(int dpi, int pointTenths, bool semibold);

// 把 "文件(&F)" 拆成显示用的 "文件" 与助记字母 F。没有助记后缀时 mnemonic 返回 L'\0'。
std::wstring ChromeMenuLabel(std::wstring_view title, wchar_t* mnemonic);

// 圆角矩形填充：描边笔与填充同色，免得 RoundRect 的细边把圆角画出一圈毛刺。
void FillRoundedRect(HDC dc, const RECT& rc, int radius, COLORREF fill);

// 自绘按钮与复选框（查找栏 / 搜索面板共用）。rc 是控件客户区，font 与 dpi 由调用方给。
void DrawChromePushButton(HDC dc, const RECT& rc, std::wstring_view text, HFONT font,
                          const ChromeColours& colours, int dpi, bool dark, bool primary,
                          bool hovered, bool pressed, bool focused, bool enabled);

void DrawChromeCheckBox(HDC dc, const RECT& rc, std::wstring_view text, HFONT font,
                        const ChromeColours& colours, int dpi, bool dark, bool checked,
                        bool hovered, bool focused, bool enabled);

// Windows 11 的窗口外观：深色标题栏 + 圆角 + 标题栏颜色跟随 chrome。
// 系统不支持某项属性（老版本 / 远程会话）时静默跳过——观感增强不该让程序不能用。
void ApplyModernWindowChrome(HWND hwnd, bool dark, bool eyeCare);

// 让手工创建的业务对话框跟上四套 chrome 配色：标题栏、窗口底色、静态文字、
// 输入框/列表、滚动条与按钮/复选框一次处理。只处理自绘窗口，不碰系统 MessageBox。
void ApplyDialogChrome(HWND hwnd);

// 进程级深浅偏好：走 uxtheme 的两个未公开序号，让系统画的菜单也跟着深浅走。
// 序号找不到（老系统）就什么都不做——这是"够到哪一层"的边界。
void ApplyDarkModePreference(bool dark);

}  // namespace npcc::shell
