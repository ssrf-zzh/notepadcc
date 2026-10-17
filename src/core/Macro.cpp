#include "core/Macro.h"

#include <algorithm>
#include <cwchar>

#include "infra/Xml.h"

namespace npcc::core {
namespace {

bool ParseIntAttrib(const infra::XmlElement& node, const wchar_t* key, intptr_t fallback,
                    intptr_t& out) {
    const std::wstring* raw = node.Attribute(key);
    if (raw == nullptr || raw->empty()) {
        out = fallback;
        return false;
    }
    wchar_t* end = nullptr;
    const long long value = ::wcstoll(raw->c_str(), &end, 10);
    if (end == raw->c_str()) {
        out = fallback;
        return false;
    }
    out = static_cast<intptr_t>(value);
    return true;
}

}  // namespace

void MacroRecorder::Start() {
    Reset();
    recording_ = true;
}

void MacroRecorder::Stop() {
    FlushPendingText();
    recording_ = false;
}

void MacroRecorder::Reset() {
    actions_.clear();
    lastCharMs_ = 0;
}

void MacroRecorder::FlushPendingText() {
    // 文本在 RecordChar 里就是"追加进最后一步"，所以这里不用再做什么——
    // 留这个函数是为了给调用方一个明确的"落定"点（命令到来、停止、回放之前），
    // 也避免以后改成延迟合并时忘了这些时机
}

void MacroRecorder::RecordCommand(const Command& command) {
    if (!recording_) {
        return;
    }
    MacroAction action;
    action.kind = MacroAction::Kind::Command;
    action.commandId = command.id;
    action.commandName = command.key;
    actions_.push_back(std::move(action));
    lastCharMs_ = 0;   // 命令之后是"新的文本段"
}

void MacroRecorder::RecordMessage(uint32_t message, intptr_t wParam, intptr_t lParam) {
    if (!recording_) {
        return;
    }
    MacroAction action;
    action.kind = MacroAction::Kind::ScintillaMessage;
    action.message = message;
    action.wParam = wParam;
    action.lParam = lParam;
    actions_.push_back(std::move(action));
    lastCharMs_ = 0;
}

void MacroRecorder::RecordChar(int ch, int64_t nowMs) {
    if (!recording_ || ch <= 0) {
        return;
    }
    const bool continueText = lastCharMs_ != 0 && nowMs - lastCharMs_ <= kTextMergeIdleMs &&
                              !actions_.empty() &&
                              actions_.back().kind == MacroAction::Kind::InsertText;
    if (!continueText) {
        MacroAction action;
        action.kind = MacroAction::Kind::InsertText;
        actions_.push_back(std::move(action));
    }
    actions_.back().text.push_back(static_cast<wchar_t>(ch));
    lastCharMs_ = nowMs;
}

MacroRunResult RunMacro(const Macro& macro, const MacroRunner& runner, int repeatCount,
                        bool stopOnError) {
    MacroRunResult result;
    if (macro.Empty()) {
        result.ok = false;
        result.message = L"这条宏没有任何步骤";
        return result;
    }
    const int repeat = std::max(1, repeatCount);
    for (int round = 0; round < repeat; ++round) {
        for (std::size_t index = 0; index < macro.actions.size(); ++index) {
            if (runner.keepGoing && !runner.keepGoing()) {
                // 用户中断：不算失败，但要如实报告执行到哪儿
                result.message = L"已中断";
                return result;
            }
            const MacroAction& action = macro.actions[index];
            bool stepOk = true;
            switch (action.kind) {
                case MacroAction::Kind::Command:
                    stepOk = runner.runCommand && runner.runCommand(action.commandId);
                    break;
                case MacroAction::Kind::ScintillaMessage:
                    if (runner.sendMessage) {
                        runner.sendMessage(action);
                    } else {
                        stepOk = false;
                    }
                    break;
                case MacroAction::Kind::InsertText:
                    if (runner.insertText) {
                        runner.insertText(action.text);
                    } else {
                        stepOk = false;
                    }
                    break;
            }
            if (!stepOk) {
                result.ok = false;
                result.failedStep = static_cast<int>(index) + 1;
                result.failedRepeat = round + 1;
                result.message = L"第 " + std::to_wstring(result.failedStep) + L" 步失败";
                if (stopOnError) {
                    return result;
                }
            }
        }
        ++result.completed;
    }
    return result;
}

void AppendMacrosXml(infra::XmlElement& root, const std::vector<Macro>& macros) {
    if (macros.empty()) {
        return;   // 一条宏都没有就不写这一节，免得文件里多一个空壳
    }
    infra::XmlElementPtr node = root.AddChild(L"Macros");
    for (const Macro& macro : macros) {
        infra::XmlElementPtr item = node->AddChild(L"Macro");
        item->SetAttribute(L"name", macro.name);
        // 保持简单、稳定的 XML 写法，便于手工编辑
        item->SetAttributeBool(L"Ctrl", (macro.modifiers & kModCtrl) != 0);
        item->SetAttributeBool(L"Alt", (macro.modifiers & kModAlt) != 0);
        item->SetAttributeBool(L"Shift", (macro.modifiers & kModShift) != 0);
        item->SetAttributeInt(L"Key", static_cast<int>(macro.keyCode));
        for (const MacroAction& action : macro.actions) {
            infra::XmlElementPtr step = item->AddChild(L"Action");
            step->SetAttributeInt(L"type", static_cast<int>(action.kind));
            switch (action.kind) {
                case MacroAction::Kind::Command:
                    // message 放本项目的命令 ID
                    step->SetAttributeInt(L"message", static_cast<int>(action.commandId));
                    step->SetAttributeInt(L"wParam", 0);
                    step->SetAttributeInt(L"lParam", 0);
                    if (!action.commandName.empty()) {
                        step->SetAttribute(L"name", action.commandName);
                    }
                    break;
                case MacroAction::Kind::ScintillaMessage:
                    step->SetAttributeInt(L"message", static_cast<int>(action.message));
                    step->SetAttributeInt(L"wParam", static_cast<int>(action.wParam));
                    step->SetAttributeInt(L"lParam", static_cast<int>(action.lParam));
                    break;
                case MacroAction::Kind::InsertText:
                    step->SetAttributeInt(L"message", 0);
                    step->SetAttributeInt(L"wParam", 0);
                    step->SetAttributeInt(L"lParam", 0);
                    step->SetAttribute(L"sParam", action.text);
                    break;
            }
        }
    }
}

std::vector<Macro> ReadMacrosXml(const infra::XmlElement& root,
                                 const std::function<CmdId(const std::wstring&)>& commandKeyToId,
                                 std::vector<std::wstring>& warnings) {
    std::vector<Macro> macros;
    const infra::XmlElement* node = root.Child(L"Macros");
    if (node == nullptr) {
        return macros;
    }
    for (const infra::XmlElement* item : node->Children(L"Macro")) {
        if (item == nullptr) {
            continue;
        }
        Macro macro;
        macro.name = item->AttributeOr(L"name", L"");
        if (macro.name.empty()) {
            warnings.push_back(L"shortcuts.xml 里有一条没有 name 的宏，已跳过");
            continue;
        }
        if (item->AttributeBool(L"Ctrl", false)) {
            macro.modifiers |= kModCtrl;
        }
        if (item->AttributeBool(L"Alt", false)) {
            macro.modifiers |= kModAlt;
        }
        if (item->AttributeBool(L"Shift", false)) {
            macro.modifiers |= kModShift;
        }
        macro.keyCode = static_cast<uint16_t>(item->AttributeInt(L"Key", 0));

        for (const infra::XmlElement* step : item->Children(L"Action")) {
            const int type = step->AttributeInt(L"type", 0);
            MacroAction action;
            if (type == static_cast<int>(MacroAction::Kind::Command)) {
                action.kind = MacroAction::Kind::Command;
                intptr_t id = 0;
                ParseIntAttrib(*step, L"message", 0, id);
                action.commandId = static_cast<CmdId>(id);
                action.commandName = step->AttributeOr(L"name", L"");
                if (action.commandId == 0 && !action.commandName.empty() && commandKeyToId) {
                    action.commandId = commandKeyToId(action.commandName);
                }
                if (action.commandId == 0) {
                    warnings.push_back(macro.name + L"：有一条命令步骤既没有可用 ID 也没有命令键，已跳过");
                    continue;
                }
            } else if (type == static_cast<int>(MacroAction::Kind::ScintillaMessage)) {
                action.kind = MacroAction::Kind::ScintillaMessage;
                intptr_t value = 0;
                ParseIntAttrib(*step, L"message", 0, value);
                action.message = static_cast<uint32_t>(value);
                ParseIntAttrib(*step, L"wParam", 0, action.wParam);
                ParseIntAttrib(*step, L"lParam", 0, action.lParam);
                if (action.message == 0) {
                    warnings.push_back(macro.name + L"：有一条消息步骤没写 message，已跳过");
                    continue;
                }
            } else if (type == static_cast<int>(MacroAction::Kind::InsertText)) {
                action.kind = MacroAction::Kind::InsertText;
                action.text = step->AttributeOr(L"sParam", L"");
                if (action.text.empty()) {
                    warnings.push_back(macro.name + L"：有一条插入文本的步骤是空的，已跳过");
                    continue;
                }
            } else {
                // 其它编辑器还可能定义查找替换、书签等动作类型。
                // 认不出的整条跳过并留一句，免得回放出"看起来做了什么但结果不对"。
                warnings.push_back(macro.name + L"：认不出的步骤类型 " + std::to_wstring(type) +
                                   L"，已跳过");
                continue;
            }
            macro.actions.push_back(std::move(action));
        }
        if (macro.Empty()) {
            warnings.push_back(macro.name + L"：一条步骤都没剩下，这个宏已跳过");
            continue;
        }
        macros.push_back(std::move(macro));
    }
    return macros;
}

}  // namespace npcc::core
