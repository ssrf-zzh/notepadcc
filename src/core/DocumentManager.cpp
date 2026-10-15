#include "core/DocumentManager.h"

#include <windows.h>

#include <algorithm>

#include "edit/Encoding.h"
#include "infra/Log.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"

namespace npcc::core {
namespace {

constexpr uint32_t kFallbackCodePage = 936;   // 简体中文 Windows 的默认 ANSI 代码页

}  // namespace

DocumentManager::DocumentManager(HWND viewParent) : viewParent_(viewParent) {}

DocumentManager::~DocumentManager() {
    for (auto& document : documents_) {
        if (document && document->view) {
            document->view->Destroy();
        }
    }
    documents_.clear();
    tabOrder_.clear();
    active_ = nullptr;
}

int DocumentManager::NextControlId() {
    return nextControlId_++;
}

std::wstring DocumentManager::FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DocumentManager::DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

edit::Document* DocumentManager::AddDocument(std::unique_ptr<edit::Document> document) {
    edit::Document* raw = document.get();
    tabOrder_.push_back(raw->buffer.id);
    documents_.push_back(std::move(document));
    return raw;
}

void DocumentManager::RemoveFromTabOrder(BufferId id) {
    tabOrder_.erase(std::remove(tabOrder_.begin(), tabOrder_.end(), id), tabOrder_.end());
}

edit::Document* DocumentManager::CreateUntitled() {
    auto document = std::make_unique<edit::Document>();
    document->buffer.id = static_cast<BufferId>(nextBufferId_++);
    document->buffer.isUntitled = true;
    document->buffer.isModified = false;
    document->buffer.encoding.uniMode = edit::UniMode::Utf8;
    document->buffer.encoding.confidence = 100;
    document->buffer.eol = edit::EolMode::Crlf;
    document->buffer.detectedEol = edit::EolMode::Crlf;
    document->buffer.displayName = L"新建 " + std::to_wstring(++untitledCounter_);

    document->view = std::make_unique<edit::EditView>();
    if (!document->view->Create(viewParent_, NextControlId())) {
        infra::LogError(L"doc", L"创建编辑控件失败（新建文档）");
        return nullptr;
    }
    document->view->ApplyLightTheme();
    document->view->SetCodePage(edit::kCodePageUtf8);
    document->view->SetEolMode(document->buffer.eol);
    document->view->SetText({});
    document->view->ApplyLanguageForPath(std::wstring());
    document->view->ClearUndoBuffer();
    document->view->MarkSavePoint();
    document->view->SetVisible(false);

    edit::Document* added = AddDocument(std::move(document));
    Activate(added);
    return added;
}

OpenOutcome DocumentManager::Open(const std::wstring& path) {
    OpenOutcome outcome;

    // 已经打开过就直接激活，不再读盘（避免同一文件两份不同内容的缓冲区）
    for (const auto& existing : documents_) {
        if (existing->buffer.HasPath() &&
            ::CompareStringOrdinal(existing->buffer.filePath.c_str(), -1, path.c_str(), -1, TRUE) ==
                CSTR_EQUAL) {
            outcome.document = existing.get();
            outcome.status = OpenStatus::AlreadyOpen;
            Activate(existing.get());
            return outcome;
        }
    }

    const io::ReadResult read = io::ReadWholeFile(path);
    if (!read.ok) {
        outcome.message = read.message;
        if (read.tooLarge) {
            outcome.status = OpenStatus::TooLarge;
        } else if (read.lastError == ERROR_ACCESS_DENIED) {
            outcome.status = OpenStatus::AccessDenied;
        } else if (read.lastError == ERROR_FILE_NOT_FOUND || read.lastError == ERROR_PATH_NOT_FOUND) {
            outcome.status = OpenStatus::NotFound;
        } else {
            outcome.status = OpenStatus::InternalError;
        }
        return outcome;
    }

    auto document = std::make_unique<edit::Document>();
    document->buffer.id = static_cast<BufferId>(nextBufferId_++);
    document->buffer.filePath = path;
    document->buffer.displayName = FileNameOf(path);
    document->buffer.isUntitled = false;
    document->buffer.isModified = false;

    const edit::LoadResult loaded = edit::DetectAndPrepare(read.bytes, kFallbackCodePage);
    document->buffer.encoding = loaded.encoding;
    document->buffer.eol = loaded.eol;
    document->buffer.detectedEol = loaded.eol;
    outcome.message = loaded.warning;   // 例如"检测到混合行尾"，非致命

    document->view = std::make_unique<edit::EditView>();
    if (!document->view->Create(viewParent_, NextControlId())) {
        outcome.status = OpenStatus::InternalError;
        outcome.message = L"创建编辑控件失败。";
        return outcome;
    }
    document->view->ApplyLightTheme();
    document->view->SetCodePage(document->buffer.encoding.uniMode == edit::UniMode::Ansi
                                    ? document->buffer.encoding.codePage
                                    : edit::kCodePageUtf8);
    document->view->SetEolMode(document->buffer.eol);
    document->view->SetText(loaded.editorBytes);
    document->view->ApplyLanguageForPath(path);
    document->view->ClearUndoBuffer();
    document->view->MarkSavePoint();
    document->view->SetVisible(false);

    edit::Document* added = AddDocument(std::move(document));
    Activate(added);
    infra::LogInfo(L"file", L"已打开: " + path);
    outcome.document = added;
    outcome.status = OpenStatus::Ok;
    return outcome;
}

SaveOutcome DocumentManager::Save(edit::Document& document) {
    if (!document.buffer.HasPath()) {
        return SaveOutcome{false, L"文档尚未指定保存路径。"};
    }
    return SaveAs(document, document.buffer.filePath);
}

SaveOutcome DocumentManager::SaveAs(edit::Document& document, const std::wstring& path) {
    SaveOutcome outcome;
    if (!document.view) {
        outcome.message = L"文档视图无效，无法保存。";
        return outcome;
    }

    std::string editorBytes = document.view->GetAllText();
    if (document.buffer.eol != edit::EolMode::Mixed) {
        editorBytes = edit::NormalizeEol(editorBytes, document.buffer.eol);
    }
    const std::string outBytes = edit::EncodeForSave(editorBytes, document.buffer.encoding);
    if (outBytes.empty() && !editorBytes.empty()) {
        outcome.message = L"按当前编码转换内容失败，文件未写入。";
        return outcome;
    }

    const io::WriteResult result = io::WriteFileAtomic(path, outBytes);
    if (!result.ok) {
        outcome.message = result.message;
        return outcome;
    }

    const bool pathChanged = !document.buffer.HasPath() || document.buffer.filePath != path;
    document.buffer.filePath = path;
    document.buffer.displayName = FileNameOf(path);
    document.buffer.isUntitled = false;
    document.buffer.isModified = false;
    document.view->MarkSavePoint();
    if (pathChanged) {
        // 扩展名可能变了，重新按新路径识别语言
        document.view->ApplyLanguageForPath(path);
    }
    outcome.ok = true;
    return outcome;
}

bool DocumentManager::Close(edit::Document* document) {
    if (document == nullptr) {
        return false;
    }
    const auto it = std::find_if(documents_.begin(), documents_.end(),
                                 [document](const std::unique_ptr<edit::Document>& item) {
                                     return item.get() == document;
                                 });
    if (it == documents_.end()) {
        return false;
    }

    const BufferId id = document->buffer.id;
    const bool wasActive = (document == active_);

    // 关闭后激活哪一个：优先占据原标签位置的文档，没有则取前一个
    size_t tabIndex = 0;
    const auto tabIt = std::find(tabOrder_.begin(), tabOrder_.end(), id);
    if (tabIt != tabOrder_.end()) {
        tabIndex = static_cast<size_t>(std::distance(tabOrder_.begin(), tabIt));
    }

    if ((*it)->view) {
        (*it)->view->Destroy();
    }
    documents_.erase(it);
    RemoveFromTabOrder(id);

    if (wasActive) {
        edit::Document* next = nullptr;
        if (!tabOrder_.empty()) {
            const size_t pick = std::min(tabIndex, tabOrder_.size() - 1);
            next = Find(tabOrder_[pick]);
        }
        active_ = nullptr;
        Activate(next);
    }
    return true;
}

edit::Document* DocumentManager::Find(BufferId id) const {
    for (const auto& document : documents_) {
        if (document->buffer.id == id) {
            return document.get();
        }
    }
    return nullptr;
}

void DocumentManager::Activate(BufferId id) {
    Activate(Find(id));
}

void DocumentManager::Activate(edit::Document* document) {
    active_ = document;
    ShowOnly(document);
}

void DocumentManager::MoveBefore(BufferId moving, BufferId before) {
    const auto from = std::find(tabOrder_.begin(), tabOrder_.end(), moving);
    if (from == tabOrder_.end()) {
        return;
    }
    const BufferId value = *from;
    tabOrder_.erase(from);

    if (before == kInvalidId) {
        tabOrder_.push_back(value);
        return;
    }
    const auto to = std::find(tabOrder_.begin(), tabOrder_.end(), before);
    tabOrder_.insert(to, value);
}

void DocumentManager::SetTabOrder(const std::vector<BufferId>& order) {
    std::vector<BufferId> rebuilt;
    rebuilt.reserve(tabOrder_.size());
    for (const BufferId id : order) {
        if (Find(id) != nullptr && std::find(rebuilt.begin(), rebuilt.end(), id) == rebuilt.end()) {
            rebuilt.push_back(id);
        }
    }
    // 顺序表里漏掉的文档补到末尾，避免标签凭空消失
    for (const BufferId id : tabOrder_) {
        if (std::find(rebuilt.begin(), rebuilt.end(), id) == rebuilt.end()) {
            rebuilt.push_back(id);
        }
    }
    tabOrder_ = std::move(rebuilt);
}

bool DocumentManager::AnyModified() const {
    for (const auto& document : documents_) {
        if (document->buffer.isModified) {
            return true;
        }
    }
    return false;
}

std::vector<edit::Document*> DocumentManager::All() const {
    std::vector<edit::Document*> result;
    result.reserve(documents_.size());
    for (const auto& document : documents_) {
        result.push_back(document.get());
    }
    return result;
}

void DocumentManager::ShowOnly(edit::Document* document) const {
    for (const auto& item : documents_) {
        if (item->view) {
            item->view->SetVisible(item.get() == document);
        }
    }
}

void DocumentManager::ResizeActive(const RECT& area) const {
    if (active_ != nullptr && active_->view) {
        active_->view->Resize(area);
    }
}

}  // namespace npcc::core
