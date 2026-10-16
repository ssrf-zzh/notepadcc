#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/Ids.h"

namespace npcc::edit {
struct DocBuffer;
class EditView;
}  // namespace npcc::edit

namespace npcc::core {

enum class CmdCategory : uint8_t {
    File = 1,
    Edit,
    Search,
    View,
    Format,
    Language,
    Settings,
    Macro,
    Run,
    Plugin,
    Window,
    Help,
    // 追加在末尾：类别值会被编进命令 ID 的高 4 位，改前面的取值会让旧快捷键失效
    Encoding,
    Tools,
};

enum ShortcutMods : uint8_t {
    kModNone  = 0,
    kModCtrl  = 1u << 0,
    kModAlt   = 1u << 1,
    kModShift = 1u << 2,
};

enum CmdFlags : uint32_t {
    kCmdNone          = 0,
    kCmdCheckable     = 1u << 0,   // 带勾选态（如"显示符号"）
    kCmdNeedsBuffer   = 1u << 1,   // 需要活动缓冲区
    kCmdNeedsSelection= 1u << 2,   // 需要非空选区
    kCmdNeedsWritable = 1u << 3,   // 需要可写缓冲区
    kCmdRecordable    = 1u << 4,   // 可被宏录制
    kCmdHidden        = 1u << 5,   // 不出现在菜单，仅供快捷键/插件调用
    // 用户自己造出来的命令（宏、运行命令）：它的快捷键记在**自己那个 XML 元素**上
    // （<Macro Ctrl=…> / <Command Ctrl=…>），不进 <InternalCommands>。
    // 原因是加载顺序：<InternalCommands> 是按命令键查注册表的，而这些命令要到外壳
    // 注册之后才存在，写在那儿下次启动会认不出来、绑定被丢掉。
    kCmdUserDefined   = 1u << 6,
};

struct CmdContext {
    ::npcc::edit::DocBuffer* buffer = nullptr;
    ::npcc::edit::EditView*  view   = nullptr;
    void*                   userData = nullptr;
};

struct Command {
    CmdId        id       = 0;
    CmdCategory  category = CmdCategory::Edit;
    std::wstring key;                       // 语言包键，如 cmd.file.save
    std::wstring text;                      // 当前显示文本（M0 直接用，M1 接入语言包）
    uint32_t     flags    = kCmdNone;

    std::function<void(const CmdContext&)> execute;
    std::function<bool(const CmdContext&)> queryEnabled;   // 可空
    std::function<bool(const CmdContext&)> queryChecked;   // 可空
};

struct ShortcutBinding {
    CmdId    cmdId     = 0;
    uint8_t  modifiers = kModNone;
    uint16_t keyCode   = 0;
};

enum class ExecResult { Ok, Disabled, NotFound, Failed, Unsupported };

}  // namespace npcc::core
