#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// 脚本插件（M3 第十二块）：<配置根>\scripts 里的 .py 文件变成工具菜单里的命令。
// 与宏、运行命令同一套编号做法——脚本命令从 128 起。这类命令带 kCmdUserDefined
// 标志：列表跟着目录内容走，把编号存进快捷键文件，下次启动就会绑到别的脚本上。
inline constexpr std::uint16_t kScriptCommandBase = 128;

// 一条可用脚本：名字 = 文件名去掉 .py（就是菜单项文本），路径是完整路径。
struct ScriptInfo {
    std::wstring name;
    std::wstring path;
};

// 脚本目录：<配置根>\scripts。目录不存在也算"没有脚本"，需要时由调用方创建。
std::filesystem::path ScriptDirectory(const std::filesystem::path& configRoot);

// 从"目录里的文件名清单"整理出脚本列表（纯函数；读目录由调用方做）。
// 规则：只认 .py（大小写不敏感）；跳过以 . 开头的隐藏文件与只叫 ".py" 的空名；
// 按名字**不区分大小写**排序；同名（只有大小写不同）只留第一个。
std::vector<ScriptInfo> ListScriptsFromFileNames(const std::filesystem::path& directory,
                                                 const std::vector<std::wstring>& fileNames);

// 按 MSVCRT 的命令行解析规则给一个参数加引号。空串、含空白、含引号的参数要包引号；
// 包引号时"引号前的连续反斜杠"要翻倍，否则收尾的引号会被反斜杠吃掉。
// 纯函数：解释器与脚本路径都可能带空格，拼命令行之前必须过这一手。
std::wstring QuoteCommandLineArgument(std::wstring_view argument);

// 解释器：exe 路径 + 是不是 py.exe 启动器。启动器要补一个 -3，明确要 Python 3——
// 机器上同时装着 2.x 时不让它挑错版本。
struct ScriptInterpreter {
    std::wstring executable;
    bool         launcher = false;
};

// 解释器候选（纯函数，只排顺序，不探测）：配置里写了就只认它（配错了要报出来，
// 不偷偷改找别的）；否则按 python.exe → py.exe 的顺序找。探测（PATH / 文件在不在）
// 由 infra 层做。
std::vector<ScriptInterpreter> ScriptInterpreterCandidates(std::wstring_view configured);

// 组装脚本命令行："<解释器>" [-3] "<脚本>"。两个参数都走 QuoteCommandLineArgument。
std::wstring BuildScriptCommandLine(const ScriptInterpreter& interpreter,
                                    std::wstring_view scriptPath);

}  // namespace npcc::core
