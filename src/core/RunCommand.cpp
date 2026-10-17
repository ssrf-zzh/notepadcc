#include "core/RunCommand.h"

#include <algorithm>
#include <cwctype>

#include "infra/Xml.h"

namespace npcc::core {
namespace {

std::wstring Upper(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    return out;
}

// 文件所在目录（不含结尾分隔符）。未保存的文档没有目录，返回空。
std::wstring DirectoryOf(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) {
        return {};
    }
    return std::wstring(path.substr(0, slash));
}

std::wstring FileNameOf(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

// 去掉最后一个扩展名（"a.tar.gz" 的 NAME_PART 是 "a.tar"）
std::wstring NamePartOf(std::wstring_view fileName) {
    const std::size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) {
        return std::wstring(fileName);
    }
    return std::wstring(fileName.substr(0, dot));
}

std::wstring ExtPartOf(std::wstring_view fileName) {
    const std::size_t dot = fileName.find_last_of(L'.');
    // dot == 0 表示"以点开头"（.gitignore 这种）：Windows 的 PathFindExtension
    // 也把它算成没有扩展名，这里跟着走——否则 NAME_PART 与 EXT_PART 会凑不回原名
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= fileName.size()) {
        return {};
    }
    return std::wstring(fileName.substr(dot + 1));
}

// 三个修饰键分开写，保存与读取时保持对称
void WriteModifiers(infra::XmlElement& node, uint8_t modifiers) {
    node.SetAttributeBool(L"Ctrl", (modifiers & kModCtrl) != 0);
    node.SetAttributeBool(L"Alt", (modifiers & kModAlt) != 0);
    node.SetAttributeBool(L"Shift", (modifiers & kModShift) != 0);
}

uint8_t ReadModifiers(const infra::XmlElement& node) {
    uint8_t modifiers = kModNone;
    if (node.AttributeBool(L"Ctrl", false)) {
        modifiers |= kModCtrl;
    }
    if (node.AttributeBool(L"Alt", false)) {
        modifiers |= kModAlt;
    }
    if (node.AttributeBool(L"Shift", false)) {
        modifiers |= kModShift;
    }
    return modifiers;
}

}  // namespace

std::wstring ExpandRunCommand(std::wstring_view command, const RunContext& context) {
    const std::wstring fileName = FileNameOf(context.fullPath);
    const std::wstring currentLine = std::to_wstring(context.currentLine);
    std::wstring out;
    out.reserve(command.size() + 16);

    std::size_t index = 0;
    while (index < command.size()) {
        const wchar_t ch = command[index];
        if (ch != L'$' || index + 1 >= command.size() || command[index + 1] != L'(') {
            out.push_back(ch);
            ++index;
            continue;
        }
        const std::size_t close = command.find(L')', index + 2);
        if (close == std::wstring_view::npos) {
            // 少了右括号：只把 `$` 原样吐出来继续走，别把后面整段都吞掉
            out.push_back(ch);
            ++index;
            continue;
        }
        const std::wstring name = Upper(command.substr(index + 2, close - index - 2));

        bool         known = true;
        std::wstring scratch;   // 现算的值（目录、名字分段）：得活到 append 之后
        const std::wstring* value = nullptr;
        if (name == L"FULL_CURRENT_PATH") {
            value = &context.fullPath;
        } else if (name == L"CURRENT_DIRECTORY") {
            scratch = DirectoryOf(context.fullPath);
        } else if (name == L"FILE_NAME") {
            value = &fileName;
        } else if (name == L"NAME_PART") {
            scratch = NamePartOf(fileName);
        } else if (name == L"EXT_PART") {
            scratch = ExtPartOf(fileName);
        } else if (name == L"CURRENT_LINE") {
            value = &currentLine;
        } else if (name == L"CURRENT_WORD") {
            value = &context.currentWord;
        } else if (name == L"APP_DIRECTORY") {
            value = &context.appDirectory;
        } else if (name == L"CURRENT_ENCODING") {
            value = &context.encodingName;
        } else if (name == L"CURRENT_EOL") {
            value = &context.eolName;
        } else {
            // 不认识的变量：原样留着（见头文件里的理由）
            known = false;
        }

        if (!known) {
            out.append(command.substr(index, close - index + 1));
        } else if (value != nullptr) {
            out.append(*value);
        } else {
            // 认出来但算出来是空的（例如未保存文档的 $(CURRENT_DIRECTORY)）：
            // 展开成空——留着字面量只会让命令跑错目标
            out.append(scratch);
        }
        index = close + 1;
    }
    return out;
}

void AppendRunCommandsXml(infra::XmlElement& root,
                          const std::vector<RunCommandDef>& commands) {
    if (commands.empty()) {
        return;   // 一条都没有就不写空节，免得文件里多出个没用的壳
    }
    infra::XmlElementPtr section = root.AddChild(L"UserDefinedCommands");
    for (const RunCommandDef& def : commands) {
        if (def.name.empty() || def.command.empty()) {
            continue;
        }
        infra::XmlElementPtr node = section->AddChild(L"Command");
        node->SetAttribute(L"name", def.name);
        WriteModifiers(*node, def.modifiers);
        node->SetAttributeInt(L"Key", static_cast<int>(def.keyCode));
        // 本项目新增的属性：一条命令是不是要把输出收进新文档
        node->SetAttributeBool(L"capture", def.captureOutput);
        node->text = def.command;
    }
}

std::vector<RunCommandDef> ReadRunCommandsXml(const infra::XmlElement& root,
                                              std::vector<std::wstring>& warnings) {
    std::vector<RunCommandDef> commands;
    const infra::XmlElement* section = root.Child(L"UserDefinedCommands");
    if (section == nullptr) {
        return commands;   // 没有这一节 = 没有运行命令，不是错误
    }
    for (const infra::XmlElement* node : section->Children(L"Command")) {
        if (node == nullptr) {
            continue;
        }
        RunCommandDef def;
        const std::wstring* name = node->Attribute(L"name");
        if (name != nullptr) {
            def.name = *name;
        }
        def.command = node->text;
        // 文本可能被写成带首尾空白的形态（手写的文件常见），去掉再判断
        while (!def.command.empty() && (def.command.front() == L'\r' ||
                                        def.command.front() == L'\n' ||
                                        def.command.front() == L' ' ||
                                        def.command.front() == L'\t')) {
            def.command.erase(def.command.begin());
        }
        while (!def.command.empty() && (def.command.back() == L'\r' ||
                                        def.command.back() == L'\n' ||
                                        def.command.back() == L' ' ||
                                        def.command.back() == L'\t')) {
            def.command.pop_back();
        }
        if (def.name.empty() || def.command.empty()) {
            warnings.push_back(L"shortcuts.xml 里有一条运行命令缺名字或命令行，已跳过");
            continue;
        }
        def.modifiers = ReadModifiers(*node);
        const int key = node->AttributeInt(L"Key", 0);
        def.keyCode = (key > 0 && key <= 0xFFFF) ? static_cast<uint16_t>(key) : 0;
        def.captureOutput = node->AttributeBool(L"capture", true);
        commands.push_back(std::move(def));
    }
    return commands;
}

}  // namespace npcc::core
