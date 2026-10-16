#pragma once

#include <cstdint>

namespace npcc::edit {

// 编辑器区里**不属于语法样式**的那些颜色：行号边距、书签与折叠记号、光标、当前行高亮、
// 选区背景、空白符号。
//
// 为什么不放进 StyleScheme：stylers.xml 是用户可以整份替换的配色文件，而"深色下边距该是
// 深灰、光标该是浅色"这件事得有人兜住——不能指望用户把每个记号都调一遍，更不能让用户
// 换一份配色就把边距变成刺眼的白。所以：**语法颜色**（按语言、按角色/样式号）走
// StyleScheme，**控件自己的皮肤**走这里，两者都由"当前是浅色还是深色"决定。
//
// 颜色一律写成 0xRRGGBB（与配置文件同一套写法），交给 Scintilla 时再转 COLORREF
// （见 StyleScheme::ToScintillaColour）。
struct EditorPalette {
    bool          dark = false;
    std::uint32_t lineNumberFore = 0x808080;   // 行号
    std::uint32_t lineNumberBack = 0xF5F5F5;
    std::uint32_t caret = 0x000000;            // 光标
    std::uint32_t caretLineBack = 0xF2F6FA;    // 当前行高亮
    std::uint32_t selectionBack = 0xCCE0F5;    // 选区
    std::uint32_t bookmarkFore = 0xFFFFFF;     // 书签圆点
    std::uint32_t bookmarkBack = 0x2E86C1;
    std::uint32_t foldFore = 0x606060;         // 折叠记号（+/−）
    std::uint32_t foldBack = 0xFFFFFF;
    std::uint32_t whitespace = 0;              // 空白符号/行尾符；0 = 用文字色（浅色下如此）
    std::uint32_t braceFore = 0x000000;        // 配对括号高亮（前景/背景）
    std::uint32_t braceBack = 0xC8E8C8;
    std::uint32_t braceBadBack = 0xF6C6C6;     // 没有配对的孤立括号
    // 护眼模式（低蓝光）对默认前景/背景的覆盖：0 = 不覆盖，照用配色方案里的值。
    // 只有 EyeCare() 工厂会填这两项；语法 token 的颜色不经过这里（见设计说明书 2.7 第 7 条）。
    std::uint32_t defaultFore = 0;
    std::uint32_t defaultBack = 0;

    static EditorPalette Light() {
        EditorPalette palette;
        palette.dark = false;
        return palette;
    }

    static EditorPalette Dark() {
        EditorPalette palette;
        palette.dark = true;
        palette.lineNumberFore = 0x858585;
        palette.lineNumberBack = 0x2D2D2D;
        palette.caret = 0xD4D4D4;          // 深色底上再用黑色光标就看不见了
        palette.caretLineBack = 0x282828;  // 当前行要"更亮一点"才辨得出来
        palette.selectionBack = 0x264F78;
        palette.bookmarkFore = 0xFFFFFF;
        palette.bookmarkBack = 0x2E86C1;
        palette.foldFore = 0xC0C0C0;
        palette.foldBack = 0x2D2D2D;
        palette.whitespace = 0x3F3F3F;     // 白点压在深底上太扎眼，压暗
        palette.braceFore = 0xF0F0F0;
        palette.braceBack = 0x2F5230;
        palette.braceBadBack = 0x5A2A2A;
        return palette;
    }

    // 护眼模式（低蓝光）：在对应浅色 / 深色皮肤的基础上，把背景相关几项换成暖色系，
    // 并给默认前景/背景一对低蓝光覆盖值。**只动这几项**——书签、括号高亮等仍跟着
    // 浅/深原版；语法 token 的颜色也不在这里，保持主题原样（不然语法高亮会被洗掉）。
    static EditorPalette EyeCare(bool darkTheme) {
        EditorPalette palette = darkTheme ? Dark() : Light();
        if (darkTheme) {
            palette.defaultFore = 0xE8DCC6;      // 暖白
            palette.defaultBack = 0x201A15;      // 暖棕黑（蓝通道最低）
            palette.lineNumberFore = 0x99896F;
            palette.lineNumberBack = 0x2A231C;
            palette.caret = 0xE8DCC6;
            palette.caretLineBack = 0x2B241D;
            palette.selectionBack = 0x4A381F;
        } else {
            palette.defaultFore = 0x3B362C;      // 暖深灰
            palette.defaultBack = 0xF4E8D4;      // 米色纸面（蓝通道最低）
            palette.lineNumberFore = 0x8C7F66;
            palette.lineNumberBack = 0xEADBBD;
            palette.caret = 0x3B362C;
            palette.caretLineBack = 0xEEDFC0;
            palette.selectionBack = 0xDCC7A0;
        }
        return palette;
    }
};

}  // namespace npcc::edit
