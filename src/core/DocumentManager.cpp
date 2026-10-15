#include "core/DocumentManager.h"

#include <windows.h>

#include <algorithm>

#include "edit/Encoding.h"
#include "edit/FileTier.h"
#include "infra/Log.h"
#include "infra/Paths.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"
#include "io/MappedFile.h"

namespace {
// 大文件只读开头这一段做编码探测
constexpr std::size_t kEncodingProbeBytes = 256 * 1024;
}

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
    document->view->ConfigureColumnEditing();
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

    // 统一成绝对路径再比较与存储：否则会话换工作目录就失效，
    // 而且同一个文件换个写法就会被当成两个文档（两份内容各自演化，很危险）。
    const std::wstring target = infra::NormalizePath(path);

    // 已经打开过就直接激活，不再读盘（避免同一文件两份不同内容的缓冲区）
    for (const auto& existing : documents_) {
        if (existing->buffer.HasPath() &&
            ::CompareStringOrdinal(existing->buffer.filePath.c_str(), -1, target.c_str(), -1,
                                   TRUE) == CSTR_EQUAL) {
            outcome.document = existing.get();
            outcome.status = OpenStatus::AlreadyOpen;
            Activate(existing.get());
            return outcome;
        }
    }

    auto document = std::make_unique<edit::Document>();
    document->buffer.id = static_cast<BufferId>(nextBufferId_++);
    document->buffer.filePath = target;
    document->buffer.displayName = FileNameOf(target);
    document->buffer.isUntitled = false;
    document->buffer.isModified = false;

    document->view = std::make_unique<edit::EditView>();
    if (!document->view->Create(viewParent_, NextControlId())) {
        outcome.status = OpenStatus::InternalError;
        outcome.message = L"创建编辑控件失败。";
        return outcome;
    }
    document->view->ApplyLightTheme();
    document->view->ConfigureColumnEditing();
    document->view->SetVisible(false);

    std::wstring loadError;
    OpenStatus loadStatus = OpenStatus::InternalError;
    if (!LoadInto(*document, loadError, loadStatus)) {
        outcome.message = loadError;
        outcome.status = loadStatus;
        return outcome;
    }
    outcome.message = document->buffer.loadWarning;

    document->view->ClearUndoBuffer();
    document->view->MarkSavePoint();

    edit::Document* added = AddDocument(std::move(document));
    Activate(added);
    std::wstring log = L"已打开: " + target;
    if (added->buffer.tier != edit::FileTier::Normal) {
        log += L"（" + std::wstring(edit::TierName(added->buffer.tier)) + L"，" +
               std::to_wstring(added->buffer.lastKnown.size / 1024) + L" KB）";
    }
    infra::LogInfo(L"file", log);
    outcome.document = added;
    outcome.status = OpenStatus::Ok;
    return outcome;
}

bool DocumentManager::LoadInto(edit::Document& document, std::wstring& error,
                               OpenStatus& status) {
    const std::wstring& target = document.buffer.filePath;
    if (document.view == nullptr) {
        error = L"文档视图无效。";
        return false;
    }
    document.buffer.loadWarning.clear();

    // 先看体积定档：这决定了走"全量读入"还是"映射 + 分块装载"
    std::uint64_t fileSize = 0;
    std::uint64_t lastWrite = 0;
    if (!io::QueryFileIdentity(target, fileSize, lastWrite)) {
        error = L"文件不存在或无法访问。";
        status = OpenStatus::NotFound;
        return false;
    }
    if (!tierPolicy_.Acceptable(fileSize)) {
        error = L"文件超过 " + std::to_wstring(tierPolicy_.refuseAboveBytes >> 20) +
                L" MB，当前版本暂不支持打开。";
        status = OpenStatus::TooLarge;
        return false;
    }
    const edit::FileTier tier = tierPolicy_.Classify(fileSize);
    document.buffer.tier = tier;
    document.buffer.lastKnown.size = fileSize;
    document.buffer.lastKnown.lastWriteMs = lastWrite;

    // 大文件路径用的是一份临时映射，装载完成后即可释放——
    // ILoader 会把数据拷进 Scintilla 的文档模型里
    io::MappedFile mapped;
    std::string loadedBytes;   // 仅常规路径使用

    if (tier == edit::FileTier::Normal) {
        const io::ReadResult read = io::ReadWholeFile(target, tierPolicy_.normalMaxBytes);
        if (!read.ok) {
            error = read.message;
            return false;
        }
        const edit::LoadResult loaded = edit::DetectAndPrepare(read.bytes, kFallbackCodePage);
        document.buffer.encoding = loaded.encoding;
        document.buffer.eol = loaded.eol;
        document.buffer.detectedEol = loaded.eol;
        document.buffer.loadWarning = loaded.warning;
        loadedBytes = loaded.editorBytes;
    } else {
        if (!mapped.Open(target)) {
            error = mapped.Error();
            status = (::GetLastError() == ERROR_ACCESS_DENIED) ? OpenStatus::AccessDenied
                                                               : OpenStatus::InternalError;
            return false;
        }
        const std::string_view mappedBytes(mapped.Data(), static_cast<size_t>(mapped.Size()));
        const std::string_view head = mappedBytes.substr(
            0, std::min<size_t>(mappedBytes.size(), kEncodingProbeBytes));
        const edit::EncodingState encoding =
            edit::DetectEncodingFromHead(head, kFallbackCodePage);
        if (encoding.uniMode == edit::UniMode::Utf16LE ||
            encoding.uniMode == edit::UniMode::Utf16BE) {
            // UTF-16 必须先转成 UTF-8 才能交给 Scintilla，那就要整份复制，
            // 大文件下等于把映射省下的内存又花回去。如实告知而不是硬撑。
            error = L"大文件模式下暂不支持 UTF-16 编码，请先用其他工具转换。";
            status = OpenStatus::EncodingFailed;
            return false;
        }
        document.buffer.encoding = encoding;
        document.buffer.eol = edit::DetectEol(head);
        document.buffer.detectedEol = document.buffer.eol;
        document.buffer.loadWarning = (tier == edit::FileTier::Huge)
                                          ? L"文件较大，已以只读方式打开。"
                                          : L"文件较大，已进入大文件模式。";
    }

    edit::EditView& view = *document.view;
    view.SetReadOnly(false);
    view.SetUndoCollection(true);
    view.SetCodePage(document.buffer.encoding.uniMode == edit::UniMode::Ansi
                         ? document.buffer.encoding.codePage
                         : edit::kCodePageUtf8);
    view.SetEolMode(document.buffer.eol);

    if (tier == edit::FileTier::Normal) {
        view.SetText(loadedBytes);
        view.ApplyLanguageForPath(target);
    } else {
        // BOM 不能交给 Scintilla：它按代码页解释缓冲区，BOM 会变成一个可见字符
        const std::size_t skip = document.buffer.encoding.hasBom ? 3u : 0u;
        const char* data = mapped.Data() + skip;
        const std::uint64_t dataSize = mapped.Size() - skip;
        if (!view.LoadFromChunkedData(data, dataSize, true)) {
            error = L"装载大文件失败。";
            return false;
        }
        if (tier == edit::FileTier::Huge) {
            view.ApplyHugeFileMode();
            document.buffer.isReadOnly = true;
        } else {
            view.ApplyLargeFileDisplayMode();
            view.ApplyLanguageForPath(target);
        }
    }
    return true;
}

bool DocumentManager::HasExternallyChanged(const edit::Document& document) const {
    if (!document.buffer.HasPath()) {
        return false;
    }
    std::uint64_t size = 0;
    std::uint64_t lastWrite = 0;
    if (!io::QueryFileIdentity(document.buffer.filePath, size, lastWrite)) {
        // 文件已经查不到了（被删或被改名）：只要之前记录过身份就算"变了"
        return document.buffer.lastKnown.Valid();
    }
    if (!document.buffer.lastKnown.Valid()) {
        return false;
    }
    return size != document.buffer.lastKnown.size ||
           lastWrite != document.buffer.lastKnown.lastWriteMs;
}

void DocumentManager::RefreshIdentity(edit::Document& document) {
    if (!document.buffer.HasPath()) {
        return;
    }
    io::QueryFileIdentity(document.buffer.filePath, document.buffer.lastKnown.size,
                          document.buffer.lastKnown.lastWriteMs);
}

bool DocumentManager::Reload(edit::Document& document, std::wstring& error) {
    if (!document.buffer.HasPath() || document.view == nullptr) {
        error = L"文档没有对应的磁盘文件，无法重新加载。";
        return false;
    }
    // 记下视图状态，重新装载之后原样恢复——用户的注意力位置不该因为
    // 一次后台改动而丢掉
    const int32_t caret = document.view->CaretPosition();
    const int32_t firstLine = document.view->FirstVisibleLine();
    const int32_t zoom = document.view->ZoomLevel();

    OpenStatus status = OpenStatus::InternalError;
    if (!LoadInto(document, error, status)) {
        return false;
    }
    document.view->ClearUndoBuffer();
    document.view->MarkSavePoint();
    document.view->SetZoom(zoom);
    document.view->SetCaretPosition(caret);
    document.view->SetFirstVisibleLine(firstLine);
    document.buffer.isModified = false;
    return true;
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
    const std::wstring target = infra::NormalizePath(path);

    std::string editorBytes = document.view->GetAllText();
    if (document.buffer.eol != edit::EolMode::Mixed) {
        editorBytes = edit::NormalizeEol(editorBytes, document.buffer.eol);
    }
    const std::string outBytes = edit::EncodeForSave(editorBytes, document.buffer.encoding);
    if (outBytes.empty() && !editorBytes.empty()) {
        outcome.message = L"按当前编码转换内容失败，文件未写入。";
        return outcome;
    }

    const io::WriteResult result = io::WriteFileAtomic(target, outBytes);
    if (!result.ok) {
        outcome.message = result.message;
        return outcome;
    }

    const bool pathChanged = !document.buffer.HasPath() || document.buffer.filePath != target;
    document.buffer.filePath = target;
    document.buffer.displayName = FileNameOf(target);
    document.buffer.isUntitled = false;
    document.buffer.isModified = false;
    document.view->MarkSavePoint();
    RefreshIdentity(document);
    if (pathChanged) {
        // 扩展名可能变了，重新按新路径识别语言
        document.view->ApplyLanguageForPath(target);
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
