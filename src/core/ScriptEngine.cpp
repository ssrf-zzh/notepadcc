#include "core/ScriptEngine.h"

#include <algorithm>
#include <cwctype>

namespace npcc::core {
namespace {

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

bool HasPyExtension(std::wstring_view fileName) {
    if (fileName.size() <= 3) {
        return false;   // ".py" 本身不算一个脚本
    }
    return Lower(fileName.substr(fileName.size() - 3)) == L".py";
}

}  // namespace

std::filesystem::path ScriptDirectory(const std::filesystem::path& configRoot) {
    return configRoot / L"scripts";
}

std::vector<ScriptInfo> ListScriptsFromFileNames(const std::filesystem::path& directory,
                                                 const std::vector<std::wstring>& fileNames) {
    std::vector<ScriptInfo> scripts;
    for (const std::wstring& fileName : fileNames) {
        if (fileName.empty() || fileName.front() == L'.') {
            continue;   // 隐藏文件（编辑器备份、资源管理器残留）不是脚本
        }
        if (!HasPyExtension(fileName)) {
            continue;
        }
        ScriptInfo info;
        info.name = fileName.substr(0, fileName.size() - 3);
        if (info.name.empty()) {
            continue;
        }
        info.path = (directory / fileName).wstring();
        scripts.push_back(std::move(info));
    }
    // 按名字排序（不区分大小写）。同名的只留第一个：名字就是菜单项，
    // 两条一模一样的菜单项用户分不清哪条是哪条
    std::stable_sort(scripts.begin(), scripts.end(), [](const ScriptInfo& a, const ScriptInfo& b) {
        return Lower(a.name) < Lower(b.name);
    });
    std::vector<ScriptInfo> unique;
    for (ScriptInfo& info : scripts) {
        const bool duplicate =
            std::any_of(unique.begin(), unique.end(), [&info](const ScriptInfo& kept) {
                return Lower(kept.name) == Lower(info.name);
            });
        if (!duplicate) {
            unique.push_back(std::move(info));
        }
    }
    return unique;
}

std::wstring QuoteCommandLineArgument(std::wstring_view argument) {
    // 规则与 MSVCRT 的解析器对齐：没有空白、没有引号时原样返回——
    // 没有引号时反斜杠不特殊，末尾是反斜杠也安全
    bool needQuotes = argument.empty();
    for (const wchar_t ch : argument) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needQuotes = true;
            break;
        }
    }
    if (!needQuotes) {
        return std::wstring(argument);
    }
    std::wstring out;
    out.push_back(L'"');
    int backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            // 引号前的连续反斜杠翻倍，再给引号本身加一层转义
            out.append(static_cast<std::size_t>(backslashes) * 2 + 1, L'\\');
            backslashes = 0;
            out.push_back(L'"');
            continue;
        }
        out.append(static_cast<std::size_t>(backslashes), L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    // 收尾引号前的连续反斜杠也要翻倍
    out.append(static_cast<std::size_t>(backslashes) * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::vector<ScriptInterpreter> ScriptInterpreterCandidates(std::wstring_view configured) {
    std::vector<ScriptInterpreter> candidates;
    if (!configured.empty()) {
        candidates.push_back(ScriptInterpreter{std::wstring(configured), false});
        return candidates;   // 配置说了算：配错了就报错，不偷偷换成 PATH 里的
    }
    candidates.push_back(ScriptInterpreter{L"python.exe", false});
    candidates.push_back(ScriptInterpreter{L"py.exe", true});
    return candidates;
}

std::wstring BuildScriptCommandLine(const ScriptInterpreter& interpreter,
                                    std::wstring_view scriptPath) {
    std::wstring line = QuoteCommandLineArgument(interpreter.executable);
    if (interpreter.launcher) {
        line += L" -3";
    }
    line += L' ';
    line += QuoteCommandLineArgument(scriptPath);
    return line;
}

}  // namespace npcc::core
