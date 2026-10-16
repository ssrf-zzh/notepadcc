#include "shell/RemoteDialog.h"

#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "core/Messages.h"
#include "infra/Log.h"
#include "infra/RemoteService.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kBrowseClass = L"notepadccRemoteBrowse";

// 浏览对话框的控件号（站点编辑器用 1830~1849，两段不撞车）
enum : int {
    kIdSite = 1801,
    kIdPath = 1802,
    kIdStatus = 1803,
    kIdList = 1804,
    kIdNameEdit = 1805,
    kIdUp = 1810,
    kIdRefresh = 1811,
    kIdOpen = 1812,
    kIdUpload = 1813,
    kIdMkdir = 1814,
    kIdRename = 1815,
    kIdDelete = 1816,
    kIdSites = 1817,
    kIdClose = 1818,
};

int DpiOf(HWND reference) {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr && reference != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr) {
            const UINT actual = getDpi(reference);
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return static_cast<int>(dpi);
}

std::wstring TextOf(HWND control) {
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return text;
}

std::wstring Trimmed(std::wstring text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.erase(0, 1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    return text;
}

// 列表行的显示格式（回归脚本整篇核对的就是它）
std::wstring FormatEntryLine(const infra::RemoteEntry& entry) {
    if (entry.isDir) {
        return L"[目录] " + entry.name;
    }
    return entry.name + L"  (" + std::to_wstring(entry.size) + L" 字节)";
}

// 目录在前、文件在后，同类按名字（大小写不敏感）——顺序稳定，回归才能整篇核对
void SortEntries(std::vector<infra::RemoteEntry>* entries) {
    std::sort(entries->begin(), entries->end(),
              [](const infra::RemoteEntry& left, const infra::RemoteEntry& right) {
                  if (left.isDir != right.isDir) {
                      return left.isDir;
                  }
                  return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
              });
}

// 后台操作投回来的结果（指针所有权从线程转移到对话框）
struct RemotePayload {
    enum class Kind { List, Download, Upload, Mkdir, Rename, Delete };
    Kind                kind = Kind::List;
    std::wstring        label;        // 操作名（"下载"/"新建文件夹"…），写状态行与日志用
    infra::RemoteResult result;
    std::wstring        dir;          // List：这次列的是哪个目录（已解析初始目录）
    std::wstring        remotePath;   // 目标远程路径
    std::wstring        otherPath;    // Rename：新名字
    std::wstring        localPath;    // Download/Upload：本地路径
};
class RemoteDialogImpl {
public:
    RemoteDialog::Outcome Show(HWND owner, infra::ThreadPool& pool,
                               RemoteDialog::Request request);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void EnsureWindowClass();
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const { return ::MulDiv(value, DpiOf(owner_), 96); }
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w,
              int h);
    HWND MakeLabel(const wchar_t* text, int x, int y, int w, int h) {
        return Make(L"STATIC", text, WS_VISIBLE | SS_LEFT, 0, x, y, w, h);
    }

    const core::RemoteSite* SelectedSite() const;
    void SetStatus(const std::wstring& text) const;
    void SetPathText() const;
    void UpdateButtons();
    void RebuildSiteCombo(const std::wstring& selectName);
    void RefreshListBox();
    std::wstring ListText() const;
    int  SelectedEntryIndex() const;

    void OnSiteChanged();
    void RequestList(const std::wstring& dir);
    void StartDownload();
    void StartUpload();
    void StartMkdir();
    void StartRename();
    void StartDelete();
    void OpenSiteEditor();
    void ActivateListSelection();
    void HandlePayload(std::unique_ptr<RemotePayload> payload);

    HWND  owner_ = nullptr;
    HWND  hwnd_ = nullptr;
    HWND  siteCombo_ = nullptr;
    HWND  pathText_ = nullptr;
    HWND  statusText_ = nullptr;
    HWND  listBox_ = nullptr;
    HWND  nameEdit_ = nullptr;
    HWND  upButton_ = nullptr;
    HWND  refreshButton_ = nullptr;
    HWND  openButton_ = nullptr;
    HWND  uploadButton_ = nullptr;
    HWND  mkdirButton_ = nullptr;
    HWND  renameButton_ = nullptr;
    HWND  deleteButton_ = nullptr;
    HWND  sitesButton_ = nullptr;
    HWND  closeButton_ = nullptr;
    HFONT font_ = nullptr;
    bool  done_ = false;
    bool  busy_ = false;

    std::vector<core::RemoteSite> sites_;
    std::wstring            configRoot_;
    std::wstring            currentFilePath_;
    std::wstring            currentDir_;      // 空 = 还没连过（用站点初始目录）
    std::wstring            pendingNotice_;   // 操作成功后让刷新完的状态行也说一声
    std::wstring            openLocalPath_;   // 非空 = 关框后主窗口要打开它
    std::vector<infra::RemoteEntry> entries_;
    infra::ThreadPool*      pool_ = nullptr;
    bool                    sitesChanged_ = false;
};

void RemoteDialogImpl::EnsureWindowClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &RemoteDialogImpl::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kBrowseClass;
    ::RegisterClassExW(&wc);
    registered = true;   // 重复注册同名类会失败，这里只认第一次（与 ToolbarDialog 同）
}

HWND RemoteDialogImpl::Make(const wchar_t* cls, const wchar_t* text, const DWORD style,
                            const int id, const int x, const int y, const int w, const int h) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, h, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

const core::RemoteSite* RemoteDialogImpl::SelectedSite() const {
    if (siteCombo_ == nullptr) {
        return nullptr;
    }
    const int index = static_cast<int>(::SendMessageW(siteCombo_, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(sites_.size())) {
        return nullptr;
    }
    return &sites_[static_cast<std::size_t>(index)];
}
void RemoteDialogImpl::SetStatus(const std::wstring& text) const {
    if (statusText_ != nullptr) {
        ::SetWindowTextW(statusText_, text.c_str());
    }
}

void RemoteDialogImpl::SetPathText() const {
    if (pathText_ == nullptr) {
        return;
    }
    const std::wstring text =
        currentDir_.empty() ? L"当前目录：（未连接）" : L"当前目录：" + currentDir_;
    ::SetWindowTextW(pathText_, text.c_str());
}

std::wstring RemoteDialogImpl::ListText() const {
    std::wstring text;
    for (const infra::RemoteEntry& entry : entries_) {
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += FormatEntryLine(entry);
    }
    return text;
}

int RemoteDialogImpl::SelectedEntryIndex() const {
    const int index = static_cast<int>(::SendMessageW(listBox_, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return -1;
    }
    return index;
}

void RemoteDialogImpl::RefreshListBox() {
    ::SendMessageW(listBox_, LB_RESETCONTENT, 0, 0);
    for (const infra::RemoteEntry& entry : entries_) {
        const std::wstring line = FormatEntryLine(entry);
        ::SendMessageW(listBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    if (!entries_.empty()) {
        ::SendMessageW(listBox_, LB_SETCURSEL, 0, 0);
    }
}

void RemoteDialogImpl::UpdateButtons() {
    const core::RemoteSite* site = SelectedSite();
    const bool ready = site != nullptr && !busy_;
    const int selected = ready ? SelectedEntryIndex() : -1;
    const bool hasEntry = selected >= 0;
    const bool isDir = hasEntry && entries_[static_cast<std::size_t>(selected)].isDir;

    if (siteCombo_ != nullptr) {
        ::EnableWindow(siteCombo_, ready ? TRUE : FALSE);
    }
    ::EnableWindow(upButton_,
                   ready && !currentDir_.empty() && currentDir_ != L"/" ? TRUE : FALSE);
    ::EnableWindow(refreshButton_, ready && !currentDir_.empty() ? TRUE : FALSE);
    ::EnableWindow(openButton_, hasEntry && !isDir ? TRUE : FALSE);
    ::EnableWindow(uploadButton_, ready ? TRUE : FALSE);
    ::EnableWindow(mkdirButton_, ready ? TRUE : FALSE);
    ::EnableWindow(renameButton_, hasEntry ? TRUE : FALSE);
    ::EnableWindow(deleteButton_, hasEntry ? TRUE : FALSE);
    ::EnableWindow(sitesButton_, busy_ ? FALSE : TRUE);
    ::EnableWindow(nameEdit_, ready ? TRUE : FALSE);
}

void RemoteDialogImpl::RebuildSiteCombo(const std::wstring& selectName) {
    ::SendMessageW(siteCombo_, CB_RESETCONTENT, 0, 0);
    int select = sites_.empty() ? -1 : 0;
    for (std::size_t index = 0; index < sites_.size(); ++index) {
        ::SendMessageW(siteCombo_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(sites_[index].name.c_str()));
        if (!selectName.empty() && _wcsicmp(sites_[index].name.c_str(), selectName.c_str()) == 0) {
            select = static_cast<int>(index);
        }
    }
    ::SendMessageW(siteCombo_, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
}

void RemoteDialogImpl::OnSiteChanged() {
    currentDir_.clear();
    entries_.clear();
    pendingNotice_.clear();
    RefreshListBox();
    SetPathText();
    const core::RemoteSite* site = SelectedSite();
    if (site == nullptr) {
        SetStatus(L"还没有远程站点：点「站点设置…」加一个");
        UpdateButtons();
        return;
    }
    infra::LogInfo(L"remote", L"打开远程站点：" + site->name);
    RequestList(L"");
}

void RemoteDialogImpl::RequestList(const std::wstring& dir) {
    const core::RemoteSite* site = SelectedSite();
    if (site == nullptr || pool_ == nullptr || busy_) {
        return;
    }
    // 站点参数不合格就别去连了：错误话术与站点编辑器用的是同一份校验
    std::wstring validation;
    if (!core::ValidateRemoteSite(*site, &validation)) {
        SetStatus(L"失败：" + validation);
        UpdateButtons();
        return;
    }
    busy_ = true;
    SetStatus(dir.empty() ? L"连接中…" : L"读取目录…");
    UpdateButtons();
    const core::RemoteSite siteCopy = *site;
    const std::wstring requested = dir;
    const HWND target = hwnd_;
    pool_->Submit([target, siteCopy, requested]() {
        auto payload = std::make_unique<RemotePayload>();
        payload->kind = RemotePayload::Kind::List;
        payload->label = L"读取目录";
        infra::RemoteServiceOptions options;
        payload->dir =
            requested.empty() ? infra::RemoteInitialDir(siteCopy, options) : requested;
        payload->remotePath = payload->dir;
        payload->result = infra::RemoteListDir(siteCopy, payload->dir, options);
        if (::PostMessageW(target, core::kMsgRemoteResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;   // 对话框没了：指针随 unique_ptr 释放
        }
        payload.release();
    });
}
void RemoteDialogImpl::StartDownload() {
    const core::RemoteSite* site = SelectedSite();
    const int index = SelectedEntryIndex();
    if (site == nullptr || busy_ || index < 0) {
        return;
    }
    const infra::RemoteEntry entry = entries_[static_cast<std::size_t>(index)];
    if (entry.isDir) {
        return;
    }
    const std::wstring remotePath = core::RemoteJoin(currentDir_, entry.name);
    std::wstring localPath;
    std::wstring error;
    if (!core::RemoteCachePath(configRoot_, site->name, remotePath, &localPath, &error)) {
        SetStatus(L"失败：" + error);
        return;
    }
    busy_ = true;
    SetStatus(L"下载中…");
    UpdateButtons();
    const core::RemoteSite siteCopy = *site;
    const HWND target = hwnd_;
    pool_->Submit([target, siteCopy, remotePath, localPath]() {
        auto payload = std::make_unique<RemotePayload>();
        payload->kind = RemotePayload::Kind::Download;
        payload->label = L"下载";
        payload->remotePath = remotePath;
        payload->localPath = localPath;
        payload->result = infra::RemoteDownloadFile(siteCopy, remotePath, localPath);
        if (::PostMessageW(target, core::kMsgRemoteResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;
        }
        payload.release();
    });
}

void RemoteDialogImpl::StartUpload() {
    const core::RemoteSite* site = SelectedSite();
    if (site == nullptr || busy_) {
        return;
    }
    std::wstring remotePath;
    if (currentFilePath_.empty()) {
        SetStatus(L"失败：当前没有打开的文档，没法上传");
        return;
    }
    if (!core::RemotePathFromCache(configRoot_, site->name, currentFilePath_, &remotePath)) {
        SetStatus(L"失败：当前文档不在站点 \"" + site->name +
                  L"\"的远程缓存里（从远程下载打开的文档才能这样传回去）");
        return;
    }
    busy_ = true;
    SetStatus(L"上传中…");
    UpdateButtons();
    const core::RemoteSite siteCopy = *site;
    const std::wstring localPath = currentFilePath_;
    const HWND target = hwnd_;
    pool_->Submit([target, siteCopy, localPath, remotePath]() {
        auto payload = std::make_unique<RemotePayload>();
        payload->kind = RemotePayload::Kind::Upload;
        payload->label = L"上传";
        payload->remotePath = remotePath;
        payload->localPath = localPath;
        payload->result = infra::RemoteUploadFile(siteCopy, localPath, remotePath);
        if (::PostMessageW(target, core::kMsgRemoteResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;
        }
        payload.release();
    });
}

void RemoteDialogImpl::StartMkdir() {
    const core::RemoteSite* site = SelectedSite();
    if (site == nullptr || busy_) {
        return;
    }
    const std::wstring name = Trimmed(TextOf(nameEdit_));
    if (!core::RemoteIsValidComponent(name)) {
        SetStatus(L"失败：名字不能为空，也不能含 / \\ 与控制字符（\".\" 与 \"..\" 也不行）");
        return;
    }
    const std::wstring remotePath = core::RemoteJoin(currentDir_, name);
    busy_ = true;
    SetStatus(L"新建文件夹中…");
    UpdateButtons();
    const core::RemoteSite siteCopy = *site;
    const HWND target = hwnd_;
    pool_->Submit([target, siteCopy, remotePath]() {
        auto payload = std::make_unique<RemotePayload>();
        payload->kind = RemotePayload::Kind::Mkdir;
        payload->label = L"新建文件夹";
        payload->remotePath = remotePath;
        payload->result = infra::RemoteMakeDir(siteCopy, remotePath);
        if (::PostMessageW(target, core::kMsgRemoteResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;
        }
        payload.release();
    });
}

void RemoteDialogImpl::StartRename() {
    const core::RemoteSite* site = SelectedSite();
    const int index = SelectedEntryIndex();
    if (site == nullptr || busy_ || index < 0) {
        return;
    }
    const std::wstring name = Trimmed(TextOf(nameEdit_));
    if (!core::RemoteIsValidComponent(name)) {
        SetStatus(L"失败：名字不能为空，也不能含 / \\ 与控制字符（\".\" 与 \"..\" 也不行）");
        return;
    }
    const std::wstring fromPath =
        core::RemoteJoin(currentDir_, entries_[static_cast<std::size_t>(index)].name);
    const std::wstring toPath = core::RemoteJoin(currentDir_, name);
    busy_ = true;
    SetStatus(L"重命名中…");
    UpdateButtons();
    const core::RemoteSite siteCopy = *site;
    const HWND target = hwnd_;
    pool_->Submit([target, siteCopy, fromPath, toPath]() {
        auto payload = std::make_unique<RemotePayload>();
        payload->kind = RemotePayload::Kind::Rename;
        payload->label = L"重命名";
        payload->remotePath = fromPath;
        payload->otherPath = toPath;
        payload->result = infra::RemoteRenamePath(siteCopy, fromPath, toPath);
        if (::PostMessageW(target, core::kMsgRemoteResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;
        }
        payload.release();
    });
}

void RemoteDialogImpl::StartDelete() {
    const core::RemoteSite* site = SelectedSite();
    const int index = SelectedEntryIndex();
    if (site == nullptr || busy_ || index < 0) {
        return;
    }
    const infra::RemoteEntry entry = entries_[static_cast<std::size_t>(index)];
    const std::wstring remotePath = core::RemoteJoin(currentDir_, entry.name);
    const std::wstring prompt = std::wstring(L"确定删除远程") +
                                (entry.isDir ? L"目录" : L"文件") + L"？\n\n" + remotePath;
    const int answer = ::MessageBoxW(hwnd_, prompt.c_str(),
                                     entry.isDir ? L"删除远程目录" : L"删除远程文件",
                                     MB_ICONQUESTION | MB_YESNO);
    if (answer != IDYES) {
        return;
    }
    busy_ = true;
    SetStatus(L"删除中…");
    UpdateButtons();
    const core::RemoteSite siteCopy = *site;
    const bool isDir = entry.isDir;
    const HWND target = hwnd_;
    pool_->Submit([target, siteCopy, remotePath, isDir]() {
        auto payload = std::make_unique<RemotePayload>();
        payload->kind = RemotePayload::Kind::Delete;
        payload->label = isDir ? L"删除目录" : L"删除文件";
        payload->remotePath = remotePath;
        payload->result = isDir ? infra::RemoteDeleteDir(siteCopy, remotePath)
                                : infra::RemoteDeleteFile(siteCopy, remotePath);
        if (::PostMessageW(target, core::kMsgRemoteResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;
        }
        payload.release();
    });
}
void RemoteDialogImpl::ActivateListSelection() {
    const int index = SelectedEntryIndex();
    if (index < 0) {
        return;
    }
    if (entries_[static_cast<std::size_t>(index)].isDir) {
        RequestList(core::RemoteJoin(currentDir_, entries_[static_cast<std::size_t>(index)].name));
        return;
    }
    StartDownload();
}

void RemoteDialogImpl::OpenSiteEditor() {
    std::vector<core::RemoteSite> edited;
    if (!RemoteSiteDialog::Show(hwnd_, sites_, &edited)) {
        return;
    }
    const core::RemoteSite* previous = SelectedSite();
    const std::wstring previousName = previous != nullptr ? previous->name : std::wstring();
    sites_ = std::move(edited);
    sitesChanged_ = true;
    RebuildSiteCombo(previousName);
    OnSiteChanged();
}

void RemoteDialogImpl::HandlePayload(std::unique_ptr<RemotePayload> payload) {
    busy_ = false;
    switch (payload->kind) {
        case RemotePayload::Kind::List: {
            if (!payload->result.ok) {
                entries_.clear();
                RefreshListBox();
                SetPathText();
                SetStatus(L"失败：" + payload->result.error);
                break;
            }
            currentDir_ = payload->dir;
            entries_ = payload->result.entries;
            SortEntries(&entries_);
            RefreshListBox();
            SetPathText();
            std::wstring status = L"共 " + std::to_wstring(entries_.size()) + L" 项";
            if (!pendingNotice_.empty()) {
                status = pendingNotice_ + L"（" + status + L"）";
                pendingNotice_.clear();
            }
            SetStatus(status);
            break;
        }
        case RemotePayload::Kind::Download: {
            if (!payload->result.ok) {
                SetStatus(L"失败：" + payload->result.error);
                break;
            }
            infra::LogInfo(L"remote",
                           L"下载 " + payload->remotePath + L" → " + payload->localPath);
            openLocalPath_ = payload->localPath;
            done_ = true;   // 关框，主窗口接着走常规的打开流程
            return;
        }
        case RemotePayload::Kind::Upload: {
            if (!payload->result.ok) {
                SetStatus(L"失败：" + payload->result.error);
                break;
            }
            infra::LogInfo(L"remote",
                           L"上传 " + payload->localPath + L" → " + payload->remotePath);
            pendingNotice_ = payload->label + L"完成";
            RequestList(currentDir_);
            return;
        }
        case RemotePayload::Kind::Mkdir: {
            if (!payload->result.ok) {
                SetStatus(L"失败：" + payload->result.error);
                break;
            }
            infra::LogInfo(L"remote", L"新建目录 " + payload->remotePath);
            pendingNotice_ = payload->label + L"完成";
            RequestList(currentDir_);
            return;
        }
        case RemotePayload::Kind::Rename: {
            if (!payload->result.ok) {
                SetStatus(L"失败：" + payload->result.error);
                break;
            }
            infra::LogInfo(L"remote",
                           L"改名 " + payload->remotePath + L" → " + payload->otherPath);
            pendingNotice_ = payload->label + L"完成";
            RequestList(currentDir_);
            return;
        }
        case RemotePayload::Kind::Delete: {
            if (!payload->result.ok) {
                SetStatus(L"失败：" + payload->result.error);
                break;
            }
            infra::LogInfo(L"remote", payload->label + L" " + payload->remotePath);
            pendingNotice_ = payload->label + L"完成";
            RequestList(currentDir_);
            return;
        }
    }
    UpdateButtons();
}

LRESULT CALLBACK RemoteDialogImpl::WndProcThunk(const HWND hwnd, const UINT message,
                                                const WPARAM wParam, const LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<RemoteDialogImpl*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<RemoteDialogImpl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT RemoteDialogImpl::WndProc(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
        case core::kMsgRemoteResult: {
            // 指针所有权在这里接过来（与函数列表 / 更新对话框同一约定）
            std::unique_ptr<RemotePayload> payload(reinterpret_cast<RemotePayload*>(lParam));
            if (payload != nullptr) {
                HandlePayload(std::move(payload));
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kIdClose || id == IDCANCEL) {
                done_ = true;
                return 0;
            }
            if (id == kIdSite && notification == CBN_SELCHANGE) {
                OnSiteChanged();
                return 0;
            }
            if (id == kIdList && notification == LBN_SELCHANGE) {
                UpdateButtons();
                return 0;
            }
            if (id == kIdList && notification == LBN_DBLCLK) {
                ActivateListSelection();
                return 0;
            }
            switch (id) {
                case kIdUp:
                    RequestList(core::RemoteParentDir(currentDir_));
                    return 0;
                case kIdRefresh:
                    RequestList(currentDir_);
                    return 0;
                case kIdOpen:
                    StartDownload();
                    return 0;
                case kIdUpload:
                    StartUpload();
                    return 0;
                case kIdMkdir:
                    StartMkdir();
                    return 0;
                case kIdRename:
                    StartRename();
                    return 0;
                case kIdDelete:
                    StartDelete();
                    return 0;
                case kIdSites:
                    OpenSiteEditor();
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_GETTEXT: {
            // 列表内容的读回（跨进程读标准列表框的 LB_GETTEXT 要传指针，不可靠，
            // 与 XPath 对话框同款做法）
            const std::wstring text = ListText();
            auto* buffer = reinterpret_cast<wchar_t*>(lParam);
            const UINT capacity = static_cast<UINT>(wParam);
            if (buffer == nullptr || capacity == 0) {
                return 0;
            }
            const std::size_t count =
                text.size() < capacity - 1 ? text.size() : static_cast<std::size_t>(capacity) - 1;
            std::memcpy(buffer, text.data(), count * sizeof(wchar_t));
            buffer[count] = L'\0';
            return static_cast<LRESULT>(count);
        }
        case WM_GETTEXTLENGTH:
            return static_cast<LRESULT>(ListText().size());
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            done_ = true;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}
RemoteDialog::Outcome RemoteDialogImpl::Show(HWND owner, infra::ThreadPool& pool,
                                             RemoteDialog::Request request) {
    EnsureWindowClass();
    owner_ = owner;
    pool_ = &pool;
    sites_ = std::move(request.sites);
    configRoot_ = std::move(request.configRoot);
    currentFilePath_ = std::move(request.currentFilePath);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(760);
    const int height = Scale(520);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kBrowseClass, L"远程站点",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                              nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return RemoteDialog::Outcome{};
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    LOGFONTW logFont{};
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0) {
        logFont = metrics.lfMessageFont;
    } else {
        ::wcscpy_s(logFont.lfFaceName, L"Microsoft YaHei UI");
        logFont.lfHeight = -12;
    }
    font_ = ::CreateFontIndirectW(&logFont);

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int clientWidth = client.right;
    const int clientHeight = client.bottom;
    const int margin = Scale(14);
    const int rowHeight = Scale(24);
    const int gap = Scale(6);
    const int buttonHeight = Scale(26);
    const int labelWidth = Scale(56);

    const int siteRowY = margin;
    MakeLabel(L"站点：", margin, siteRowY + Scale(4), labelWidth, rowHeight);
    siteCombo_ = Make(L"COMBOBOX", L"",
                      WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kIdSite,
                      margin + labelWidth, siteRowY, Scale(240), Scale(200));
    sitesButton_ = Make(L"BUTTON", L"站点设置…", WS_VISIBLE | WS_TABSTOP, kIdSites,
                        margin + labelWidth + Scale(240) + gap, siteRowY, Scale(110),
                        buttonHeight);

    const int pathY = siteRowY + rowHeight + gap;
    pathText_ = Make(L"STATIC", L"当前目录：（未连接）", WS_VISIBLE | SS_LEFT, kIdPath, margin,
                     pathY, clientWidth - margin * 2, Scale(20));

    const int buttonY = clientHeight - margin - buttonHeight;
    const int statusY = buttonY - gap - Scale(20);
    const int nameRowY = statusY - gap - buttonHeight;
    const int listTop = pathY + Scale(20) + gap;
    listBox_ = Make(L"LISTBOX", L"",
                    WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, kIdList,
                    margin, listTop, clientWidth - margin * 2, nameRowY - gap - listTop);

    MakeLabel(L"名称：", margin, nameRowY + Scale(4), labelWidth, rowHeight);
    nameEdit_ = Make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                     kIdNameEdit, margin + labelWidth, nameRowY, Scale(220), buttonHeight);
    mkdirButton_ = Make(L"BUTTON", L"新建文件夹", WS_VISIBLE | WS_TABSTOP, kIdMkdir,
                        margin + labelWidth + Scale(220) + gap, nameRowY, Scale(110),
                        buttonHeight);
    renameButton_ = Make(L"BUTTON", L"重命名", WS_VISIBLE | WS_TABSTOP, kIdRename,
                         margin + labelWidth + Scale(220) + gap * 2 + Scale(110), nameRowY,
                         Scale(90), buttonHeight);

    statusText_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdStatus, margin, statusY,
                       clientWidth - margin * 2, Scale(20));

    upButton_ = Make(L"BUTTON", L"上一级", WS_VISIBLE | WS_TABSTOP, kIdUp, margin, buttonY,
                     Scale(90), buttonHeight);
    refreshButton_ = Make(L"BUTTON", L"刷新", WS_VISIBLE | WS_TABSTOP, kIdRefresh,
                          margin + Scale(90) + gap, buttonY, Scale(90), buttonHeight);
    closeButton_ = Make(L"BUTTON", L"关闭", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose,
                        clientWidth - margin - Scale(90), buttonY, Scale(90), buttonHeight);
    deleteButton_ = Make(L"BUTTON", L"删除", WS_VISIBLE | WS_TABSTOP, kIdDelete,
                         clientWidth - margin - (Scale(90) + gap) - Scale(90), buttonY,
                         Scale(90), buttonHeight);
    uploadButton_ = Make(L"BUTTON", L"上传当前文档", WS_VISIBLE | WS_TABSTOP, kIdUpload,
                         clientWidth - margin - (Scale(90) + gap) * 2 - Scale(130), buttonY,
                         Scale(130), buttonHeight);
    openButton_ = Make(L"BUTTON", L"下载并打开", WS_VISIBLE | WS_TABSTOP, kIdOpen,
                       clientWidth - margin - (Scale(90) + gap) * 2 - Scale(130) - gap -
                           Scale(110),
                       buttonY, Scale(110), buttonHeight);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](const HWND child, const LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }

    RebuildSiteCombo(L"");
    UpdateButtons();
    SetPathText();
    if (sites_.empty()) {
        SetStatus(L"还没有远程站点：点「站点设置…」加一个");
    } else {
        OnSiteChanged();
    }

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    if (sitesButton_ != nullptr) {
        ::SetFocus(sitesButton_);
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        if (::IsDialogMessageW(hwnd_, &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    ::EnableWindow(owner_, TRUE);
    ::SetForegroundWindow(owner_);
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;

    RemoteDialog::Outcome outcome;
    outcome.openLocalPath = std::move(openLocalPath_);
    outcome.sites = std::move(sites_);
    outcome.sitesChanged = sitesChanged_;
    return outcome;
}

}  // namespace

RemoteDialog::Outcome RemoteDialog::Show(HWND owner, infra::ThreadPool& pool, Request request) {
    RemoteDialogImpl dialog;
    return dialog.Show(owner, pool, std::move(request));
}

}  // namespace npcc::shell
