#include "core/Localization.h"

namespace npcc::core {
namespace {

LangEntry Entry(const wchar_t* key, const wchar_t* value, const wchar_t* accel = L"") {
    LangEntry entry;
    entry.key = key;
    entry.value = value;
    entry.accel = accel;
    return entry;
}

// 英文那一套。**键必须覆盖程序里注册的全部命令**：回退链是"当前语言 → 简体中文 →
// 键本身"，少了哪一条，英文界面上就会冒出中文（或者更糟：一个键名）。
// 这一条不是靠自觉：启动时会逐个命令核对，缺键写进日志；界面回归脚本会拿英文
// 启动一遍，核对日志里没有缺键记录。
std::vector<LangPack> MakeBuiltin() {
    LangPack english;
    english.locale = L"en-US";
    english.name = L"English";
    english.entries = {
        // ---- 文件 ----
        Entry(L"cmd.file.new", L"New", L"N"),
        Entry(L"cmd.file.open", L"Open...", L"O"),
        Entry(L"cmd.file.save", L"Save", L"S"),
        Entry(L"cmd.file.saveAs", L"Save As...", L"A"),
        Entry(L"cmd.file.close", L"Close Current Document", L"C"),
        Entry(L"cmd.file.closeAll", L"Close All Documents", L"L"),
        Entry(L"cmd.file.exit", L"Exit", L"X"),
        Entry(L"cmd.file.print", L"Print...", L"P"),
        Entry(L"cmd.file.exportHtml", L"Export as HTML...", L"H"),
        Entry(L"cmd.file.exportRtf", L"Export as RTF...", L"R"),
        // ---- 编辑 ----
        Entry(L"cmd.edit.undo", L"Undo", L"U"),
        Entry(L"cmd.edit.redo", L"Redo", L"R"),
        Entry(L"cmd.edit.cut", L"Cut", L"T"),
        Entry(L"cmd.edit.copy", L"Copy", L"C"),
        Entry(L"cmd.edit.paste", L"Paste", L"P"),
        Entry(L"cmd.edit.selectAll", L"Select All", L"L"),
        Entry(L"cmd.edit.columnEdit", L"Column Editor...", L"C"),
        Entry(L"cmd.edit.toggleOverwrite", L"Insert / Overwrite Mode", L"V"),
        Entry(L"cmd.edit.autoComplete", L"Auto-Completion", L"A"),
        // ---- 搜索 ----
        Entry(L"cmd.search.find", L"Find...", L"F"),
        Entry(L"cmd.search.replace", L"Replace...", L"H"),
        Entry(L"cmd.search.findNext", L"Find Next", L"N"),
        Entry(L"cmd.search.findPrev", L"Find Previous", L"P"),
        Entry(L"cmd.search.inFiles", L"Find in Files...", L"I"),
        Entry(L"cmd.search.toggleBookmark", L"Toggle Bookmark", L"B"),
        Entry(L"cmd.search.nextBookmark", L"Next Bookmark", L"N"),
        Entry(L"cmd.search.prevBookmark", L"Previous Bookmark", L"P"),
        Entry(L"cmd.search.clearBookmarks", L"Clear All Bookmarks", L"C"),
        Entry(L"cmd.search.replaceInFiles", L"Replace in Files...", L"R"),
        // ---- 视图 ----
        Entry(L"cmd.view.zoomIn", L"Zoom In", L"I"),
        Entry(L"cmd.view.zoomOut", L"Zoom Out", L"O"),
        Entry(L"cmd.view.zoomReset", L"Restore Default Zoom", L"Z"),
        Entry(L"cmd.view.toggleWrap", L"Word Wrap", L"W"),
        Entry(L"cmd.view.toggleSpaces", L"Show Whitespace", L"S"),
        Entry(L"cmd.view.toggleEol", L"Show End of Line", L"E"),
        Entry(L"cmd.view.toggleNumbers", L"Show Line Numbers", L"N"),
        Entry(L"cmd.view.foldToggle", L"Fold / Unfold Current Level", L"F"),
        Entry(L"cmd.view.foldAll", L"Fold All", L"L"),
        Entry(L"cmd.view.unfoldAll", L"Unfold All", L"E"),
        Entry(L"cmd.view.splitToggle", L"Split View (Clone Current Document)", L"P"),
        Entry(L"cmd.view.cyclePane", L"Switch to Other View", L"W"),
        Entry(L"cmd.view.scrollSync", L"Synchronize Scrolling", L"Y"),
        Entry(L"cmd.view.darkMode", L"Dark Mode", L"D"),
        Entry(L"cmd.view.workspaceOpen", L"Open Folder as Workspace...", L"F"),
        Entry(L"cmd.view.workspaceClose", L"Close Workspace", L"C"),
        Entry(L"cmd.view.workspaceRefresh", L"Refresh Workspace", L"R"),
        Entry(L"cmd.view.workspaceOpenSelected", L"Open Selected File", L"O"),
        Entry(L"cmd.view.functionList", L"Function List", L"F"),
        Entry(L"cmd.view.functionJump", L"Jump to Selected Symbol", L"J"),
        // ---- 宏 ----
        Entry(L"cmd.macro.record", L"Start / Stop Recording", L"R"),
        Entry(L"cmd.macro.playLast", L"Play Last Macro", L"P"),
        Entry(L"cmd.macro.runMultiple", L"Run Last Macro Multiple Times...", L"L"),
        Entry(L"cmd.macro.saveLast", L"Save Recorded Macro...", L"S"),
        // ---- 格式 / 编码 ----
        Entry(L"cmd.format.eolCrlf", L"Convert to Windows EOL (CRLF)"),
        Entry(L"cmd.format.eolLf", L"Convert to Unix EOL (LF)"),
        Entry(L"cmd.format.eolCr", L"Convert to Mac EOL (CR)"),
        Entry(L"cmd.encoding.ansi", L"Convert to ANSI (default code page)", L"A"),
        Entry(L"cmd.encoding.utf8", L"Convert to UTF-8", L"U"),
        Entry(L"cmd.encoding.utf8Bom", L"Convert to UTF-8 (with BOM)", L"B"),
        Entry(L"cmd.encoding.utf16Le", L"Convert to UTF-16 LE", L"L"),
        Entry(L"cmd.encoding.utf16Be", L"Convert to UTF-16 BE", L"E"),
        // ---- 工具：行操作 ----
        Entry(L"cmd.tool.duplicateLine", L"Duplicate Current Line", L"D"),
        Entry(L"cmd.tool.deleteLine", L"Delete Current Line", L"L"),
        Entry(L"cmd.tool.moveLineUp", L"Move Line Up", L"U"),
        Entry(L"cmd.tool.moveLineDown", L"Move Line Down", L"N"),
        Entry(L"cmd.tool.sortAsc", L"Sort Ascending", L"A"),
        Entry(L"cmd.tool.sortDesc", L"Sort Descending", L"Z"),
        Entry(L"cmd.tool.sortCaseInsAsc", L"Sort Ascending (case-insensitive)"),
        Entry(L"cmd.tool.sortCaseInsDesc", L"Sort Descending (case-insensitive)"),
        Entry(L"cmd.tool.sortLengthAsc", L"Sort by Length Ascending"),
        Entry(L"cmd.tool.sortLengthDesc", L"Sort by Length Descending"),
        Entry(L"cmd.tool.removeEmpty", L"Delete Empty Lines", L"E"),
        Entry(L"cmd.tool.removeDupFirst", L"Delete Duplicate Lines (keep first)"),
        Entry(L"cmd.tool.removeDupLast", L"Delete Duplicate Lines (keep last)"),
        Entry(L"cmd.tool.reverseLines", L"Reverse Line Order", L"R"),
        Entry(L"cmd.tool.trimTrailing", L"Trim Trailing Whitespace"),
        Entry(L"cmd.tool.trimLeading", L"Trim Leading Whitespace"),
        Entry(L"cmd.tool.addLineNumbers", L"Add Line Numbers"),
        Entry(L"cmd.tool.stripLineNumbers", L"Remove Line Numbers"),
        Entry(L"cmd.tool.upperCase", L"Convert to Uppercase", L"U"),
        Entry(L"cmd.tool.lowerCase", L"Convert to Lowercase", L"O"),
        // ---- 工具：编码与实体 ----
        Entry(L"cmd.tool.urlEncode", L"URL Encode"),
        Entry(L"cmd.tool.urlDecode", L"URL Decode"),
        Entry(L"cmd.tool.base64Encode", L"Base64 Encode"),
        Entry(L"cmd.tool.base64Decode", L"Base64 Decode"),
        Entry(L"cmd.tool.htmlEscape", L"Escape HTML Entities"),
        Entry(L"cmd.tool.htmlUnescape", L"Unescape HTML Entities"),
        // ---- 工具：注释与结构化数据 ----
        Entry(L"cmd.tool.toggleComment", L"Toggle Comment", L"Q"),
        Entry(L"cmd.tool.blockComment", L"Toggle Block Comment", L"B"),
        Entry(L"cmd.tool.styleConfigurator", L"Style Configurator...", L"S"),
        Entry(L"cmd.tool.formatJson", L"Format JSON", L"J"),
        Entry(L"cmd.tool.minifyJson", L"Minify JSON", L"M"),
        Entry(L"cmd.tool.formatXml", L"Pretty-print XML", L"X"),
        // ---- 工具：导出到配置目录 ----
        Entry(L"cmd.tool.exportLanguages", L"Export Language Definitions to Config Folder"),
        Entry(L"cmd.tool.exportUdlSample", L"Export UDL Sample to Config Folder"),
        Entry(L"cmd.tool.exportLangPack", L"Export Language Pack Template (English)"),
        // ---- 窗口 / 运行 ----
        Entry(L"cmd.window.next", L"Next Tab", L"N"),
        Entry(L"cmd.window.prev", L"Previous Tab", L"P"),
        Entry(L"cmd.run.dialog", L"Run...", L"R"),
        // ---- 设置 ----
        Entry(L"cmd.settings.preferences", L"Preferences...", L"P"),
        Entry(L"cmd.settings.shortcuts", L"Manage Shortcuts...", L"K"),
        Entry(L"cmd.settings.registerAssoc", L"Add to 'Open with' List", L"A"),
        Entry(L"cmd.settings.unregisterAssoc", L"Remove from 'Open with' List", L"R"),
        Entry(L"cmd.settings.plugins", L"Plugin Manager...", L"G"),
        // 界面语言那几条命令的文本就是语言自己的名字（"English"/"简体中文"），
        // 故意不翻译：语言名要用它自己的语言写，用户才找得到
        // ---- 帮助 ----
        Entry(L"cmd.help.about", L"About notepadcc", L"A"),
        // ---- 菜单类别 ----
        Entry(L"cat.file", L"File"),
        Entry(L"cat.edit", L"Edit"),
        Entry(L"cat.search", L"Search"),
        Entry(L"cat.view", L"View"),
        Entry(L"cat.format", L"Format"),
        Entry(L"cat.language", L"Language"),
        Entry(L"cat.settings", L"Settings"),
        Entry(L"cat.macro", L"Macro"),
        Entry(L"cat.run", L"Run"),
        Entry(L"cat.plugin", L"Plugins"),
        Entry(L"cat.window", L"Window"),
        Entry(L"cat.help", L"Help"),
        Entry(L"cat.encoding", L"Encoding"),
        Entry(L"cat.tools", L"Tools"),
    };
    std::vector<LangPack> packs;
    packs.push_back(std::move(english));
    return packs;
}

}  // namespace

const std::vector<LangPack>& BuiltinLangPacks() {
    static const std::vector<LangPack> packs = MakeBuiltin();
    return packs;
}

}  // namespace npcc::core
