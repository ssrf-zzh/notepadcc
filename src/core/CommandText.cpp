#include "core/CommandText.h"

#include <windows.h>

namespace npcc::core {
namespace {

struct CategoryEntry {
    CmdCategory     category;
    const wchar_t*  text;
    wchar_t         mnemonic;
};

// 唯一的类别名表：菜单顺序与快捷键管理界面的分组顺序都取这张表。
const CategoryEntry kCategories[] = {
    {CmdCategory::File,     L"文件",     L'F'},
    {CmdCategory::Edit,     L"编辑",     L'E'},
    {CmdCategory::Search,   L"搜索",     L'S'},
    {CmdCategory::View,     L"视图",     L'V'},
    {CmdCategory::Format,   L"格式",     L'O'},
    {CmdCategory::Language, L"语言",     L'L'},
    {CmdCategory::Settings, L"设置",     L'T'},
    {CmdCategory::Macro,    L"宏",       L'M'},
    {CmdCategory::Run,      L"运行",     L'R'},
    {CmdCategory::Plugin,   L"插件",     L'P'},
    {CmdCategory::Window,   L"窗口",     L'W'},
    {CmdCategory::Help,     L"帮助",     L'H'},
};

const CategoryEntry* FindCategory(CmdCategory category) {
    for (const CategoryEntry& entry : kCategories) {
        if (entry.category == category) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

std::wstring StripAccelerator(std::wstring_view text) {
    std::wstring_view body = text;
    // 结尾的省略号（表示"会弹对话框"）先去掉
    while (!body.empty() && (body.back() == L'.' || body.back() == L' ')) {
        body.remove_suffix(1);
    }
    // 再见掉结尾的 "(&X)"。只认括号里紧跟 '&' 的形式，
    // 免得把 "UTF-8（带 BOM）" 这种正常括号也当成助记符砍掉。
    if (!body.empty() && body.back() == L')') {
        const std::size_t open = body.rfind(L'(');
        if (open != std::wstring_view::npos && open + 1 < body.size() &&
            body[open + 1] == L'&') {
            body = body.substr(0, open);
            while (!body.empty() && body.back() == L' ') {
                body.remove_suffix(1);
            }
        }
    }
    std::wstring out;
    out.reserve(body.size());
    for (const wchar_t ch : body) {
        if (ch != L'&') {
            out.push_back(ch);
        }
    }
    return out;
}

std::wstring CategoryText(CmdCategory category) {
    const CategoryEntry* entry = FindCategory(category);
    return entry != nullptr ? std::wstring(entry->text) : std::wstring(L"其他");
}

wchar_t CategoryMnemonic(CmdCategory category) {
    const CategoryEntry* entry = FindCategory(category);
    return entry != nullptr ? entry->mnemonic : L'\0';
}

std::wstring ShortcutText(uint8_t modifiers, uint16_t keyCode) {
    if (keyCode == 0) {
        return {};
    }
    std::wstring text;
    if ((modifiers & kModCtrl) != 0) {
        text += L"Ctrl+";
    }
    if ((modifiers & kModAlt) != 0) {
        text += L"Alt+";
    }
    if ((modifiers & kModShift) != 0) {
        text += L"Shift+";
    }
    wchar_t name[64] = {};
    const UINT scanCode = ::MapVirtualKeyW(keyCode, MAPVK_VK_TO_VSC);
    if (::GetKeyNameTextW(static_cast<LONG>(scanCode << 16), name, 64) > 0) {
        text += name;
    } else {
        // 个别虚拟键没有可读名字，退回 VK 号，至少不显示成空
        text += L"VK" + std::to_wstring(keyCode);
    }
    return text;
}

}  // namespace npcc::core
