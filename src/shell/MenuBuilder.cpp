#include "shell/MenuBuilder.h"

#include <string>

#include "core/CommandText.h"

namespace npcc::shell {
namespace {

// 只保留"菜单里有哪些类别、按什么顺序排"；类别名与助记符字母
// 统一由 core::CategoryText / CategoryMnemonic 提供，避免两处各写一份慢慢长歪。
const core::CmdCategory kMenuOrder[] = {
    core::CmdCategory::File,
    core::CmdCategory::Edit,
    core::CmdCategory::Search,
    core::CmdCategory::View,
    core::CmdCategory::Format,
    core::CmdCategory::Encoding,
    core::CmdCategory::Tools,
    core::CmdCategory::Settings,
    core::CmdCategory::Window,
    core::CmdCategory::Macro,
    core::CmdCategory::Run,
    core::CmdCategory::Plugin,
    core::CmdCategory::Help,
};

std::wstring ShortcutHint(const core::CommandRegistry& registry, core::CmdId id) {
    for (const auto& binding : registry.AllBindings()) {
        if (binding.cmdId != id) {
            continue;
        }
        return core::ShortcutText(binding.modifiers, binding.keyCode);
    }
    return {};
}

}  // namespace

HMENU BuildMainMenu(const core::CommandRegistry& registry) {
    const HMENU mainMenu = ::CreateMenu();
    if (mainMenu == nullptr) {
        return nullptr;
    }

    for (const core::CmdCategory category : kMenuOrder) {
        const std::vector<core::CmdId> ids = registry.ByCategory(category);
        if (ids.empty()) {
            continue;
        }
        std::wstring title = core::CategoryText(category);
        const wchar_t mnemonic = core::CategoryMnemonic(category);
        if (mnemonic != L'\0') {
            title += L"(&";
            title += mnemonic;
            title += L")";
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
                      reinterpret_cast<UINT_PTR>(popup), title.c_str());
    }
    return mainMenu;
}

}  // namespace npcc::shell
