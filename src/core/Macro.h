#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Command.h"

namespace npcc::infra {
struct XmlElement;
}

namespace npcc::core {

// 一条宏步骤（设计文档 3.5 / 5.7 / 7.3）。
//
// 三种步骤，对应设计文档里 <Action type="..."> 的三种取值：
//   0 命令          —— 走我们自己的命令注册表（行操作、大小写、注释…都在这里）
//   1 Scintilla 消息 —— 只记 (message, wParam, lParam) 三个**数值**参数。这是刻意的：
//                       指针参数没法安全地序列化，也没法跨会话重放，所以录制时只收
//                       "参数全是值"的消息（按词移动、缩进、删除到行尾这类）
//   2 插入文本      —— 用户敲进去的字，按空闲合并成一步（不是一字一步）
//
// **录制的是命令流，不是键鼠事件**（设计文档 2.9 第 1 条）：命令流对窗口位置、焦点、
// DPI 变化都不敏感，跨机器重放的成功率高得多。
struct MacroAction {
    enum class Kind : int {
        Command = 0,
        ScintillaMessage = 1,
        InsertText = 2,
    };

    Kind         kind = Kind::Command;
    CmdId        commandId = 0;      // Command：命令 ID（稳定编号，回放时按它找命令）
    std::wstring commandName;        // Command：命令键，给文件加可读性、ID 找不到时兜底
    uint32_t     message = 0;        // ScintillaMessage：Scintilla 消息号
    intptr_t     wParam = 0;
    intptr_t     lParam = 0;
    std::wstring text;               // InsertText：要插入的文本
};

struct Macro {
    std::wstring name;
    uint8_t      modifiers = kModNone;   // 绑定的快捷键（可以没有）
    uint16_t     keyCode = 0;
    std::vector<MacroAction> actions;

    bool Empty() const noexcept { return actions.empty(); }
};

// 录制器：把"命令执行事件 + 敲进去的字"变成一串步骤。
//
// 文本合并规则（设计文档 5.7 第 4 条）：连续输入按**空闲时间**合并成一步——两次输入
// 间隔不超过 kTextMergeIdleMs 就接着写进同一步；中间来了命令（或停止录制）就先把攒着
// 的文本落定。逐字符存会让一条宏动辄几百步，既难读也难改。
class MacroRecorder {
public:
    static constexpr int64_t kTextMergeIdleMs = 300;

    void Start();
    void Stop();
    bool Recording() const noexcept { return recording_; }

    // 命令执行事件（由 CommandRegistry 的录制钩子调）
    void RecordCommand(const Command& command);
    // 输入了一个字符（外壳在 SCN_CHARADDED 时调）。nowMs 用单调时钟（GetTickCount64）
    void RecordChar(int ch, int64_t nowMs);
    // Scintilla 原生消息（只有参数全是值才记）
    void RecordMessage(uint32_t message, intptr_t wParam, intptr_t lParam);
    // 把攒着的文本落定成一步（命令到来、停止录制、开始回放之前都要调）
    void FlushPendingText();

    const std::vector<MacroAction>& Actions() const noexcept { return actions_; }
    bool Empty() const noexcept { return actions_.empty(); }
    void Reset();

private:
    bool                     recording_ = false;
    std::vector<MacroAction> actions_;
    int64_t                  lastCharMs_ = 0;
};

// 回放宏。三件事由外壳注入（注册表、编辑控件、消息发送都在外壳手里）：
//   * 执行命令：返回是否成功（命令不存在或被禁用都算失败）
//   * 发 Scintilla 消息
//   * 插入文本
struct MacroRunner {
    std::function<bool(CmdId)>               runCommand;
    std::function<void(const MacroAction&)>  sendMessage;
    std::function<void(std::wstring_view)>   insertText;
    // 每一步之前问一次"要不要继续"（用户按了停止就中断）
    std::function<bool()>                    keepGoing;
};

struct MacroRunResult {
    bool         ok = true;         // 是否全程无失败
    int          completed = 0;     // 实际执行的遍数（宽松模式下"跳过失败步骤"也算执行过）
    int          failedStep = -1;   // 出错时：第几步（1 起）；-1 表示没出错
    int          failedRepeat = -1; // 出错时：第几遍（1 起）
    std::wstring message;           // 给人看的说明
};

// 跑 repeatCount 遍；stopOnError=false 时跳过失败步骤继续（设计文档 5.7 第 4 条）
MacroRunResult RunMacro(const Macro& macro, const MacroRunner& runner, int repeatCount,
                        bool stopOnError);

// ---- 持久化（shortcuts.xml 里的 <Macros> 一节）----
// 这两个函数只做"宏 ↔ 那段 XML"的转换；文件读写由 ShortcutStore 负责
// （一份文件只留一个写者，免得两边各写一半互相覆盖）。
void AppendMacrosXml(infra::XmlElement& root, const std::vector<Macro>& macros);
// 认不出的步骤会跳过并记一条警告；commandKeyToId 把命令键换成 ID（ID 优先）
std::vector<Macro> ReadMacrosXml(const infra::XmlElement& root,
                                 const std::function<CmdId(const std::wstring&)>& commandKeyToId,
                                 std::vector<std::wstring>& warnings);

}  // namespace npcc::core
