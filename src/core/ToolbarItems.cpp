#include "core/ToolbarItems.h"

#include <algorithm>

namespace npcc::core {

const ToolbarItems& DefaultToolbarItems() {
    // 默认这套是"天天看得见"的那些：文件三件套、撤销重做、查找、书签、注释、
    // 折叠、暗色与摘要。放不下的都不放——工具栏是给人一眼扫的，不是菜单的镜像
    static const ToolbarItems items = {
        L"cmd.file.new",
        L"cmd.file.open",
        L"cmd.file.save",
        kToolbarSeparator,
        L"cmd.edit.undo",
        L"cmd.edit.redo",
        kToolbarSeparator,
        L"cmd.search.find",
        L"cmd.search.findNext",
        kToolbarSeparator,
        L"cmd.search.toggleBookmark",
        L"cmd.search.nextBookmark",
        kToolbarSeparator,
        L"cmd.tool.toggleComment",
        kToolbarSeparator,
        L"cmd.view.foldAll",
        L"cmd.view.unfoldAll",
        kToolbarSeparator,
        L"cmd.view.darkMode",
        L"cmd.view.summary",
    };
    return items;
}

ToolbarItems NormalizeToolbarItems(const ToolbarItems& requested,
                                   const std::function<bool(const std::wstring&)>& known) {
    ToolbarItems out;
    out.reserve(requested.size());
    for (const std::wstring& raw : requested) {
        if (raw.empty()) {
            continue;
        }
        if (raw == kToolbarSeparator) {
            // 开头的与连排的分隔符直接吃掉：一条竖线画在边缘或跟前一条挨着，
            // 看起来都像渲染出错，而不是"这里本来有个分隔"
            if (out.empty() || out.back() == kToolbarSeparator) {
                continue;
            }
            out.push_back(raw);
            continue;
        }
        if (!known || !known(raw)) {
            continue;   // 键不存在（插件没装、宏被删了）：静默丢掉，日志由调用方记
        }
        if (std::find(out.begin(), out.end(), raw) != out.end()) {
            continue;   // 同一个按钮放两次没有意义
        }
        out.push_back(raw);
    }
    while (!out.empty() && out.back() == kToolbarSeparator) {
        out.pop_back();
    }
    return out;
}

}  // namespace npcc::core
