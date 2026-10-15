#include "shell/MainWindow.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "infra/Log.h"
#include "core/SessionManager.h"
#include "shell/MenuBuilder.h"
#include "shell/TabBar.h"

namespace npcc::shell {
namespace {

// 命令 ID：高 4 位类别 + 低 12 位序号（见 core/Ids.h 的 MakeCmdId）
enum : npcc::core::CmdId {
    kCmdFileNew     = npcc::core::MakeCmdId(1, 1),
    kCmdFileOpen    = npcc::core::MakeCmdId(1, 2),
    kCmdFileSave    = npcc::core::MakeCmdId(1, 3),
    kCmdFileSaveAs  = npcc::core::MakeCmdId(1, 4),
    kCmdFileClose   = npcc::core::MakeCmdId(1, 5),
    kCmdFileCloseAll= npcc::core::MakeCmdId(1, 6),
    kCmdFileExit    = npcc::core::MakeCmdId(1, 7),

    kCmdEditUndo      = npcc::core::MakeCmdId(2, 1),
    kCmdEditRedo      = npcc::core::MakeCmdId(2, 2),
    kCmdEditCut       = npcc::core::MakeCmdId(2, 3),
    kCmdEditCopy      = npcc::core::MakeCmdId(2, 4),
    kCmdEditPaste     = npcc::core::MakeCmdId(2, 5),
    kCmdEditSelectAll = npcc::core::MakeCmdId(2, 6),

    kCmdViewZoomIn        = npcc::core::MakeCmdId(4, 1),
    kCmdViewZoomOut       = npcc::core::MakeCmdId(4, 2),
    kCmdViewZoomReset     = npcc::core::MakeCmdId(4, 3),
    kCmdViewToggleWrap    = npcc::core::MakeCmdId(4, 4),
    kCmdViewToggleSpaces  = npcc::core::MakeCmdId(4, 5),
    kCmdViewToggleEol     = npcc::core::MakeCmdId(4, 6),
    kCmdViewToggleNumbers = npcc::core::MakeCmdId(4, 7),

    kCmdWindowNext = npcc::core::MakeCmdId(11, 1),
    kCmdWindowPrev = npcc::core::MakeCmdId(11, 2),

    kCmdHelpAbout = npcc::core::MakeCmdId(12, 1),

    // Scintilla 原生命令：不新增语义，直接转发
    kSciRedo      = 2011,
    kSciSelectAll = 2013,
    kSciUndo      = 2176,
    kSciCut       = 2177,
    kSciCopy      = 2178,
    kSciPaste     = 2179,
};

void CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || ::OpenClipboard(owner) == 0) {
        return;
    }
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr) {
        if (void* target = ::GlobalLock(memory); target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            ::GlobalUnlock(memory);
            if (::SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                memory = nullptr;   // 所有权已转移给剪贴板
            }
        }
        if (memory != nullptr) {
            ::GlobalFree(memory);
        }
    }
    ::CloseClipboard();
}

}  // namespace

MainWindow::~MainWindow() {
    tabBar_.Destroy();
    documents_.reset();
    if (accel_ != nullptr) {
        ::DestroyAcceleratorTable(accel_);
        accel_ = nullptr;
    }
    if (menu_ != nullptr) {
        ::DestroyMenu(menu_);
        menu_ = nullptr;
    }
}

bool MainWindow::RegisterWindowClass(HINSTANCE instance) const {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0;
}

bool MainWindow::Create(HINSTANCE instance, const std::vector<std::wstring>& filesToOpen) {
    instance_ = instance;
    hwnd_ = ::CreateWindowExW(0, kClassName, L"notepadcc", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                              nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    for (const std::wstring& path : filesToOpen) {
        const core::OpenOutcome outcome = documents_->Open(path);
        if (outcome.status == core::OpenStatus::Ok || outcome.status == core::OpenStatus::AlreadyOpen) {
            lastDirectory_ = core::DocumentManager::DirectoryOf(path);
        } else {
            infra::LogWarn(L"file", L"打开失败: " + path + L" / " + outcome.message);
            ::MessageBoxW(hwnd_, outcome.message.c_str(), L"打开文件失败", MB_ICONWARNING);
        }
    }
    SyncTabBar();
    ActivateDocument(documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0);
    return true;
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT MainWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            tabBar_.Create(hwnd_, 1);
            statusBar_.Create(hwnd_, 2);
            documents_ = std::make_unique<core::DocumentManager>(hwnd_);

            RegisterCommands();
            menu_ = BuildMainMenu(registry_);
            if (menu_ != nullptr) {
                ::SetMenu(hwnd_, menu_);
            }
            RebuildAccelerators();

            ::DragAcceptFiles(hwnd_, TRUE);
            // 优先恢复上次会话；没有可用会话时才给一个空白文档
            if (!RestoreSession()) {
                EnsureAtLeastOneDocument();
            }
            return 0;
        }
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_SETFOCUS: {
            if (edit::Document* document = documents_->Active(); document != nullptr && document->view) {
                ::SetFocus(document->view->Handle());
            }
            return 0;
        }
        case WM_COMMAND: {
            const auto id = static_cast<core::CmdId>(LOWORD(wParam));
            const core::CmdContext context = Context();
            if (registry_.Execute(id, context) == core::ExecResult::NotFound) {
                return ::DefWindowProcW(hwnd_, message, wParam, lParam);
            }
            UpdateStatus();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->hwndFrom == tabBar_.Handle()) {
                HandleTabBarNotify(*reinterpret_cast<const TabBarNotify*>(lParam));
                return 0;
            }
            if (DocumentOfView(header->hwndFrom) != nullptr) {
                HandleEditorNotify(*header);
                return 0;
            }
            break;
        }
        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && i < 32; ++i) {
                std::vector<wchar_t> buffer(4096, L'\0');
                if (::DragQueryFileW(drop, i, buffer.data(),
                                     static_cast<UINT>(buffer.size())) > 0) {
                    const core::OpenOutcome outcome = documents_->Open(buffer.data());
                    if (outcome.status != core::OpenStatus::Ok &&
                        outcome.status != core::OpenStatus::AlreadyOpen) {
                        ::MessageBoxW(hwnd_, outcome.message.c_str(), L"打开文件失败", MB_ICONWARNING);
                    }
                }
            }
            ::DragFinish(drop);
            SyncTabBar();
            ActivateDocument(documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0);
            return 0;
        }
        case WM_CLOSE: {
            // 会话要在关闭文档之前取快照——CloseAllDocuments 之后就没得记了
            const core::SessionSnapshot snapshot = BuildSessionSnapshot();
            if (CloseAllDocuments()) {
                std::wstring error;
                if (!core::SessionManager::Save(snapshot, &error)) {
                    infra::LogWarn(L"session", L"会话保存失败: " + error);
                }
                ::DestroyWindow(hwnd_);
            }
            return 0;
        }
        case WM_DESTROY:
            // DestroyWindow 先销毁子窗口，此时编辑控件的句柄已失效：
            // 只清空记录，不再对它们发消息（否则是对失效句柄的调用）。
            if (documents_) {
                for (edit::Document* document : documents_->All()) {
                    if (document->view) {
                        document->view->Detach();
                    }
                }
            }
            tabBar_.Destroy();
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

core::CmdContext MainWindow::Context() {
    core::CmdContext context;
    if (documents_) {
        if (edit::Document* document = documents_->Active(); document != nullptr) {
            context.buffer = &document->buffer;
            context.view = document->view.get();
        }
    }
    return context;
}

edit::Document* MainWindow::DocumentOfView(HWND view) const {
    if (!documents_ || view == nullptr) {
        return nullptr;
    }
    for (edit::Document* document : documents_->All()) {
        if (document->view && document->view->Handle() == view) {
            return document;
        }
    }
    return nullptr;
}

void MainWindow::RegisterCommands() {
    using core::CmdCategory;
    using core::CmdContext;

    auto add = [this](core::CmdId id, CmdCategory category, const wchar_t* key,
                      const wchar_t* text, uint32_t flags,
                      std::function<void(const CmdContext&)> handler) {
        core::Command command;
        command.id = id;
        command.category = category;
        command.key = key;
        command.text = text;
        command.flags = flags;
        command.execute = std::move(handler);
        registry_.Register(std::move(command));
    };

    const uint32_t needsBuffer = core::kCmdNeedsBuffer;
    const uint32_t needsWritable = core::kCmdNeedsBuffer | core::kCmdNeedsWritable;

    // ---- 文件 ----
    add(kCmdFileNew, CmdCategory::File, L"cmd.file.new", L"新建(&N)",
        core::kCmdNone, [this](const CmdContext&) { CommandNew(); });
    add(kCmdFileOpen, CmdCategory::File, L"cmd.file.open", L"打开(&O)...",
        core::kCmdNone, [this](const CmdContext&) { CommandOpen(); });
    add(kCmdFileSave, CmdCategory::File, L"cmd.file.save", L"保存(&S)",
        needsBuffer, [this](const CmdContext&) { CommandSave(); });
    add(kCmdFileSaveAs, CmdCategory::File, L"cmd.file.saveAs", L"另存为(&A)...",
        needsBuffer, [this](const CmdContext&) { CommandSaveAs(); });
    add(kCmdFileClose, CmdCategory::File, L"cmd.file.close", L"关闭当前文档(&C)",
        needsBuffer, [this](const CmdContext&) { CommandCloseCurrent(); });
    add(kCmdFileCloseAll, CmdCategory::File, L"cmd.file.closeAll", L"关闭全部文档(&L)",
        core::kCmdNone, [this](const CmdContext&) { CommandCloseAll(); });
    add(kCmdFileExit, CmdCategory::File, L"cmd.file.exit", L"退出(&X)",
        core::kCmdNone, [this](const CmdContext&) { ::PostMessageW(hwnd_, WM_CLOSE, 0, 0); });

    // ---- 编辑 ----
    add(kCmdEditUndo, CmdCategory::Edit, L"cmd.edit.undo", L"撤销(&U)",
        needsWritable, [this](const CmdContext& c) { c.view->Send(kSciUndo); });
    add(kCmdEditRedo, CmdCategory::Edit, L"cmd.edit.redo", L"重做(&R)",
        needsWritable, [this](const CmdContext& c) { c.view->Send(kSciRedo); });
    add(kCmdEditCut, CmdCategory::Edit, L"cmd.edit.cut", L"剪切(&T)",
        needsWritable, [this](const CmdContext& c) { c.view->Send(kSciCut); });
    add(kCmdEditCopy, CmdCategory::Edit, L"cmd.edit.copy", L"复制(&C)",
        needsBuffer, [this](const CmdContext& c) { c.view->Send(kSciCopy); });
    add(kCmdEditPaste, CmdCategory::Edit, L"cmd.edit.paste", L"粘贴(&P)",
        needsWritable, [this](const CmdContext& c) { c.view->Send(kSciPaste); });
    add(kCmdEditSelectAll, CmdCategory::Edit, L"cmd.edit.selectAll", L"全选(&L)",
        needsBuffer, [this](const CmdContext& c) { c.view->Send(kSciSelectAll); });

    // ---- 视图 ----
    add(kCmdViewZoomIn, CmdCategory::View, L"cmd.view.zoomIn", L"放大(&I)",
        needsBuffer, [this](const CmdContext& c) { c.view->SetZoom(c.view->ZoomLevel() + 1); });
    add(kCmdViewZoomOut, CmdCategory::View, L"cmd.view.zoomOut", L"缩小(&O)",
        needsBuffer, [this](const CmdContext& c) { c.view->SetZoom(c.view->ZoomLevel() - 1); });
    add(kCmdViewZoomReset, CmdCategory::View, L"cmd.view.zoomReset", L"恢复默认缩放(&Z)",
        needsBuffer, [this](const CmdContext& c) { c.view->SetZoom(0); });
    add(kCmdViewToggleWrap, CmdCategory::View, L"cmd.view.toggleWrap", L"自动换行(&W)",
        core::kCmdCheckable, [this](const CmdContext&) {
            wrapEnabled_ = !wrapEnabled_;
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->SetWrapMode(wrapEnabled_);
            }
        });
    add(kCmdViewToggleSpaces, CmdCategory::View, L"cmd.view.toggleSpaces", L"显示空白符号(&S)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showWhitespace_ = !showWhitespace_;
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->SetShowWhitespace(showWhitespace_);
            }
        });
    add(kCmdViewToggleEol, CmdCategory::View, L"cmd.view.toggleEol", L"显示行尾符(&E)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showEol_ = !showEol_;
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->SetShowEol(showEol_);
            }
        });
    add(kCmdViewToggleNumbers, CmdCategory::View, L"cmd.view.toggleNumbers", L"显示行号(&N)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showLineNumbers_ = !showLineNumbers_;
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->ShowLineNumbers(showLineNumbers_);
            }
        });

    // ---- 窗口 ----
    add(kCmdWindowNext, CmdCategory::Window, L"cmd.window.next", L"下一个标签(&N)",
        core::kCmdNone, [this](const CmdContext&) { CommandNextTab(); });
    add(kCmdWindowPrev, CmdCategory::Window, L"cmd.window.prev", L"上一个标签(&P)",
        core::kCmdNone, [this](const CmdContext&) { CommandPrevTab(); });

    // ---- 帮助 ----
    add(kCmdHelpAbout, CmdCategory::Help, L"cmd.help.about", L"关于 notepadcc(&A)",
        core::kCmdNone, [this](const CmdContext&) { CommandAbout(); });

    // 勾选态统一由命令注册表回答（设计文档 1.3 规则 1）
    auto bindChecked = [this](core::CmdId id, std::function<bool()> predicate) {
        core::Command* command = const_cast<core::Command*>(registry_.Find(id));
        if (command != nullptr) {
            command->queryChecked = [predicate](const CmdContext&) { return predicate(); };
        }
    };
    bindChecked(kCmdViewToggleWrap, [this] { return wrapEnabled_; });
    bindChecked(kCmdViewToggleSpaces, [this] { return showWhitespace_; });
    bindChecked(kCmdViewToggleEol, [this] { return showEol_; });
    bindChecked(kCmdViewToggleNumbers, [this] { return showLineNumbers_; });

    // ---- 默认快捷键 ----
    const uint8_t ctrl = core::kModCtrl;
    const uint8_t ctrlShift = core::kModCtrl | core::kModShift;
    const uint8_t ctrlAlt = core::kModCtrl | core::kModAlt;
    registry_.BindShortcut(kCmdFileNew, ctrl, 'N', true);
    registry_.BindShortcut(kCmdFileOpen, ctrl, 'O', true);
    registry_.BindShortcut(kCmdFileSave, ctrl, 'S', true);
    registry_.BindShortcut(kCmdFileSaveAs, ctrlShift, 'S', true);
    registry_.BindShortcut(kCmdFileClose, ctrl, 'W', true);
    registry_.BindShortcut(kCmdFileCloseAll, ctrlShift, 'W', true);
    registry_.BindShortcut(kCmdEditUndo, ctrl, 'Z', true);
    registry_.BindShortcut(kCmdEditRedo, ctrl, 'Y', true);
    registry_.BindShortcut(kCmdEditCut, ctrl, 'X', true);
    registry_.BindShortcut(kCmdEditCopy, ctrl, 'C', true);
    registry_.BindShortcut(kCmdEditPaste, ctrl, 'V', true);
    registry_.BindShortcut(kCmdEditSelectAll, ctrl, 'A', true);
    registry_.BindShortcut(kCmdViewZoomIn, ctrl, VK_OEM_PLUS, true);
    registry_.BindShortcut(kCmdViewZoomOut, ctrl, VK_OEM_MINUS, true);
    registry_.BindShortcut(kCmdViewZoomReset, ctrl, '0', true);
    registry_.BindShortcut(kCmdViewToggleWrap, core::kModAlt, 'Z', true);
    registry_.BindShortcut(kCmdViewToggleSpaces, ctrlAlt, 'W', true);
    registry_.BindShortcut(kCmdViewToggleEol, ctrlAlt, 'E', true);
    registry_.BindShortcut(kCmdWindowNext, ctrl, VK_TAB, true);
    registry_.BindShortcut(kCmdWindowPrev, ctrlShift, VK_TAB, true);
    registry_.BindShortcut(kCmdWindowNext, ctrl, VK_NEXT, false);
    registry_.BindShortcut(kCmdWindowPrev, ctrl, VK_PRIOR, false);
}

void MainWindow::RebuildAccelerators() {
    std::vector<ACCEL> entries;
    for (const auto& binding : registry_.AllBindings()) {
        if (binding.cmdId > 0xFFFF) {
            continue;   // ACCEL.cmd 是 WORD
        }
        ACCEL entry{};
        entry.fVirt = FVIRTKEY;
        if ((binding.modifiers & core::kModCtrl) != 0) {
            entry.fVirt |= FCONTROL;
        }
        if ((binding.modifiers & core::kModAlt) != 0) {
            entry.fVirt |= FALT;
        }
        if ((binding.modifiers & core::kModShift) != 0) {
            entry.fVirt |= FSHIFT;
        }
        entry.key = binding.keyCode;
        entry.cmd = static_cast<WORD>(binding.cmdId);
        entries.push_back(entry);
    }
    if (accel_ != nullptr) {
        ::DestroyAcceleratorTable(accel_);
        accel_ = nullptr;
    }
    if (!entries.empty()) {
        accel_ = ::CreateAcceleratorTableW(entries.data(), static_cast<int>(entries.size()));
    }
}

void MainWindow::LayoutChildren() const {
    if (!documents_) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    statusBar_.Resize(client);

    const int tabHeight = tabBar_.Height();
    if (tabBar_.Handle() != nullptr) {
        ::MoveWindow(tabBar_.Handle(), 0, 0, client.right, tabHeight, TRUE);
    }

    RECT viewRect{};
    viewRect.left = 0;
    viewRect.top = tabHeight;
    viewRect.right = client.right;
    viewRect.bottom = client.bottom - statusBar_.Height();
    if (viewRect.bottom < viewRect.top) {
        viewRect.bottom = viewRect.top;
    }
    documents_->ResizeActive(viewRect);
}

void MainWindow::ApplyViewToggles(edit::EditView& view) const {
    // 这些是控件级设置，新激活的视图需要按当前全局状态复现
    view.SetWrapMode(wrapEnabled_);
    view.SetShowWhitespace(showWhitespace_);
    view.SetShowEol(showEol_);
    view.ShowLineNumbers(showLineNumbers_);
}

void MainWindow::SyncTabBar() {
    if (!documents_) {
        return;
    }
    std::vector<TabItem> items;
    items.reserve(documents_->TabOrder().size());
    for (const core::BufferId id : documents_->TabOrder()) {
        edit::Document* document = documents_->Find(id);
        if (document == nullptr) {
            continue;
        }
        TabItem item;
        item.id = id;
        item.title = document->buffer.displayName.empty() ? L"未命名"
                                                          : document->buffer.displayName;
        item.modified = document->buffer.isModified;
        item.hasPath = document->buffer.HasPath();
        items.push_back(std::move(item));
    }
    const core::BufferId active =
        documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0;
    tabBar_.SetItems(std::move(items), active);
}

void MainWindow::UpdateTitle() const {
    std::wstring title = L"notepadcc";
    if (documents_) {
        if (edit::Document* document = documents_->Active(); document != nullptr) {
            title += L" - " + document->buffer.displayName;
            if (document->buffer.isModified) {
                title += L" *";
            }
        }
    }
    ::SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::UpdateStatus() const {
    if (!documents_) {
        return;
    }
    edit::Document* document = documents_->Active();
    if (document == nullptr || !document->view) {
        statusBar_.SetPosition(0, 0, 0);
        statusBar_.SetDocumentInfo(L"", L"", L"", false);
        return;
    }
    statusBar_.SetPosition(document->view->CurrentLine(), document->view->CurrentColumn(),
                           document->view->SelectionByteCount());
    statusBar_.SetDocumentInfo(edit::EncodingName(document->buffer.encoding),
                               edit::EolName(document->buffer.eol),
                               document->view->CurrentLanguageName(),
                               document->buffer.isModified);
}

void MainWindow::ActivateDocument(core::BufferId id) {
    if (!documents_) {
        return;
    }
    documents_->Activate(id);
    if (edit::Document* document = documents_->Active(); document != nullptr && document->view) {
        ApplyViewToggles(*document->view);
        ::SetFocus(document->view->Handle());
    }
    tabBar_.SetActive(id);
    LayoutChildren();
    UpdateTitle();
    UpdateStatus();
}

void MainWindow::EnsureAtLeastOneDocument() {
    if (!documents_) {
        return;
    }
    if (documents_->Count() == 0) {
        documents_->CreateUntitled();
    }
    if (documents_->Active() == nullptr && !documents_->TabOrder().empty()) {
        documents_->Activate(documents_->TabOrder().front());
    }
    SyncTabBar();
    ActivateDocument(documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0);
}

core::SessionSnapshot MainWindow::BuildSessionSnapshot() const {
    core::SessionSnapshot snapshot;
    if (!documents_) {
        return snapshot;
    }
    for (const core::BufferId id : documents_->TabOrder()) {
        edit::Document* document = documents_->Find(id);
        // 只记有路径的文档，见 SessionManager 的说明
        if (document == nullptr || !document->buffer.HasPath() || !document->view) {
            continue;
        }
        core::SessionDocument entry;
        entry.filePath = document->buffer.filePath;
        entry.caretPos = document->view->CaretPosition();
        entry.firstVisibleLine = document->view->FirstVisibleLine();
        entry.zoomLevel = document->view->ZoomLevel();
        entry.isActive = (document == documents_->Active());
        snapshot.documents.push_back(std::move(entry));
    }
    return snapshot;
}

bool MainWindow::RestoreSession() {
    core::SessionSnapshot snapshot;
    std::wstring error;
    if (!core::SessionManager::Load(snapshot, &error)) {
        if (!error.empty()) {
            infra::LogInfo(L"session", L"未恢复会话: " + error);
        }
        return false;
    }
    if (snapshot.documents.empty()) {
        return false;
    }

    edit::Document* active = nullptr;
    int restored = 0;
    for (const core::SessionDocument& entry : snapshot.documents) {
        const core::OpenOutcome outcome = documents_->Open(entry.filePath);
        if (outcome.status != core::OpenStatus::Ok &&
            outcome.status != core::OpenStatus::AlreadyOpen) {
            // 文件被删除或改名是常态，跳过继续恢复其余文档，不打断启动
            infra::LogWarn(L"session", L"会话中的文件已无法打开: " + entry.filePath);
            continue;
        }
        edit::Document* document = outcome.document;
        if (document == nullptr || !document->view) {
            continue;
        }
        // 先定位光标（会顺带滚动），再用记录的首可见行覆盖滚动位置
        document->view->SetCaretPosition(entry.caretPos);
        document->view->SetFirstVisibleLine(entry.firstVisibleLine);
        document->view->SetZoom(entry.zoomLevel);
        if (entry.isActive) {
            active = document;
        }
        ++restored;
    }
    if (restored == 0) {
        infra::LogWarn(L"session", L"会话中的所有文件都无法打开，改为新建文档");
        return false;
    }

    SyncTabBar();
    const core::BufferId target =
        active != nullptr ? active->buffer.id : documents_->TabOrder().front();
    ActivateDocument(target);
    infra::LogInfo(L"session", L"已恢复 " + std::to_wstring(restored) + L" 个文档");
    return true;
}

bool MainWindow::SaveDocument(edit::Document& document) {
    if (!document.buffer.HasPath()) {
        return SaveDocumentAs(document);
    }
    const core::SaveOutcome outcome = documents_->Save(document);
    if (!outcome.ok) {
        infra::LogError(L"file", L"保存失败: " + document.buffer.filePath + L" / " + outcome.message);
        ::MessageBoxW(hwnd_, outcome.message.c_str(), L"保存失败", MB_ICONERROR);
        return false;
    }
    lastDirectory_ = core::DocumentManager::DirectoryOf(document.buffer.filePath);
    infra::LogInfo(L"file", L"已保存: " + document.buffer.filePath);
    SyncTabBar();
    UpdateTitle();
    UpdateStatus();
    return true;
}

bool MainWindow::SaveDocumentAs(edit::Document& document) {
    std::vector<wchar_t> buffer(32768, L'\0');
    if (!document.buffer.displayName.empty()) {
        std::wcsncpy(buffer.data(), document.buffer.displayName.c_str(), buffer.size() - 1);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"所有文件\0*.*\0文本文件\0*.txt\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrDefExt = L"txt";
    if (!lastDirectory_.empty()) {
        dialog.lpstrInitialDir = lastDirectory_.c_str();
    }
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (::GetSaveFileNameW(&dialog) == 0) {
        return false;
    }

    const core::SaveOutcome outcome = documents_->SaveAs(document, buffer.data());
    if (!outcome.ok) {
        infra::LogError(L"file", L"另存为失败: " + std::wstring(buffer.data()) + L" / " +
                                     outcome.message);
        ::MessageBoxW(hwnd_, outcome.message.c_str(), L"保存失败", MB_ICONERROR);
        return false;
    }
    lastDirectory_ = core::DocumentManager::DirectoryOf(buffer.data());
    infra::LogInfo(L"file", L"已另存为: " + std::wstring(buffer.data()));
    SyncTabBar();
    UpdateTitle();
    UpdateStatus();
    return true;
}

bool MainWindow::ConfirmSaveIfModified(edit::Document& document) {
    if (!document.buffer.isModified) {
        return true;
    }
    const std::wstring question = L"是否保存对「" + document.buffer.displayName + L"」的更改？";
    const int answer = ::MessageBoxW(hwnd_, question.c_str(), L"notepadcc",
                                     MB_ICONQUESTION | MB_YESNOCANCEL);
    if (answer == IDYES) {
        return SaveDocument(document);
    }
    return answer == IDNO;
}

bool MainWindow::CloseDocument(core::BufferId id, bool force) {
    edit::Document* document = documents_->Find(id);
    if (document == nullptr) {
        return true;
    }
    if (!force && !ConfirmSaveIfModified(*document)) {
        return false;
    }
    return documents_->Close(document);
}

bool MainWindow::CloseOthers(core::BufferId keep) {
    // 先按顺序问一遍，遇到取消就整体中止（避免"关了一半"的中间态）
    const std::vector<core::BufferId> order = documents_->TabOrder();
    for (const core::BufferId id : order) {
        if (id == keep) {
            continue;
        }
        edit::Document* document = documents_->Find(id);
        if (document != nullptr && !ConfirmSaveIfModified(*document)) {
            return false;
        }
    }
    for (const core::BufferId id : order) {
        if (id != keep) {
            CloseDocument(id, true);
        }
    }
    EnsureAtLeastOneDocument();
    return true;
}

bool MainWindow::CloseRightOf(core::BufferId anchor) {
    const std::vector<core::BufferId> order = documents_->TabOrder();
    const auto position = std::find(order.begin(), order.end(), anchor);
    if (position == order.end()) {
        return true;
    }
    std::vector<core::BufferId> targets(position + 1, order.end());
    for (const core::BufferId id : targets) {
        edit::Document* document = documents_->Find(id);
        if (document != nullptr && !ConfirmSaveIfModified(*document)) {
            return false;
        }
    }
    for (const core::BufferId id : targets) {
        CloseDocument(id, true);
    }
    EnsureAtLeastOneDocument();
    return true;
}

bool MainWindow::CloseAllDocuments() {
    const std::vector<core::BufferId> order = documents_->TabOrder();
    for (const core::BufferId id : order) {
        edit::Document* document = documents_->Find(id);
        if (document != nullptr && !ConfirmSaveIfModified(*document)) {
            return false;
        }
    }
    for (const core::BufferId id : order) {
        CloseDocument(id, true);
    }
    return true;
}

void MainWindow::HandleTabBarNotify(const TabBarNotify& notification) {
    switch (notification.action) {
        case TabAction::Activate:
            ActivateDocument(notification.source);
            break;
        case TabAction::Close:
            if (CloseDocument(notification.source, false)) {
                EnsureAtLeastOneDocument();
            }
            break;
        case TabAction::CloseOthers:
            CloseOthers(notification.source);
            break;
        case TabAction::CloseRight:
            CloseRightOf(notification.source);
            break;
        case TabAction::CloseAll:
            if (CloseAllDocuments()) {
                EnsureAtLeastOneDocument();
            }
            break;
        case TabAction::CopyPath:
            if (edit::Document* document = documents_->Find(notification.source);
                document != nullptr && document->buffer.HasPath()) {
                CopyTextToClipboard(hwnd_, document->buffer.filePath);
            }
            break;
        case TabAction::Reorder:
            documents_->MoveBefore(notification.source, notification.target);
            SyncTabBar();
            break;
    }
}

void MainWindow::HandleEditorNotify(const NMHDR& header) {
    edit::EditorNotification note;
    if (!edit::EditView::TranslateNotify(&header, note)) {
        return;
    }
    edit::Document* document = DocumentOfView(header.hwndFrom);
    if (document == nullptr) {
        return;
    }

    // 关键：修改标记记在"发出通知的那个文档"上，而不是当前活动文档。
    // 打开文件时会先给隐藏的控件 SetText，此时活动文档还不是它。
    switch (note.event) {
        case edit::EditorEvent::SavePointLeft:
            document->buffer.isModified = true;
            break;
        case edit::EditorEvent::SavePointReached:
            document->buffer.isModified = false;
            break;
        case edit::EditorEvent::UpdateUI:
            if (document == documents_->Active()) {
                UpdateStatus();
            }
            return;
        default:
            return;
    }

    SyncTabBar();
    if (document == documents_->Active()) {
        UpdateTitle();
        UpdateStatus();
    }
}

void MainWindow::CommandNew() {
    if (edit::Document* document = documents_->CreateUntitled(); document != nullptr) {
        SyncTabBar();
        ActivateDocument(document->buffer.id);
    }
}

void MainWindow::CommandOpen() {
    std::vector<wchar_t> buffer(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter =
        L"文本与源码文件\0*.txt;*.log;*.md;*.ini;*.json;*.xml;*.c;*.h;*.cpp;*.hpp;*.py;*.js;*.css;*.html;*.sql\0"
        L"所有文件\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    if (!lastDirectory_.empty()) {
        dialog.lpstrInitialDir = lastDirectory_.c_str();
    }
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (::GetOpenFileNameW(&dialog) == 0) {
        return;
    }

    // OFN_ALLOWMULTISELECT 时缓冲区是 "目录\0文件1\0文件2\0\0"；只有一个文件时是完整路径
    const wchar_t* first = buffer.data();
    const wchar_t* second = first + std::wcslen(first) + 1;
    if (*second == L'\0') {
        const core::OpenOutcome outcome = documents_->Open(first);
        if (outcome.status != core::OpenStatus::Ok &&
            outcome.status != core::OpenStatus::AlreadyOpen) {
            ::MessageBoxW(hwnd_, outcome.message.c_str(), L"打开文件失败", MB_ICONWARNING);
        } else {
            lastDirectory_ = core::DocumentManager::DirectoryOf(first);
        }
    } else {
        const std::wstring directory = first;
        for (const wchar_t* name = second; *name != L'\0'; name += std::wcslen(name) + 1) {
            const std::wstring full = directory + L"\\" + name;
            const core::OpenOutcome outcome = documents_->Open(full);
            if (outcome.status != core::OpenStatus::Ok &&
                outcome.status != core::OpenStatus::AlreadyOpen) {
                ::MessageBoxW(hwnd_, outcome.message.c_str(), L"打开文件失败", MB_ICONWARNING);
            }
        }
        lastDirectory_ = directory;
    }

    SyncTabBar();
    ActivateDocument(documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0);
}

bool MainWindow::CommandSave() {
    if (edit::Document* document = documents_->Active(); document != nullptr) {
        return SaveDocument(*document);
    }
    return false;
}

bool MainWindow::CommandSaveAs() {
    if (edit::Document* document = documents_->Active(); document != nullptr) {
        return SaveDocumentAs(*document);
    }
    return false;
}

bool MainWindow::CommandCloseCurrent() {
    if (edit::Document* document = documents_->Active(); document != nullptr) {
        const core::BufferId id = document->buffer.id;
        if (!CloseDocument(id, false)) {
            return false;
        }
        EnsureAtLeastOneDocument();
    }
    return true;
}

bool MainWindow::CommandCloseAll() {
    if (!CloseAllDocuments()) {
        return false;
    }
    EnsureAtLeastOneDocument();
    return true;
}

void MainWindow::CommandNextTab() {
    if (!documents_ || documents_->TabOrder().empty()) {
        return;
    }
    const std::vector<core::BufferId>& order = documents_->TabOrder();
    const core::BufferId current =
        documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0;
    const auto position = std::find(order.begin(), order.end(), current);
    const size_t next = (position == order.end() || position + 1 == order.end())
                            ? 0
                            : static_cast<size_t>(std::distance(order.begin(), position)) + 1;
    ActivateDocument(order[next]);
}

void MainWindow::CommandPrevTab() {
    if (!documents_ || documents_->TabOrder().empty()) {
        return;
    }
    const std::vector<core::BufferId>& order = documents_->TabOrder();
    const core::BufferId current =
        documents_->Active() != nullptr ? documents_->Active()->buffer.id : 0;
    const auto position = std::find(order.begin(), order.end(), current);
    size_t previous = order.size() - 1;
    if (position != order.end()) {
        const size_t index = static_cast<size_t>(std::distance(order.begin(), position));
        previous = (index == 0) ? order.size() - 1 : index - 1;
    }
    ActivateDocument(order[previous]);
}

void MainWindow::CommandAbout() {
    ::MessageBoxW(hwnd_,
                  L"notepadcc 1.0.0（M0 + 多标签）\n\n"
                  L"原生 Win32 + Scintilla 的轻量文本 / 源码编辑器。\n"
                  L"已完成：多标签与标签栏（拖拽排序、中键关闭、右键菜单、脏标记）、"
                  L"多编码打开与保存、原子写盘、语法着色、命令注册表与可重绑快捷键。",
                  L"关于 notepadcc", MB_ICONINFORMATION);
}

}  // namespace npcc::shell
