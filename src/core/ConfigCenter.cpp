#include "core/ConfigCenter.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/AtomicFile.h"
#include "io/XmlFile.h"

namespace npcc::core {
namespace {

constexpr const wchar_t* kRootName = L"NotepadccConfig";
constexpr int            kSchemaVersion = 1;
constexpr long long      kSaveMergeWindowMs = 1000;

const infra::XmlElement* Section(const infra::XmlElement* root, const wchar_t* name) {
    return root != nullptr ? root->Child(name) : nullptr;
}

bool ReadBool(const infra::XmlElement* node, const wchar_t* name, bool fallback,
              std::vector<std::wstring>& warnings, const wchar_t* context) {
    if (node == nullptr) {
        return fallback;
    }
    const std::wstring* raw = node->Attribute(name);
    if (raw == nullptr) {
        return fallback;
    }
    if (*raw == L"yes" || *raw == L"true" || *raw == L"1") {
        return true;
    }
    if (*raw == L"no" || *raw == L"false" || *raw == L"0") {
        return false;
    }
    warnings.push_back(std::wstring(context) + L"." + name + L"=\"" + *raw +
                       L"\" 不是合法布尔值，已回退默认值");
    return fallback;
}

int ReadIntInRange(const infra::XmlElement* node, const wchar_t* name, int fallback, int low,
                   int high, std::vector<std::wstring>& warnings, const wchar_t* context) {
    if (node == nullptr) {
        return fallback;
    }
    const std::wstring* raw = node->Attribute(name);
    if (raw == nullptr) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(raw->c_str(), &end, 10);
    if (end == nullptr || *end != L'\0' || parsed < low || parsed > high) {
        // 配置坏掉不该让程序起不来，回退默认值并记一笔
        warnings.push_back(std::wstring(context) + L"." + name + L"=\"" + *raw +
                           L"\" 超出允许范围 [" + std::to_wstring(low) + L"," +
                           std::to_wstring(high) + L"]，已回退默认值");
        return fallback;
    }
    return static_cast<int>(parsed);
}

std::wstring ReadString(const infra::XmlElement* node, const wchar_t* name,
                        const std::wstring& fallback) {
    if (node == nullptr) {
        return fallback;
    }
    const std::wstring* raw = node->Attribute(name);
    return raw != nullptr && !raw->empty() ? *raw : fallback;
}

// 编码与行尾的标识统一取自 edit/Encoding：config、快照、崩溃抢救都写同一套词。
// 这里只做宽窄字符的搬运——标识本身是纯 ASCII。
std::string NarrowAscii(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

std::wstring WidenAscii(const char* text) {
    std::wstring out;
    for (const char* cursor = text; cursor != nullptr && *cursor != '\0'; ++cursor) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
    }
    return out;
}

edit::UniMode ReadUniMode(const infra::XmlElement* node, const wchar_t* name,
                          edit::UniMode fallback, std::vector<std::wstring>& warnings) {
    if (node == nullptr) {
        return fallback;
    }
    const std::wstring text = WidenAscii(edit::UniModeToken(fallback));
    const std::wstring* raw = node->Attribute(name);
    if (raw == nullptr) {
        return fallback;
    }
    edit::UniMode mode = fallback;
    if (!edit::ParseUniModeToken(NarrowAscii(*raw), mode)) {
        warnings.push_back(L"newFile." + std::wstring(name) + L"=\"" + *raw +
                           L"\" 不是已知编码，已回退默认值（" + text + L"）");
        return fallback;
    }
    return mode;
}

edit::EolMode ReadEolMode(const infra::XmlElement* node, const wchar_t* name,
                          edit::EolMode fallback, std::vector<std::wstring>& warnings) {
    if (node == nullptr) {
        return fallback;
    }
    const std::wstring* raw = node->Attribute(name);
    if (raw == nullptr) {
        return fallback;
    }
    edit::EolMode eol = fallback;
    if (!edit::ParseEolToken(NarrowAscii(*raw), eol)) {
        warnings.push_back(L"newFile." + std::wstring(name) + L"=\"" + *raw +
                           L"\" 不是已知行尾，已回退默认值");
        return fallback;
    }
    return eol;
}

}  // namespace

std::filesystem::path ConfigCenter::Path() {
    return infra::ConfigRoot() / L"config.xml";
}

bool ConfigCenter::Load(std::wstring* error) {
    warnings_.clear();

    const std::filesystem::path path = Path();
    const std::wstring pathText = path.wstring();
    if (::GetFileAttributesW(pathText.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 首次运行没有配置文件是常态，用默认值继续
        return true;
    }

    infra::XmlDocument document;
    std::wstring parseError;
    if (!io::LoadXmlFile(pathText, document, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }
    const infra::XmlElement* root = document.Root();
    if (root == nullptr || root->name != kRootName) {
        if (error != nullptr) {
            *error = L"配置文件根元素不是 " + std::wstring(kRootName);
        }
        return false;
    }
    const int version = root->AttributeInt(L"schemaVersion", 0);
    if (version > kSchemaVersion) {
        if (error != nullptr) {
            *error = L"配置文件来自更新的版本（" + std::to_wstring(version) + L"）";
        }
        return false;
    }

    const infra::XmlElement* general = Section(root, L"general");
    general_.uiLanguage = ReadString(general, L"uiLanguage", general_.uiLanguage);
    general_.restoreSessionOnStart =
        ReadBool(general, L"restoreSessionOnStart", general_.restoreSessionOnStart, warnings_,
                 L"general");
    general_.multiInstance = ReadBool(general, L"multiInstance", general_.multiInstance,
                                      warnings_, L"general");
    general_.recentFileCount = static_cast<std::uint32_t>(ReadIntInRange(
        general, L"recentFileCount", static_cast<int>(general_.recentFileCount), 0, 100,
        warnings_, L"general"));
    // 主题名：认不出的名字记一条警告并回浅色（"认不出就用默认值"这条总则）
    general_.theme = ReadString(general, L"theme", general_.theme);
    if (general_.theme != L"light" && general_.theme != L"dark") {
        warnings_.push_back(L"[general] theme 只认 light / dark，实际是 \"" + general_.theme +
                            L"\"，已改回 light");
        general_.theme = L"light";
    }
    // 工作区目录：不在这里校验"存在不存在"——目录可能只是暂时不在（U 盘、网络盘），
    // 启动时打不开就当空工作区，配置保留着，等人把盘接回来
    general_.workspaceFolder = ReadString(general, L"workspaceFolder", general_.workspaceFolder);
    general_.functionListVisible = ReadBool(general, L"functionList", general_.functionListVisible,
                                            warnings_, L"general");
    general_.markdownPreviewVisible =
        ReadBool(general, L"markdownPreview", general_.markdownPreviewVisible, warnings_,
                 L"general");

    const infra::XmlElement* editor = Section(root, L"edit");
    if (editor != nullptr) {
        edit_.tabToSpaces = ReadBool(editor, L"tabToSpaces", edit_.tabToSpaces, warnings_, L"edit");
        edit_.tabWidth = static_cast<std::uint32_t>(ReadIntInRange(
            editor, L"tabWidth", static_cast<int>(edit_.tabWidth), 1, 64, warnings_, L"edit"));
        edit_.autoIndent = ReadBool(editor, L"autoIndent", edit_.autoIndent, warnings_, L"edit");
        edit_.smartIndent = ReadBool(editor, L"smartIndent", edit_.smartIndent, warnings_, L"edit");
        edit_.showWhitespace =
            ReadBool(editor, L"showWhitespace", edit_.showWhitespace, warnings_, L"edit");
        edit_.showEol = ReadBool(editor, L"showEol", edit_.showEol, warnings_, L"edit");
        edit_.showLineNumbers =
            ReadBool(editor, L"showLineNumbers", edit_.showLineNumbers, warnings_, L"edit");
        edit_.showIndentGuide =
            ReadBool(editor, L"showIndentGuide", edit_.showIndentGuide, warnings_, L"edit");
        edit_.caretLineHighlight = ReadBool(editor, L"caretLineHighlight",
                                            edit_.caretLineHighlight, warnings_, L"edit");
        edit_.wrapLongLines =
            ReadBool(editor, L"wrapLongLines", edit_.wrapLongLines, warnings_, L"edit");
        edit_.zoomLevel = static_cast<std::uint32_t>(
            ReadIntInRange(editor, L"zoomLevel", static_cast<int>(edit_.zoomLevel), -20, 40,
                           warnings_, L"edit"));
    }

    const infra::XmlElement* newFile = Section(root, L"newFile");
    if (newFile != nullptr) {
        newFile_.defaultUniMode =
            ReadUniMode(newFile, L"defaultUniMode", newFile_.defaultUniMode, warnings_);
        newFile_.defaultCodePage = static_cast<std::uint32_t>(ReadIntInRange(
            newFile, L"defaultCodePage", static_cast<int>(newFile_.defaultCodePage), 1, 65535,
            warnings_, L"newFile"));
        newFile_.defaultEol =
            ReadEolMode(newFile, L"defaultEol", newFile_.defaultEol, warnings_);
    }

    const infra::XmlElement* backup = Section(root, L"backup");
    if (backup != nullptr) {
        backup_.enabled = ReadBool(backup, L"enabled", backup_.enabled, warnings_, L"backup");
        backup_.intervalSeconds = static_cast<std::uint32_t>(ReadIntInRange(
            backup, L"intervalSeconds", static_cast<int>(backup_.intervalSeconds), 5, 3600,
            warnings_, L"backup"));
        backup_.keepSnapshots = static_cast<std::uint32_t>(
            ReadIntInRange(backup, L"keepSnapshots", static_cast<int>(backup_.keepSnapshots), 1,
                           100, warnings_, L"backup"));
        backup_.keepConvertBackup =
            ReadBool(backup, L"keepConvertBackup", backup_.keepConvertBackup, warnings_, L"backup");
    }

    const infra::XmlElement* large = Section(root, L"largeFile");
    if (large != nullptr) {
        const int tier1Mb = ReadIntInRange(
            large, L"tier1MaxMB",
            static_cast<int>(largeFile_.tier1MaxBytes / (1024 * 1024)), 1, 4096, warnings_,
            L"largeFile");
        const int tier2Mb = ReadIntInRange(
            large, L"tier2MaxMB",
            static_cast<int>(largeFile_.tier2MaxBytes / (1024 * 1024)), 1, 4096, warnings_,
            L"largeFile");
        const int refuseMb = ReadIntInRange(
            large, L"refuseAboveMB",
            static_cast<int>(largeFile_.refuseAboveBytes / (1024 * 1024)), 1, 65536, warnings_,
            L"largeFile");
        if (tier2Mb < tier1Mb) {
            warnings_.push_back(L"largeFile: tier2MaxMB 小于 tier1MaxMB，已回退默认阈值");
        } else if (refuseMb < tier2Mb) {
            warnings_.push_back(L"largeFile: refuseAboveMB 小于 tier2MaxMB，已回退默认阈值");
        } else {
            largeFile_.tier1MaxBytes = static_cast<std::uint64_t>(tier1Mb) * 1024 * 1024;
            largeFile_.tier2MaxBytes = static_cast<std::uint64_t>(tier2Mb) * 1024 * 1024;
            largeFile_.refuseAboveBytes = static_cast<std::uint64_t>(refuseMb) * 1024 * 1024;
        }
    }

    const infra::XmlElement* search = Section(root, L"search");
    if (search != nullptr) {
        search_.matchCase = ReadBool(search, L"matchCase", search_.matchCase, warnings_, L"search");
        search_.wholeWordOnly =
            ReadBool(search, L"wholeWordOnly", search_.wholeWordOnly, warnings_, L"search");
        search_.dotMatchesNewline = ReadBool(search, L"dotMatchesNewline",
                                             search_.dotMatchesNewline, warnings_, L"search");
        search_.wrapAround = ReadBool(search, L"wrapAround", search_.wrapAround, warnings_, L"search");
        search_.lastFilter = ReadString(search, L"lastFilter", search_.lastFilter);
    }

    for (const std::wstring& warning : warnings_) {
        infra::LogWarn(L"config", L"配置项已回退默认值: " + warning);
    }
    return true;
}

bool ConfigCenter::Save(bool force, std::wstring* error) {
    const long long now = static_cast<long long>(::GetTickCount64());
    if (!force && !dirty_) {
        return true;   // 没有改动，不必写
    }
    if (!force && (now - lastSaveTick_) < kSaveMergeWindowMs) {
        // 一秒内的连续改动合并成一次写盘：先记着，等下次请求或 Flush
        dirty_ = true;
        return true;
    }

    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(kRootName);
    root->SetAttributeInt(L"schemaVersion", kSchemaVersion);

    infra::XmlElementPtr general = root->AddChild(L"general");
    general->SetAttribute(L"uiLanguage", general_.uiLanguage);
    general->SetAttributeBool(L"restoreSessionOnStart", general_.restoreSessionOnStart);
    general->SetAttributeBool(L"multiInstance", general_.multiInstance);
    general->SetAttributeInt(L"recentFileCount", static_cast<int>(general_.recentFileCount));
    general->SetAttribute(L"theme", general_.theme);
    general->SetAttribute(L"workspaceFolder", general_.workspaceFolder);
    general->SetAttributeBool(L"functionList", general_.functionListVisible);
    general->SetAttributeBool(L"markdownPreview", general_.markdownPreviewVisible);

    infra::XmlElementPtr editor = root->AddChild(L"edit");
    editor->SetAttributeBool(L"tabToSpaces", edit_.tabToSpaces);
    editor->SetAttributeInt(L"tabWidth", static_cast<int>(edit_.tabWidth));
    editor->SetAttributeBool(L"autoIndent", edit_.autoIndent);
    editor->SetAttributeBool(L"smartIndent", edit_.smartIndent);
    editor->SetAttributeBool(L"showWhitespace", edit_.showWhitespace);
    editor->SetAttributeBool(L"showEol", edit_.showEol);
    editor->SetAttributeBool(L"showLineNumbers", edit_.showLineNumbers);
    editor->SetAttributeBool(L"showIndentGuide", edit_.showIndentGuide);
    editor->SetAttributeBool(L"caretLineHighlight", edit_.caretLineHighlight);
    editor->SetAttributeBool(L"wrapLongLines", edit_.wrapLongLines);
    editor->SetAttributeInt(L"zoomLevel", static_cast<int>(edit_.zoomLevel));

    infra::XmlElementPtr newFile = root->AddChild(L"newFile");
    newFile->SetAttribute(L"defaultUniMode", WidenAscii(edit::UniModeToken(newFile_.defaultUniMode)));
    newFile->SetAttributeInt(L"defaultCodePage", static_cast<int>(newFile_.defaultCodePage));
    newFile->SetAttribute(L"defaultEol", WidenAscii(edit::EolToken(newFile_.defaultEol)));

    infra::XmlElementPtr backup = root->AddChild(L"backup");
    backup->SetAttributeBool(L"enabled", backup_.enabled);
    backup->SetAttributeInt(L"intervalSeconds", static_cast<int>(backup_.intervalSeconds));
    backup->SetAttributeInt(L"keepSnapshots", static_cast<int>(backup_.keepSnapshots));
    backup->SetAttributeBool(L"keepConvertBackup", backup_.keepConvertBackup);

    infra::XmlElementPtr large = root->AddChild(L"largeFile");
    large->SetAttributeInt(L"tier1MaxMB",
                           static_cast<int>(largeFile_.tier1MaxBytes / (1024 * 1024)));
    large->SetAttributeInt(L"tier2MaxMB",
                           static_cast<int>(largeFile_.tier2MaxBytes / (1024 * 1024)));
    large->SetAttributeInt(L"refuseAboveMB",
                           static_cast<int>(largeFile_.refuseAboveBytes / (1024 * 1024)));

    infra::XmlElementPtr search = root->AddChild(L"search");
    search->SetAttributeBool(L"matchCase", search_.matchCase);
    search->SetAttributeBool(L"wholeWordOnly", search_.wholeWordOnly);
    search->SetAttributeBool(L"dotMatchesNewline", search_.dotMatchesNewline);
    search->SetAttributeBool(L"wrapAround", search_.wrapAround);
    search->SetAttribute(L"lastFilter", search_.lastFilter);

    const bool ok = io::SaveXmlFile(Path().wstring(), document, error);
    if (ok) {
        dirty_ = false;
        lastSaveTick_ = now;
    }
    return ok;
}

void ConfigCenter::RequestSave() {
    dirty_ = true;
    Save(false, nullptr);
}

void ConfigCenter::Flush() {
    if (dirty_) {
        Save(true, nullptr);
    }
}

void ConfigCenter::SaveNow() {
    dirty_ = true;
    Save(true, nullptr);
}

}  // namespace npcc::core
