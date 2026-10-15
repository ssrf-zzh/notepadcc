#include "core/SessionManager.h"

#include <windows.h>

#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/XmlFile.h"

namespace npcc::core {
namespace {

constexpr const wchar_t* kRootName = L"Session";
constexpr std::uint32_t  kSchemaVersion = 1;

}  // namespace

std::wstring SessionManager::SessionPath() {
    return (infra::ConfigRoot() / L"session.xml").wstring();
}

bool SessionManager::Save(const SessionSnapshot& snapshot, std::wstring* error) {
    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(kRootName);
    root->SetAttributeInt(L"schemaVersion", static_cast<int>(kSchemaVersion));

    for (const SessionDocument& entry : snapshot.documents) {
        infra::XmlElementPtr node = root->AddChild(L"Doc");
        node->SetAttribute(L"path", entry.filePath);
        node->SetAttributeInt(L"caret", entry.caretPos);
        node->SetAttributeInt(L"firstVisibleLine", entry.firstVisibleLine);
        node->SetAttributeInt(L"zoom", entry.zoomLevel);
        if (entry.isActive) {
            node->SetAttributeBool(L"active", true);
        }
        if (!entry.bookmarks.empty()) {
            infra::XmlElementPtr marks = node->AddChild(L"Bookmarks");
            for (const std::int32_t line : entry.bookmarks) {
                marks->AddChild(L"Line")->SetAttributeInt(L"n", line);
            }
        }
        if (!entry.folds.empty()) {
            infra::XmlElementPtr folds = node->AddChild(L"Folds");
            for (const std::int32_t line : entry.folds) {
                folds->AddChild(L"Line")->SetAttributeInt(L"n", line);
            }
        }
    }

    return io::SaveXmlFile(SessionPath(), document, error);
}

bool SessionManager::Load(SessionSnapshot& snapshot, std::wstring* error) {
    const std::wstring path = SessionPath();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error != nullptr) {
            *error = L"会话文件尚不存在";
        }
        return false;
    }

    infra::XmlDocument document;
    std::wstring parseError;
    if (!io::LoadXmlFile(path, document, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }

    const infra::XmlElement* root = document.Root();
    if (root == nullptr || root->name != kRootName) {
        if (error != nullptr) {
            *error = L"会话文件根元素不是 Session";
        }
        return false;
    }

    const int version = root->AttributeInt(L"schemaVersion", 0);
    if (version > static_cast<int>(kSchemaVersion)) {
        // 会话文件来自更新版本：宁可不用，也不要按错误的结构猜测
        if (error != nullptr) {
            *error = L"会话文件来自更新的版本（" + std::to_wstring(version) + L"）";
        }
        return false;
    }

    SessionSnapshot parsed;
    parsed.schemaVersion = static_cast<std::uint32_t>(version > 0 ? version : 1);
    for (const infra::XmlElement* node : root->Children(L"Doc")) {
        SessionDocument entry;
        entry.filePath = node->AttributeOr(L"path", L"");
        if (entry.filePath.empty()) {
            continue;
        }
        entry.caretPos = node->AttributeInt(L"caret", 0);
        entry.firstVisibleLine = node->AttributeInt(L"firstVisibleLine", 0);
        entry.zoomLevel = node->AttributeInt(L"zoom", 0);
        entry.isActive = node->AttributeBool(L"active", false);
        if (const infra::XmlElement* marks = node->Child(L"Bookmarks"); marks != nullptr) {
            for (const infra::XmlElement* line : marks->Children(L"Line")) {
                const int value = line->AttributeInt(L"n", -1);
                if (value >= 0) {
                    entry.bookmarks.push_back(value);
                }
            }
        }
        if (const infra::XmlElement* folds = node->Child(L"Folds"); folds != nullptr) {
            for (const infra::XmlElement* line : folds->Children(L"Line")) {
                const int value = line->AttributeInt(L"n", -1);
                if (value >= 0) {
                    entry.folds.push_back(value);
                }
            }
        }
        parsed.documents.push_back(std::move(entry));
    }

    snapshot = std::move(parsed);
    return true;
}

}  // namespace npcc::core
