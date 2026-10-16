#pragma once

#include <windows.h>

namespace npcc::shell {

// 现代 chrome 的一套配色（M3 第十四块）：工具栏 / 标签栏 / 状态栏共用。
// 三档跟编辑区一致——浅色 / 深色 / 护眼（护眼再跟深浅成四套）。
//
// 为什么和 EditorPalette 分开：编辑区的颜色被界面回归按色值钉得很死
// （暗色模式、护眼各自核对过具体 RGB），外壳这些颜色只服务观感，改动它不该碰编辑区。
struct ChromeColours {
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

// 圆角矩形填充：描边笔与填充同色，免得 RoundRect 的细边把圆角画出一圈毛刺。
void FillRoundedRect(HDC dc, const RECT& rc, int radius, COLORREF fill);

// Windows 11 的窗口外观：深色标题栏 + 圆角 + 标题栏颜色跟随 chrome。
// 系统不支持某项属性（老版本 / 远程会话）时静默跳过——观感增强不该让程序不能用。
void ApplyModernWindowChrome(HWND hwnd, bool dark, bool eyeCare);

// 进程级深浅偏好：走 uxtheme 的两个未公开序号，让系统画的菜单也跟着深浅走。
// 序号找不到（老系统）就什么都不做——这是"够到哪一层"的边界。
void ApplyDarkModePreference(bool dark);

}  // namespace npcc::shell
