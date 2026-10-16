#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Command.h"

namespace npcc::infra {
struct XmlElement;
}

namespace npcc::core {

// 运行命令在命令 ID 空间里的位置（与宏那一套一致）：
// 运行类（9）的第 1 号是"运行…"对话框本身，第 kRunCommandBase 号起按存储顺序
// 排用户定义的那些。ShortcutStore 回填快捷键时要用同一个算法，所以放在这里，
// 不许在别处再写一遍字面量。
inline constexpr uint16_t kRunCommandDialogIndex = 1;
inline constexpr uint16_t kRunCommandBase = 128;

// 运行命令的变量上下文（设计文档 2.13 第 3 条）。
//
// 变量名**大小写不敏感**（写 $(file_name) 也认），认不出来的 $(...) 原样留着：
// 静默换成空串会让命令悄悄跑错目标，那种错误比"命令没跑起来"难查得多。
struct RunContext {
    std::wstring fullPath;          // $(FULL_CURRENT_PATH)：当前文件完整路径（未保存的文档为空）
    std::wstring appDirectory;      // $(NPP_DIRECTORY)：程序所在目录（为兼容保留这个名字）
    std::wstring currentWord;       // $(CURRENT_WORD)：光标处的词（没有词就是空）
    int          currentLine = 1;   // $(CURRENT_LINE)：光标所在行（1 基）
    std::wstring encodingName;      // $(CURRENT_ENCODING)：UTF-8 / ANSI / UTF-16LE …
    std::wstring eolName;           // $(CURRENT_EOL)：CRLF / LF / CR / Mixed
};

// 展开 $(...) 变量。纯函数：给命令行与上下文，返回展开后的命令行，不做 IO。
std::wstring ExpandRunCommand(std::wstring_view command, const RunContext& context);

// 一条用户定义的运行命令（shortcuts.xml 的 <UserDefinedCommands> 一节）。
//
// 变量是在**运行时**才展开的：存进文件的是 $(FULL_CURRENT_PATH) 这种原文，
// 这样同一条命令在任何文件上都能用；存展开后的结果等于把它钉死在当时的那个文件上。
struct RunCommandDef {
    std::wstring name;                  // 名字既是标识也是菜单项文本
    std::wstring command;               // 命令行（原文，含变量）
    uint8_t      modifiers = kModNone;  // 绑定的快捷键（可以没有）
    uint16_t     keyCode = 0;
    bool         captureOutput = true;  // 是否把输出收进一个新文档
};

// 只做"运行命令 ↔ 那段 XML"的转换；文件读写由 ShortcutStore 负责
// （一份文件只留一个写者，见 ShortcutStore 的说明）。
void AppendRunCommandsXml(infra::XmlElement& root, const std::vector<RunCommandDef>& commands);
// 缺 name 或空命令行的记录会跳过并记一条警告
std::vector<RunCommandDef> ReadRunCommandsXml(const infra::XmlElement& root,
                                              std::vector<std::wstring>& warnings);

}  // namespace npcc::core
