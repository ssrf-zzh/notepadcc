#include "shell/MenuBuilder.h"

#include <string>

namespace npcc::shell {
namespace {

struct CategoryEntry {
    core::CmdCategory category;
    const wchar_t*    title;
};

const CategoryEntry kMenuOrder[] = {
    {core::CmdCategory::File,   L"文件(&F)"},
    {core::CmdCategory::Edit,   L"编辑(&E)"},
    {core::CmdCategory::Search, L"搜索(&S)"},
    {core::CmdCategory::View,   L"视图(&V)"},
    {core::CmdCategory::Settings, L"设置(&T)"},
    {core::CmdCategory::Window, L"窗口(&W)"},
    {core::CmdCategory::Macro,  L"宏(&M)"},
    {core::CmdCategory::Run,    L"运行(&R)"},
    {core::CmdCategory::Plugin, L"插件(&P)"},
    {core::CmdCategory::Help,   L"帮助(&H)"},
};

std::wstring ShortcutHint(const core::CommandRegistry& registry, core::CmdId id) {
    for (const auto& binding : registry.AllBindings()) {
        if (binding.cmdId != id) {
            continue;
        }
        std::wstring text;
        if ((binding.modifiers & core::kModCtrl) != 0) {
            text += L"Ctrl+";
        }
        if ((binding.modifiers & core::kModAlt) != 0) {
            text += L"Alt+";
        }
        if ((binding.modifiers & core::kModShift) != 0) {
            text += L"Shift+";
        }
        wchar_t name[64] = {};
        const UINT scanCode = ::MapVirtualKeyW(binding.keyCode, MAPVK_VK_TO_VSC);
        if (::GetKeyNameTextW(static_cast<LONG>(scanCode << 16), name, 64) > 0) {
            text += name;
        }
        return text;
    }
    return {};
}

}  // namespace

HMENU BuildMainMenu(const core::CommandRegistry& registry) {
    const HMENU mainMenu = ::CreateMenu();
    if (mainMenu == nullptr) {
        return nullptr;
    }

    for (const auto& entry : kMenuOrder) {
        const std::vector<core::CmdId> ids = registry.ByCategory(entry.category);
        if (ids.empty()) {
            continue;
        }
        const HMENU popup = ::CreatePopupMenu();
        if (popup == nullptr) {
            continue;
        }

        UINT position = 0;
        for (const core::CmdId id : ids) {
            const core::Command* command = registry.Find(id);
            if (command == nullptr || (command->flags & core::kCmdHidden) != 0) {
                continue;
            }
            std::wstring label = command->text;
            // 分隔约定：文本以 "-" 开头表示插入一条分隔线
            if (label == L"-") {
                ::InsertMenuW(popup, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
                continue;
            }
            const std::wstring hint = ShortcutHint(registry, id);
            if (!hint.empty()) {
                label += L"\t" + hint;
            }
            const UINT flags = MF_BYPOSITION | MF_STRING |
                               ((command->flags & core::kCmdCheckable) != 0 ? MF_UNCHECKED : 0);
            ::InsertMenuW(popup, position++, flags, static_cast<UINT_PTR>(command->id), label.c_str());
        }

        ::InsertMenuW(mainMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_POPUP,
                      reinterpret_cast<UINT_PTR>(popup), entry.title);
    }
    return mainMenu;
}

}  // namespace npcc::shell
