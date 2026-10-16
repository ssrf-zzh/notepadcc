#include "core/ShortcutStore.h"

#include <windows.h>

#include <algorithm>

#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/XmlFile.h"

namespace npcc::core {
namespace {

constexpr const wchar_t* kRootName = L"NotepadPlusCompat";
constexpr int            kSchemaVersion = 1;
constexpr uint8_t        kKnownModifiers = kModCtrl | kModAlt | kModShift;

constexpr const wchar_t* kShortcutElement = L"Shortcut";
constexpr const wchar_t* kInternalSection = L"InternalCommands";

// 三个修饰键属性分开写是为了跟对标产品的文件格式对齐，读的时候拼回来。
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

void WriteModifiers(infra::XmlElement& node, uint8_t modifiers) {
    node.SetAttributeBool(L"Ctrl", (modifiers & kModCtrl) != 0);
    node.SetAttributeBool(L"Alt", (modifiers & kModAlt) != 0);
    node.SetAttributeBool(L"Shift", (modifiers & kModShift) != 0);
}

const Command* ResolveCommand(const CommandRegistry& registry, const infra::XmlElement& node,
                              std::vector<std::wstring>& warnings) {
    // 先按 name 找，再退回 id。name 是稳定的命令键（cmd.file.save），
    // 不会随命令编号调整而改变，所以它是主键。
    const std::wstring* name = node.Attribute(L"name");
    if (name != nullptr && !name->empty()) {
        for (const Command& command : registry.All()) {
            if (command.key == *name) {
                return &command;
            }
        }
        warnings.push_back(L"shortcuts.xml 里的命令 \"" + *name + L"\" 不认识，已跳过");
        return nullptr;
    }
    const int id = node.AttributeInt(L"id", 0);
    if (id != 0) {
        if (const Command* command = registry.Find(static_cast<CmdId>(id)); command != nullptr) {
            return command;
        }
        warnings.push_back(L"shortcuts.xml 里的命令 ID " + std::to_wstring(id) + L" 不认识，已跳过");
        return nullptr;
    }
    warnings.push_back(L"shortcuts.xml 里有一条既没有 name 也没有 id 的记录，已跳过");
    return nullptr;
}

}  // namespace

std::filesystem::path ShortcutStore::Path() {
    return infra::ConfigRoot() / L"shortcuts.xml";
}

bool IsBindableShortcut(uint8_t modifiers, uint16_t keyCode) {
    if (keyCode == 0) {
        return false;
    }
    switch (keyCode) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_CAPITAL:
        case VK_NUMLOCK:
        case VK_SCROLL:
        case VK_APPS:
        case VK_PAUSE:
            return false;   // 修饰键自己不能当快捷键
        default:
            break;
    }
    if ((modifiers & (kModCtrl | kModAlt)) != 0) {
        return true;
    }
    if (keyCode >= VK_F1 && keyCode <= VK_F24) {
        return true;
    }
    switch (keyCode) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_SNAPSHOT:
            return true;
        default:
            // 剩下的（字母、数字、方向键、Home/End/PgUp/PgDn、Tab、Enter、Esc）
            // 一律不放行：它们要么是打字要用，要么是界面导航要用。
            return false;
    }
}

bool ShortcutStore::Load(CommandRegistry& registry, std::wstring* error,
                         std::vector<std::wstring>* warnings) {
    std::vector<std::wstring> localWarnings;
    std::vector<std::wstring>& sink = warnings != nullptr ? *warnings : localWarnings;

    const std::filesystem::path path = Path();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;   // 没有文件 = 全默认，不算失败
    }

    infra::XmlDocument document;
    std::wstring parseError;
    if (!io::LoadXmlFile(path.wstring(), document, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }
    const infra::XmlElement* root = document.Root();
    if (root == nullptr || root->name != kRootName) {
        if (error != nullptr) {
            *error = L"shortcuts.xml 的根元素不是 " + std::wstring(kRootName);
        }
        return false;
    }
    if (root->AttributeInt(L"schemaVersion", 0) > kSchemaVersion) {
        if (error != nullptr) {
            *error = L"shortcuts.xml 来自更新的版本，已忽略";
        }
        return false;
    }
    // 宏那一节与快捷键无关，但它就住在这个文件里，所以顺手读进来（保存时要写回去）
    macros_ = ReadMacrosXml(
        *root,
        [&registry](const std::wstring& key) -> CmdId {
            for (const Command& command : registry.All()) {
                if (command.key == key) {
                    return command.id;
                }
            }
            return 0;
        },
        sink);

    const infra::XmlElement* section = root->Child(kInternalSection);
    if (section == nullptr) {
        for (const std::wstring& warning : sink) {
            infra::LogWarn(L"shortcut", L"快捷键配置: " + warning);
        }
        return true;
    }

    std::vector<CmdId> replaced;   // 已经用文件内容覆盖过的命令
    for (const infra::XmlElement* node : section->Children(kShortcutElement)) {
        if (node == nullptr) {
            continue;
        }
        const Command* command = ResolveCommand(registry, *node, sink);
        if (command == nullptr) {
            continue;
        }
        const CmdId id = command->id;

        // 同一个命令第一次出现时，先把它的默认绑定整体清掉；
        // 之后同一命令的其它记录都是"追加一条绑定"。
        if (std::find(replaced.begin(), replaced.end(), id) == replaced.end()) {
            replaced.push_back(id);
            registry.ClearShortcutsOf(id);
        }

        const int key = node->AttributeInt(L"Key", 0);
        if (key <= 0 || key > 0xFFFF) {
            continue;   // Key="0"（或非法值）= 这个命令不绑定快捷键
        }

        uint8_t modifiers = ReadModifiers(*node);
        if ((modifiers & ~kKnownModifiers) != 0) {
            sink.push_back(command->key + L"：修饰键里有未知位，已忽略");
            modifiers &= kKnownModifiers;
        }

        const std::vector<CmdId> conflicts =
            registry.FindConflicts(modifiers, static_cast<uint16_t>(key));
        for (const CmdId owner : conflicts) {
            const Command* other = registry.Find(owner);
            sink.push_back(command->key + L"：快捷键与 " +
                           (other != nullptr ? other->key : std::to_wstring(owner)) +
                           L" 冲突，先来的那条已被顶掉");
        }
        registry.BindShortcut(id, modifiers, static_cast<uint16_t>(key), true);
    }

    for (const std::wstring& warning : sink) {
        infra::LogWarn(L"shortcut", L"快捷键配置: " + warning);
    }
    return true;
}

bool ShortcutStore::Save(const CommandRegistry& registry, std::wstring* error) const {
    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(kRootName);
    root->SetAttributeInt(L"schemaVersion", kSchemaVersion);

    infra::XmlElementPtr section = root->AddChild(kInternalSection);
    for (const Command& command : registry.All()) {
        if ((command.flags & kCmdHidden) != 0) {
            continue;   // 隐藏命令不进文件
        }
        const std::vector<ShortcutBinding> bindings = registry.BindingsOf(command.id);
        if (bindings.empty()) {
            // 明确记成"不绑定"，否则下次读的时候默认快捷键会复活
            infra::XmlElementPtr node = section->AddChild(kShortcutElement);
            node->SetAttribute(L"name", command.key);
            node->SetAttributeInt(L"id", static_cast<int>(command.id));
            WriteModifiers(*node, kModNone);
            node->SetAttributeInt(L"Key", 0);
            continue;
        }
        for (const ShortcutBinding& binding : bindings) {
            infra::XmlElementPtr node = section->AddChild(kShortcutElement);
            node->SetAttribute(L"name", command.key);
            node->SetAttributeInt(L"id", static_cast<int>(command.id));
            WriteModifiers(*node, binding.modifiers);
            node->SetAttributeInt(L"Key", static_cast<int>(binding.keyCode));
        }
    }
    // 宏跟着一起写：这份文件只有一个写者，改快捷键这条路径不会把宏抹掉
    AppendMacrosXml(*root, macros_);
    return io::SaveXmlFile(Path().wstring(), document, error);
}

std::size_t ShortcutStore::AddOrReplaceMacro(Macro macro) {
    for (std::size_t index = 0; index < macros_.size(); ++index) {
        if (macros_[index].name == macro.name) {
            macros_[index] = std::move(macro);   // 宏名就是它的标识
            return index;
        }
    }
    macros_.push_back(std::move(macro));
    return macros_.size() - 1;
}

}  // namespace npcc::core
