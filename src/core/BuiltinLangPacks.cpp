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
        Entry(L"cmd.file.remote", L"Remote Sites...", L"M"),
        Entry(L"cmd.file.editCopy", L"Edit as Copy (Read-Only Large File)...", L"E"),
        // ---- 编辑 ----
        Entry(L"cmd.edit.undo", L"Undo", L"U"),
        Entry(L"cmd.edit.redo", L"Redo", L"R"),
        Entry(L"cmd.edit.cut", L"Cut", L"T"),
        Entry(L"cmd.edit.copy", L"Copy", L"C"),
        Entry(L"cmd.edit.copyAsRtf", L"Copy as Rich Text (RTF)", L"R"),
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
        Entry(L"cmd.view.eyeCare", L"Eye Care Mode (Low Blue Light)", L"E"),
        Entry(L"cmd.view.hexToggle", L"Hex View (read-only)"),
        Entry(L"cmd.view.hexGoTo", L"Go to Offset...", L"O"),
        Entry(L"cmd.view.previewBrowser", L"Open Preview in Browser", L"B"),
        Entry(L"cmd.view.markdownPreview", L"Markdown Preview Panel", L"M"),
        Entry(L"cmd.view.jsonTree", L"JSON Tree View", L"J"),
        Entry(L"cmd.view.xmlTree", L"XML Tree View", L"X"),
        Entry(L"cmd.view.gotoLine", L"Go to Line...", L"L"),
        Entry(L"cmd.view.workspaceOpen", L"Open Folder as Workspace...", L"F"),
        Entry(L"cmd.view.workspaceClose", L"Close Workspace", L"C"),
        Entry(L"cmd.view.workspaceRefresh", L"Refresh Workspace", L"R"),
        Entry(L"cmd.view.workspaceOpenSelected", L"Open Selected File", L"O"),
        Entry(L"cmd.view.functionList", L"Function List", L"F"),
        Entry(L"cmd.view.functionJump", L"Jump to Selected Symbol", L"J"),
        Entry(L"cmd.view.fullScreen", L"Full Screen", L"U"),
        Entry(L"cmd.view.gotoMatchBrace", L"Go to Matching Brace", L"B"),
        Entry(L"cmd.view.summary", L"Summary...", L"S"),
        Entry(L"cmd.view.toolbar", L"Toolbar", L"T"),
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
        Entry(L"cmd.tool.qpEncode", L"Quoted-Printable Encode"),
        Entry(L"cmd.tool.qpDecode", L"Quoted-Printable Decode"),
        Entry(L"cmd.tool.htmlEscape", L"Escape HTML Entities"),
        Entry(L"cmd.tool.htmlUnescape", L"Unescape HTML Entities"),
        // ---- 工具：注释与结构化数据 ----
        Entry(L"cmd.tool.toggleComment", L"Toggle Comment", L"Q"),
        Entry(L"cmd.tool.blockComment", L"Toggle Block Comment", L"B"),
        Entry(L"cmd.tool.styleConfigurator", L"Style Configurator...", L"S"),
        Entry(L"cmd.tool.formatJson", L"Format JSON", L"J"),
        Entry(L"cmd.tool.minifyJson", L"Minify JSON", L"M"),
        Entry(L"cmd.tool.formatXml", L"Pretty-print XML", L"X"),
        Entry(L"cmd.tool.validateXml", L"Validate XML", L"V"),
        Entry(L"cmd.tool.xpathQuery", L"XPath Query...", L"P"),
        // ---- 工具：导出到配置目录 ----
        Entry(L"cmd.tool.exportLanguages", L"Export Language Definitions to Config Folder"),
        Entry(L"cmd.tool.exportUdlSample", L"Export UDL Sample to Config Folder"),
        Entry(L"cmd.tool.exportLangPack", L"Export Language Pack Template (English)"),
        Entry(L"cmd.tool.compareDocument", L"Compare with Another Open Document...", L"C"),
        Entry(L"cmd.tool.compareDisk", L"Compare with Version on Disk", L"D"),
        // ---- 工具：脚本插件（M3 第十二块）----
        Entry(L"cmd.tool.scriptOpenDir", L"Open Scripts Folder", L"S"),
        Entry(L"cmd.tool.scriptRefresh", L"Refresh Script List", L"R"),
        Entry(L"cmd.tool.scriptRun", L"Run Script...", L"U"),
        Entry(L"cmd.tool.scriptPrefix", L"Script: "),
        Entry(L"compare.pick.title", L"Pick a Document to Compare"),
        Entry(L"compare.pick.prompt", L"Current document: "),
        Entry(L"compare.pick.promptTail", L"  --  compare with which one?"),
        Entry(L"compare.pick.ok", L"Compare(&C)"),
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
        Entry(L"cmd.settings.toolbar", L"Customize Toolbar...", L"T"),
        Entry(L"cmd.settings.contextMenu", L"Customize Context Menu...", L"M"),
        // 界面语言那几条命令的文本就是语言自己的名字（"English"/"简体中文"），
        // 故意不翻译：语言名要用它自己的语言写，用户才找得到
        // ---- 帮助 ----
        Entry(L"cmd.help.about", L"About notepadcc", L"A"),
        Entry(L"cmd.help.checkUpdate", L"Check for Updates...", L"U"),
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
        // ---- 首选项对话框（prefs.*，第十八块之后新加的文案都从这里接）----
        Entry(L"prefs.title", L"Preferences"),
        Entry(L"prefs.page.general", L"General"),
        Entry(L"prefs.page.edit", L"Editing"),
        Entry(L"prefs.page.newFile", L"New File"),
        Entry(L"prefs.page.advanced", L"Advanced"),
        Entry(L"prefs.ok", L"OK"),
        Entry(L"prefs.cancel", L"Cancel"),
        Entry(L"prefs.uiLanguage", L"Interface language:"),
        Entry(L"prefs.restoreSession", L"Restore the last session on startup"),
        Entry(L"prefs.multiInstance",
              L"Allow multiple instances (off by default: a new launch hands the file to the "
              L"running instance)"),
        Entry(L"prefs.recentCount", L"Recent files to keep (0-100):"),
        Entry(L"prefs.tabWidth", L"Tab width (1-64):"),
        Entry(L"prefs.tabToSpaces", L"Replace tabs with spaces"),
        Entry(L"prefs.autoIndent", L"Auto-indent"),
        Entry(L"prefs.smartIndent", L"Smart indent (follow the previous line)"),
        Entry(L"prefs.showWhitespace", L"Show whitespace"),
        Entry(L"prefs.showEol", L"Show end-of-line"),
        Entry(L"prefs.showLineNumbers", L"Show line numbers"),
        Entry(L"prefs.indentGuide", L"Show indent guides"),
        Entry(L"prefs.caretLine", L"Highlight the current line"),
        Entry(L"prefs.wrap", L"Word wrap"),
        Entry(L"prefs.zoom", L"Zoom level (-20 to 40):"),
        Entry(L"prefs.defaultEncoding", L"Default encoding:"),
        Entry(L"prefs.enc.utf8Bom", L"UTF-8 (with BOM)"),
        Entry(L"prefs.enc.ansi", L"ANSI (code page)"),
        Entry(L"prefs.defaultCodePage", L"Default code page (ANSI only):"),
        Entry(L"prefs.defaultEol", L"Default line ending:"),
        Entry(L"prefs.eol.crlf", L"CRLF (Windows)"),
        Entry(L"prefs.eol.lf", L"LF (Unix)"),
        Entry(L"prefs.eol.cr", L"CR (Mac)"),
        Entry(L"prefs.backupEnabled", L"Enable crash-recovery snapshots"),
        Entry(L"prefs.backupInterval", L"Snapshot interval (seconds, 5-3600):"),
        Entry(L"prefs.backupKeep", L"Snapshots to keep (1-100):"),
        Entry(L"prefs.convertBackup", L"Keep a .bak copy before converting the encoding"),
        Entry(L"prefs.tier1", L"Large-file threshold (MB, above this use mapped loading):"),
        Entry(L"prefs.tier2", L"Read-only threshold (MB, above this open read-only):"),
        Entry(L"prefs.refuse", L"Refuse threshold (MB, above this do not open):"),
        Entry(L"prefs.updateCheck", L"Check for updates on startup"),
        Entry(L"prefs.updateUrl", L"Update source URL (https required):"),
        Entry(L"prefs.advancedNote",
              L"Note: the three thresholds must increase strictly and take effect right after "
              L"saving. Unsaved content is written periodically to the backup folder under the "
              L"config directory; documents above the large-file threshold are not snapshotted "
              L"on a timer (they are still rescued on a crash), and an abnormal exit also leaves "
              L"a crash report and a minidump."),
        Entry(L"prefs.field.recentCount", L"Recent files to keep"),
        Entry(L"prefs.field.tabWidth", L"Tab width"),
        Entry(L"prefs.field.zoom", L"Zoom level"),
        Entry(L"prefs.field.codePage", L"Default code page"),
        Entry(L"prefs.field.backupInterval", L"Snapshot interval"),
        Entry(L"prefs.field.backupKeep", L"Snapshots to keep"),
        Entry(L"prefs.field.tier1", L"Large-file threshold"),
        Entry(L"prefs.field.tier2", L"Read-only threshold"),
        Entry(L"prefs.field.refuse", L"Refuse threshold"),
        Entry(L"prefs.error.range",
              L"\"{0}\" must be a whole number between {1} and {2}; the current input is "
              L"\"{3}\"."),
        Entry(L"prefs.error.tiers",
              L"The large-file thresholds must increase strictly: large-file < read-only < "
              L"refuse."),
        Entry(L"prefs.error.updateUrl",
              L"Update source is not allowed: {0} (https:// required; plain http://127.0.0.1 "
              L"is for local testing only)"),
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
