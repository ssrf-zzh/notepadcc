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
#include "core/Messages.h"
#include "core/SessionManager.h"
#include "edit/ColumnEdit.h"
#include "edit/ColumnText.h"
#include "search/ByteMatcher.h"
#include "shell/MenuBuilder.h"
#include "shell/TabBar.h"
#include "shell/ColumnDialog.h"
#include "shell/PreferencesDialog.h"
#include "shell/ShortcutDialog.h"
#include "core/ShortcutStore.h"

namespace npcc::shell {
namespace {

// 命令 ID：高 4 位类别 + 低 12 位序号（见 core/Ids.h 的 MakeCmdId）
// 定时器 ID
constexpr UINT_PTR kTimerAutosave = 1;
constexpr UINT_PTR kTimerCrashTest = 2;

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
    kCmdEditColumnEdit = npcc::core::MakeCmdId(2, 7),
    kCmdEditToggleOverwrite = npcc::core::MakeCmdId(2, 8),

    kCmdSearchFind     = npcc::core::MakeCmdId(3, 1),
    kCmdSearchReplace  = npcc::core::MakeCmdId(3, 2),
    kCmdSearchFindNext = npcc::core::MakeCmdId(3, 3),
    kCmdSearchFindPrev = npcc::core::MakeCmdId(3, 4),
    kCmdSearchInFiles  = npcc::core::MakeCmdId(3, 5),
    kCmdSearchToggleBookmark = npcc::core::MakeCmdId(3, 6),
    kCmdSearchNextBookmark   = npcc::core::MakeCmdId(3, 7),
    kCmdSearchPrevBookmark   = npcc::core::MakeCmdId(3, 8),
    kCmdSearchClearBookmarks = npcc::core::MakeCmdId(3, 9),
    kCmdSearchReplaceInFiles = npcc::core::MakeCmdId(3, 10),

    kCmdViewZoomIn        = npcc::core::MakeCmdId(4, 1),
    kCmdViewZoomOut       = npcc::core::MakeCmdId(4, 2),
    kCmdViewZoomReset     = npcc::core::MakeCmdId(4, 3),
    kCmdViewToggleWrap    = npcc::core::MakeCmdId(4, 4),
    kCmdViewToggleSpaces  = npcc::core::MakeCmdId(4, 5),
    kCmdViewToggleEol     = npcc::core::MakeCmdId(4, 6),
    kCmdViewToggleNumbers = npcc::core::MakeCmdId(4, 7),

    kCmdWindowNext = npcc::core::MakeCmdId(11, 1),
    kCmdWindowPrev = npcc::core::MakeCmdId(11, 2),

    kCmdSettingsPreferences = npcc::core::MakeCmdId(7, 1),
    kCmdSettingsShortcuts   = npcc::core::MakeCmdId(7, 2),

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
    // 先停掉监视线程：它的回调会往本窗口投消息，窗口没了就不该再跑
    fileWatcher_.reset();
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
            // 配置要在建文档之前加载：新建文档的默认编码与行尾来自它
            std::wstring configError;
            if (!config_.Load(&configError)) {
                infra::LogWarn(L"config", L"配置加载失败，改用默认值: " + configError);
            }

            tabBar_.Create(hwnd_, 1);
            statusBar_.Create(hwnd_, 2);
            findBar_.Create(hwnd_);
            searchPanel_.Create(hwnd_);
            documents_ = std::make_unique<core::DocumentManager>(hwnd_);
            ApplyConfigToRuntime();
            crossSearch_ = std::make_unique<search::CrossFileSearch>();
            crossReplace_ = std::make_unique<search::CrossFileReplace>();
            // 外部改动监视：回调在监视线程上，转发回 UI 线程处理
            fileWatcher_ = std::make_unique<io::FileWatcher>();
            fileWatcher_->SetCallback([this](const std::wstring& path, io::FileChangeKind kind) {
                auto* copy = new std::pair<std::wstring, io::FileChangeKind>(path, kind);
                if (::PostMessageW(hwnd_, core::kMsgFileChanged, 0,
                                   reinterpret_cast<LPARAM>(copy)) == 0) {
                    delete copy;
                }
            });

            RegisterCommands();
            // 默认方案先留一份（"恢复默认"要用），随后让 shortcuts.xml 覆盖它
            defaultShortcuts_ = registry_.AllBindings();
            std::wstring shortcutError;
            if (!core::ShortcutStore().Load(registry_, &shortcutError)) {
                infra::LogWarn(L"shortcut", L"快捷键配置加载失败，改用默认方案: " + shortcutError);
            }
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

            // 崩溃抢救槽位：数组在这里一次性分配好，之后只改内容，不再动指针
            rescueDocs_ = std::make_unique<infra::crashguard::RescueDocument[]>(
                infra::crashguard::kMaxRescueDocuments);
            infra::crashguard::RescueProvider provider;
            provider.lengthOf = &MainWindow::RescueLengthOf;
            provider.copyInto = &MainWindow::RescueCopyInto;
            provider.context = this;
            infra::crashguard::SetDocuments(rescueDocs_.get(),
                                            infra::crashguard::kMaxRescueDocuments, provider);
            RefreshRescueSlots();

            ScheduleAutosave();
            if (crashTest_) {
                // 自检开关：给脚本留几秒钟制造"未保存的改动"，然后再崩
                ::SetTimer(hwnd_, kTimerCrashTest, 6000, nullptr);
            }
            // 恢复提示必须放在窗口显示之后：WM_CREATE 里弹模态框会把启动卡住，
            // 用户看到的先是对话框、后是主窗口，体验很怪。
            // 答复之前不许写快照，否则会把上一轮没来得及恢复的内容直接覆盖掉。
            recoveryPending_ = true;
            ::PostMessageW(hwnd_, core::kMsgStartupRecovery, 0, 0);
            return 0;
        }
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_SETFOCUS: {
            if (edit::Document* document = documents_->Active(); document != nullptr && document->view) {
                ::SetFocus(document->view->Handle());
            }
            // 窗口重新获得焦点时也做一次兜底检查
            CheckActiveDocumentForExternalChange();
            return 0;
        }
        case WM_COMMAND: {
            const int controlId = LOWORD(wParam);
            // 查找栏的控件事件（按钮点击的通知码为 0，输入框内容变化时非 0）
            if (FindBar::IsFindBarControl(controlId) && HIWORD(wParam) == 0) {
                HandleFindBarCommand(controlId);
                return 0;
            }
            if (SearchPanel::IsSearchPanelControl(controlId) && HIWORD(wParam) == 0) {
                HandleSearchPanelCommand(controlId);
                return 0;
            }
            const auto id = static_cast<core::CmdId>(controlId);
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
        // 后台搜索通过消息把结果递回 UI 线程；指针的所有权一并转移过来
        case core::kMsgSearchResult: {
            std::unique_ptr<search::FileSearchResult> result(
                reinterpret_cast<search::FileSearchResult*>(lParam));
            searchPanel_.AppendFileResult(*result);
            return 0;
        }
        case core::kMsgSearchWakeup:
            UpdateSearchProgress();
            return 0;
        case core::kMsgSearchFinished: {
            std::unique_ptr<search::SearchProgress> progress(
                reinterpret_cast<search::SearchProgress*>(lParam));
            FinishFileSearch(*progress);
            return 0;
        }
        case core::kMsgReplacePlan: {
            std::unique_ptr<search::FileReplacePlan> plan(
                reinterpret_cast<search::FileReplacePlan*>(lParam));
            searchPanel_.AppendReplacePlan(*plan);
            return 0;
        }
        case core::kMsgReplaceFinished: {
            std::unique_ptr<search::SearchProgress> progress(
                reinterpret_cast<search::SearchProgress*>(lParam));
            searchPanel_.SetRunning(false);
            searchPanel_.EndReplacePreview();
            std::wstring text = progress->cancelled ? L"已取消：" : L"预览就绪：";
            text += L"扫描 " + std::to_wstring(progress->filesScanned) + L" / " +
                    std::to_wstring(progress->filesTotal) + L" 个文件，将改动 " +
                    std::to_wstring(progress->filesMatched) + L" 个文件、共 " +
                    std::to_wstring(progress->hits) + L" 处";
            if (progress->failedFiles > 0) {
                text += L"，跳过 " + std::to_wstring(progress->failedFiles) + L" 个";
            }
            text += L"。核对无误后点「应用」。";
            searchPanel_.SetStatus(text);
            infra::LogInfo(L"replace", text);
            replaceJob_.reset();
            return 0;
        }
        case core::kMsgFileChanged: {
            std::unique_ptr<std::pair<std::wstring, io::FileChangeKind>> change(
                reinterpret_cast<std::pair<std::wstring, io::FileChangeKind>*>(lParam));
            HandleExternalChange(change->first, change->second);
            return 0;
        }
        case core::kMsgStartupRecovery:
            CheckStartupRecovery();
            return 0;
        case WM_TIMER:
            if (wParam == kTimerAutosave) {
                WriteSnapshots();
                return 0;
            }
            if (wParam == kTimerCrashTest) {
                ::KillTimer(hwnd_, kTimerCrashTest);
                infra::crashguard::RaiseTestCrash();   // 自检开关：这里之后不会再返回
                return 0;
            }
            break;
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
            // 关窗前先让后台搜索停下来，否则回调会在窗口销毁后打到已释放的对象上
            if (searchJob_) {
                searchJob_->Cancel();
                searchJob_->Wait();
                searchJob_.reset();
            }
            // 会话要在关闭文档之前取快照——CloseAllDocuments 之后就没得记了
            const core::SessionSnapshot snapshot = BuildSessionSnapshot();
            if (CloseAllDocuments()) {
                // 正常退出：未保存的内容已经逐一问过了，快照不该留到下次启动再问一遍
                ::KillTimer(hwnd_, kTimerAutosave);
                snapshots_.RemoveAllAutosave();
                config_.Flush();   // 退出前把配置落盘
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
    add(kCmdEditColumnEdit, CmdCategory::Edit, L"cmd.edit.columnEdit",
        L"列块编辑(&C)...", needsBuffer,
        [this](const CmdContext&) { CommandColumnEdit(); });
    add(kCmdEditToggleOverwrite, CmdCategory::Edit, L"cmd.edit.toggleOverwrite",
        L"插入/改写模式(&V)", needsBuffer,
        [this](const CmdContext&) { CommandToggleOverwrite(); });

    // ---- 搜索 ----
    add(kCmdSearchFind, CmdCategory::Search, L"cmd.search.find", L"查找(&F)...",
        needsBuffer, [this](const CmdContext&) { CommandFind(false); });
    add(kCmdSearchReplace, CmdCategory::Search, L"cmd.search.replace", L"替换(&H)...",
        needsBuffer, [this](const CmdContext&) { CommandFind(true); });
    add(kCmdSearchFindNext, CmdCategory::Search, L"cmd.search.findNext", L"查找下一个(&N)",
        needsBuffer, [this](const CmdContext&) { CommandFindNext(true); });
    add(kCmdSearchFindPrev, CmdCategory::Search, L"cmd.search.findPrev", L"查找上一个(&P)",
        needsBuffer, [this](const CmdContext&) { CommandFindNext(false); });
    add(kCmdSearchInFiles, CmdCategory::Search, L"cmd.search.inFiles",
        L"在文件中查找(&I)...", core::kCmdNone,
        [this](const CmdContext&) { CommandFindInFiles(); });
    add(kCmdSearchToggleBookmark, CmdCategory::Search, L"cmd.search.toggleBookmark",
        L"标记/取消书签(&B)", needsBuffer,
        [this](const CmdContext&) { CommandToggleBookmark(); });
    add(kCmdSearchNextBookmark, CmdCategory::Search, L"cmd.search.nextBookmark",
        L"下一个书签(&N)", needsBuffer,
        [this](const CmdContext&) { CommandGotoBookmark(true); });
    add(kCmdSearchPrevBookmark, CmdCategory::Search, L"cmd.search.prevBookmark",
        L"上一个书签(&P)", needsBuffer,
        [this](const CmdContext&) { CommandGotoBookmark(false); });
    add(kCmdSearchClearBookmarks, CmdCategory::Search, L"cmd.search.clearBookmarks",
        L"清除全部书签(&C)", needsBuffer,
        [this](const CmdContext&) { CommandClearBookmarks(); });
    add(kCmdSearchReplaceInFiles, CmdCategory::Search, L"cmd.search.replaceInFiles",
        L"在文件中替换(&R)...", core::kCmdNone,
        [this](const CmdContext&) { CommandReplaceInFiles(); });

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
            config_.Edit().wrapLongLines = wrapEnabled_;
            config_.RequestSave();
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->SetWrapMode(wrapEnabled_);
            }
        });
    add(kCmdViewToggleSpaces, CmdCategory::View, L"cmd.view.toggleSpaces", L"显示空白符号(&S)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showWhitespace_ = !showWhitespace_;
            config_.Edit().showWhitespace = showWhitespace_;
            config_.RequestSave();
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->SetShowWhitespace(showWhitespace_);
            }
        });
    add(kCmdViewToggleEol, CmdCategory::View, L"cmd.view.toggleEol", L"显示行尾符(&E)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showEol_ = !showEol_;
            config_.Edit().showEol = showEol_;
            config_.RequestSave();
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->SetShowEol(showEol_);
            }
        });
    add(kCmdViewToggleNumbers, CmdCategory::View, L"cmd.view.toggleNumbers", L"显示行号(&N)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showLineNumbers_ = !showLineNumbers_;
            config_.Edit().showLineNumbers = showLineNumbers_;
            config_.RequestSave();
            if (edit::Document* document = documents_->Active(); document != nullptr) {
                document->view->ShowLineNumbers(showLineNumbers_);
            }
        });

    // ---- 窗口 ----
    add(kCmdWindowNext, CmdCategory::Window, L"cmd.window.next", L"下一个标签(&N)",
        core::kCmdNone, [this](const CmdContext&) { CommandNextTab(); });
    add(kCmdWindowPrev, CmdCategory::Window, L"cmd.window.prev", L"上一个标签(&P)",
        core::kCmdNone, [this](const CmdContext&) { CommandPrevTab(); });

    // ---- 设置 ----
    add(kCmdSettingsPreferences, CmdCategory::Settings, L"cmd.settings.preferences",
        L"首选项(&P)...", core::kCmdNone,
        [this](const CmdContext&) { CommandPreferences(); });
    add(kCmdSettingsShortcuts, CmdCategory::Settings, L"cmd.settings.shortcuts",
        L"管理快捷键(&K)...", core::kCmdNone,
        [this](const CmdContext&) { CommandManageShortcuts(); });

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
    registry_.BindShortcut(kCmdEditColumnEdit, core::kModAlt, 'C', true);
    registry_.BindShortcut(kCmdEditToggleOverwrite, core::kModNone, VK_INSERT, true);
    registry_.BindShortcut(kCmdSearchFind, ctrl, 'F', true);
    registry_.BindShortcut(kCmdSearchReplace, ctrl, 'H', true);
    registry_.BindShortcut(kCmdSearchFindNext, core::kModNone, VK_F3, true);
    registry_.BindShortcut(kCmdSearchFindPrev, core::kModShift, VK_F3, true);
    registry_.BindShortcut(kCmdSearchInFiles, ctrlShift, 'F', true);
    registry_.BindShortcut(kCmdSearchToggleBookmark, ctrl, VK_F2, true);
    registry_.BindShortcut(kCmdSearchNextBookmark, core::kModNone, VK_F2, true);
    registry_.BindShortcut(kCmdSearchPrevBookmark, core::kModShift, VK_F2, true);
    registry_.BindShortcut(kCmdSearchReplaceInFiles, ctrlShift, 'H', true);
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
    registry_.BindShortcut(kCmdSettingsPreferences, ctrl, VK_OEM_COMMA, true);
}

void MainWindow::RebuildMenu() {
    // 重绑快捷键之后菜单里的提示列会变，所以要整份重建。
    // 先建新的再换上去，中途失败也不至于把菜单弄没。
    HMENU rebuilt = BuildMainMenu(registry_);
    if (rebuilt == nullptr) {
        return;
    }
    HMENU previous = menu_;
    menu_ = rebuilt;
    ::SetMenu(hwnd_, menu_);
    ::DrawMenuBar(hwnd_);
    if (previous != nullptr) {
        ::DestroyMenu(previous);
    }
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

    int bottomInset = statusBar_.Height();
    if (searchPanel_.Visible() && searchPanel_.Handle() != nullptr) {
        const int panelHeight = searchPanel_.Height();
        ::MoveWindow(searchPanel_.Handle(), 0, client.bottom - bottomInset - panelHeight,
                     client.right, panelHeight, TRUE);
        bottomInset += panelHeight;
    }
    if (findBar_.Visible() && findBar_.Handle() != nullptr) {
        const int findHeight = findBar_.Height();
        ::MoveWindow(findBar_.Handle(), 0, client.bottom - bottomInset - findHeight,
                     client.right, findHeight, TRUE);
        bottomInset += findHeight;
    }

    RECT viewRect{};
    viewRect.left = 0;
    viewRect.top = tabHeight;
    viewRect.right = client.right;
    viewRect.bottom = client.bottom - bottomInset;
    if (viewRect.bottom < viewRect.top) {
        viewRect.bottom = viewRect.top;
    }
    documents_->ResizeActive(viewRect);
}

void MainWindow::ApplyConfigToRuntime() {
    const core::EditConfig& edit = config_.Edit();
    showWhitespace_ = edit.showWhitespace;
    showEol_ = edit.showEol;
    showLineNumbers_ = edit.showLineNumbers;
    wrapEnabled_ = edit.wrapLongLines;

    if (documents_) {
        const core::LargeFileConfig& large = config_.LargeFile();
        documents_->TierPolicyRef().normalMaxBytes = large.tier1MaxBytes;
        documents_->TierPolicyRef().largeMaxBytes = large.tier2MaxBytes;
        documents_->TierPolicyRef().refuseAboveBytes = large.refuseAboveBytes;

        const core::NewFileConfig& newFile = config_.NewFile();
        documents_->SetNewFileDefaults(newFile.defaultUniMode, newFile.defaultEol,
                                       newFile.defaultCodePage);
    }
}

// ---------------------------------------------------------------- 未保存内容的快照
//
// 两条防线，各管一半：
//   * 定时快照：对付"进程被强杀 / 断电 / 蓝屏"——这类情况下没有任何代码有机会跑。
//   * 崩溃抢救：进程还活着但要走异常退出流程时，把所有未保存内容抢救到 crash 目录。
// 只有两条都在，才不会出现"改了半小时，一次强杀全没了"。

namespace {

std::uint64_t HashOf(std::string_view bytes) {
    std::uint64_t hash = 1469598103934665603ull;   // FNV-1a 64
    for (const char ch : bytes) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

// 定长缓冲的宽字符拷贝：超出就截断，绝不越界。
// 不用 wcsncpy_s + _TRUNCATE，是因为各家 CRT 对它的支持不一致。
void CopyWideInto(wchar_t* destination, std::size_t capacity, const std::wstring& source) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const std::size_t count = std::min(source.size(), capacity - 1);
    std::memcpy(destination, source.c_str(), count * sizeof(wchar_t));
    destination[count] = L'\0';
}

// 窄字符版本（编码 / 行尾标识，纯 ASCII）
void CopyInto(char* destination, std::size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    std::size_t count = 0;
    while (source != nullptr && source[count] != '\0' && count + 1 < capacity) {
        destination[count] = source[count];
        ++count;
    }
    destination[count] = '\0';
}

}  // namespace

void MainWindow::ScheduleAutosave() {
    if (hwnd_ == nullptr) {
        return;
    }
    ::KillTimer(hwnd_, kTimerAutosave);
    if (!config_.Backup().enabled) {
        // 关掉定时快照就顺手清干净：否则下次启动还会拿旧快照来问，
        // 那与"我已经关掉了这个功能"的预期相反。
        snapshots_.RemoveAllAutosave();
        snapshotStates_.clear();
        return;
    }
    const std::uint32_t seconds = std::max<std::uint32_t>(5, config_.Backup().intervalSeconds);
    ::SetTimer(hwnd_, kTimerAutosave, seconds * 1000, nullptr);
}

void MainWindow::RemoveSnapshotFor(core::BufferId id) {
    core::SnapshotInfo info;
    info.file = core::SnapshotStore::AutosaveDir() / (L"doc_" + std::to_wstring(id) + L".snap");
    snapshots_.Remove(info);
    snapshotStates_.erase(
        std::remove_if(snapshotStates_.begin(), snapshotStates_.end(),
                       [id](const SnapshotState& state) { return state.id == id; }),
        snapshotStates_.end());
}

void MainWindow::WriteSnapshots() {
    if (documents_ == nullptr || recoveryPending_) {
        return;   // 恢复提示还没答复时不能写：会把上一轮的内容直接覆盖掉
    }
    const std::uint64_t sizeLimit = config_.LargeFile().tier1MaxBytes;
    int written = 0;
    for (edit::Document* document : documents_->All()) {
        if (document == nullptr || document->view == nullptr || !document->buffer.isModified) {
            continue;
        }
        const core::BufferId id = document->buffer.id;
        auto state = std::find_if(snapshotStates_.begin(), snapshotStates_.end(),
                                  [id](const SnapshotState& s) { return s.id == id; });
        if (state == snapshotStates_.end()) {
            snapshotStates_.push_back(SnapshotState{id, 0, false});
            state = snapshotStates_.end() - 1;
        }

        const std::int32_t length = document->view->ByteLength();
        if (static_cast<std::uint64_t>(length) > sizeLimit) {
            // 大文件每 30 秒重写几十兆，代价与收益不成比例。这类文档靠退出前的保存
            // 询问与崩溃抢救兜底，这句话只说一次，免得日志被刷屏。
            if (!state->tooLargeWarned) {
                state->tooLargeWarned = true;
                infra::LogWarn(L"snapshot",
                               L"文档超过降级阈值，已跳过定时快照（仍会随崩溃抢救）: " +
                                   document->buffer.displayName);
            }
            continue;
        }

        const std::string content = document->view->GetAllText();
        const std::uint64_t hash = HashOf(content);
        if (hash == state->hash) {
            continue;   // 自上次快照以来内容没变，不必再写一次盘
        }

        core::SnapshotInfo info;
        info.file = core::SnapshotStore::AutosaveDir() /
                    (L"doc_" + std::to_wstring(id) + L".snap");
        info.originalPath = document->buffer.filePath;
        info.displayName = document->buffer.displayName;
        info.encoding = document->buffer.encoding;
        info.eol = document->buffer.eol;
        info.untitled = !document->buffer.HasPath();
        info.savedAt = core::SnapshotStore::DisplayTime();
        std::wstring error;
        if (snapshots_.Write(info, content, &error)) {
            state->hash = hash;
            ++written;
        } else {
            infra::LogWarn(L"snapshot", L"写入快照失败: " + document->buffer.displayName +
                                             L" / " + error);
        }
    }
    if (written > 0) {
        infra::LogInfo(L"snapshot", L"已为 " + std::to_wstring(written) + L" 个文档写下快照");
    }
}

void MainWindow::RefreshRescueSlots() {
    if (rescueDocs_ == nullptr || documents_ == nullptr) {
        return;
    }
    const std::vector<edit::Document*> all = documents_->All();
    for (int i = 0; i < infra::crashguard::kMaxRescueDocuments; ++i) {
        infra::crashguard::RescueDocument& slot = rescueDocs_[i];
        if (i >= static_cast<int>(all.size()) || all[i] == nullptr) {
            slot.view = nullptr;
            slot.index = -1;
            slot.name[0] = L'\0';
            slot.path[0] = L'\0';
            slot.modified = false;
            continue;
        }
        const edit::Document& document = *all[i];
        slot.view = document.view.get();   // 只有提供者会解引用它
        slot.index = i;
        slot.modified = document.buffer.isModified;
        slot.codePage = document.buffer.encoding.codePage;
        CopyInto(slot.uniMode, sizeof(slot.uniMode), edit::UniModeToken(document.buffer.encoding.uniMode));
        CopyInto(slot.eol, sizeof(slot.eol), edit::EolToken(document.buffer.eol));
        slot.hasBom = document.buffer.encoding.hasBom;
        CopyWideInto(slot.name, sizeof(slot.name) / sizeof(slot.name[0]),
                     document.buffer.displayName);
        CopyWideInto(slot.path, sizeof(slot.path) / sizeof(slot.path[0]),
                     document.buffer.filePath);
    }
}

int MainWindow::RescueLengthOf(void* context, int index) {
    auto* self = static_cast<MainWindow*>(context);
    if (self == nullptr || self->rescueDocs_ == nullptr || index < 0 ||
        index >= infra::crashguard::kMaxRescueDocuments) {
        return -1;
    }
    auto* view = static_cast<edit::EditView*>(self->rescueDocs_[index].view);
    if (view == nullptr || !view->Valid()) {
        return -1;
    }
    return static_cast<int>(view->ByteLength());
}

int MainWindow::RescueCopyInto(void* context, int index, char* buffer, int capacity) {
    auto* self = static_cast<MainWindow*>(context);
    if (self == nullptr || self->rescueDocs_ == nullptr || buffer == nullptr || index < 0 ||
        index >= infra::crashguard::kMaxRescueDocuments) {
        return -1;
    }
    auto* view = static_cast<edit::EditView*>(self->rescueDocs_[index].view);
    if (view == nullptr || !view->Valid()) {
        return -1;
    }
    return static_cast<int>(view->CopyTextTo(buffer, capacity));
}

void MainWindow::CheckStartupRecovery() {
    const std::vector<core::SnapshotInfo> found = snapshots_.List();
    if (found.empty()) {
        recoveryPending_ = false;
        snapshots_.PruneCrashDirs(config_.Backup().keepSnapshots);
        return;
    }

    std::wstring question = L"上次运行没有正常结束，发现有 " + std::to_wstring(found.size()) +
                            L" 份未保存的内容：\n\n";
    const std::size_t shown = std::min<std::size_t>(found.size(), 10);
    for (std::size_t i = 0; i < shown; ++i) {
        const core::SnapshotInfo& info = found[i];
        question += L"· " + (info.originalPath.empty() ? info.displayName : info.originalPath) +
                    L"（" + info.savedAt + L"）\n";
    }
    if (found.size() > shown) {
        question += L"…另有 " + std::to_wstring(found.size() - shown) + L" 份\n";
    }
    question += L"\n选择「是」把它们恢复成文档（内容是未保存状态，需要自己确认后保存）；"
                L"选择「否」丢弃。";

    const int answer = ::MessageBoxW(hwnd_, question.c_str(), L"恢复未保存的内容",
                                     MB_ICONWARNING | MB_YESNO);
    if (answer == IDYES) {
        for (const core::SnapshotInfo& info : found) {
            RestoreSnapshot(info);
        }
        infra::LogInfo(L"snapshot",
                       L"已恢复 " + std::to_wstring(found.size()) + L" 份未保存内容");
    } else {
        for (const core::SnapshotInfo& info : found) {
            snapshots_.Remove(info);
        }
        infra::LogInfo(L"snapshot",
                       L"用户放弃了 " + std::to_wstring(found.size()) + L" 份未保存内容");
    }
    snapshots_.PruneCrashDirs(config_.Backup().keepSnapshots);
    recoveryPending_ = false;

    SyncTabBar();
    if (documents_->Active() != nullptr) {
        ActivateDocument(documents_->Active()->buffer.id);
    }
    UpdateStatus();
}

void MainWindow::RestoreSnapshot(const core::SnapshotInfo& info) {
    std::string content;
    std::wstring error;
    if (!snapshots_.Read(info, content, &error)) {
        infra::LogWarn(L"snapshot", L"恢复失败: " + error);
        snapshots_.Remove(info);
        return;
    }

    // 原文件还在就正常打开、再把快照内容盖上去（这样路径、语言着色、书签边距
    // 全都与平时一致，只是内容是未保存的那一版）；文件没了就退回未命名文档。
    edit::Document* document = nullptr;
    const bool pathUsable =
        !info.originalPath.empty() &&
        ::GetFileAttributesW(info.originalPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (pathUsable) {
        const core::OpenOutcome outcome = documents_->Open(info.originalPath);
        if (outcome.status == core::OpenStatus::Ok ||
            outcome.status == core::OpenStatus::AlreadyOpen) {
            document = outcome.document;
        }
    }
    if (document == nullptr) {
        document = documents_->CreateUntitled();
    }
    if (document == nullptr || document->view == nullptr) {
        snapshots_.Remove(info);
        return;
    }

    document->buffer.encoding = info.encoding;
    document->buffer.eol = info.eol;
    document->view->SetCodePage(info.encoding.uniMode == edit::UniMode::Ansi
                                    ? info.encoding.codePage
                                    : edit::kCodePageUtf8);
    document->view->SetEolMode(info.eol);
    document->view->SetText(content);
    if (!pathUsable && !info.displayName.empty()) {
        document->buffer.displayName = info.displayName;
    }
    // 恢复出来的内容本来就没存过盘：不要 MarkSavePoint，让"已修改"标记留着，
    // 否则用户可能以为已经保存好了，直接关窗口就把内容丢了。
    document->buffer.isModified = true;
    snapshots_.Remove(info);   // 摘掉这份快照本身（文件名不一定与当前文档 id 对应）
    infra::LogInfo(L"snapshot", L"已恢复: " + document->buffer.displayName);
}

void MainWindow::ApplyEditConfigToAllViews() const {
    if (!documents_) {
        return;
    }
    for (edit::Document* document : documents_->All()) {
        if (document->view == nullptr) {
            continue;
        }
        ApplyEditConfigToView(*document->view);
    }
}

void MainWindow::ApplyEditConfigToView(edit::EditView& view) const {
    const core::EditConfig& edit = config_.Edit();
    view.SetTabWidth(static_cast<int32_t>(edit.tabWidth), !edit.tabToSpaces);
    view.SetShowWhitespace(edit.showWhitespace);
    view.SetShowEol(edit.showEol);
    view.ShowLineNumbers(edit.showLineNumbers);
    view.SetWrapMode(edit.wrapLongLines);
    view.SetIndentGuides(edit.showIndentGuide);
    view.SetCaretLineVisible(edit.caretLineHighlight);
    view.SetZoom(static_cast<int32_t>(edit.zoomLevel));
}

void MainWindow::ApplyViewToggles(edit::Document& document) const {
    // 这些是控件级设置，新激活的视图需要按当前配置复现。
    // 注意大文件档另有降级项：直接照搬配置会把降级覆盖掉，
    // 所以常规档才照配置来，降级档在配置之上再叠一层降级。
    if (document.view == nullptr) {
        return;
    }
    switch (document.buffer.tier) {
        case edit::FileTier::Normal:
            ApplyEditConfigToView(*document.view);
            break;
        case edit::FileTier::Large:
            ApplyEditConfigToView(*document.view);
            document.view->ApplyLargeFileDisplayMode();
            break;
        case edit::FileTier::Huge:
            ApplyEditConfigToView(*document.view);
            document.view->ApplyLargeFileDisplayMode();
            document.view->SetReadOnly(true);
            document.view->SetUndoCollection(false);
            break;
    }
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
    // 标签集合变了，被监视的文件集合也跟着变
    SyncFileWatcher();
    // 崩溃抢救的槽位也要跟着变，否则崩了会抢救错文档
    RefreshRescueSlots();
}

void MainWindow::SyncFileWatcher() {
    if (!fileWatcher_ || !documents_) {
        return;
    }
    // 整体重建比增量增删可靠得多：任何一条打开/关闭/另存为的路径漏掉
    // 增删都会导致监视集与标签集不一致，而这种不一致很难被发现。
    fileWatcher_->Clear();
    for (edit::Document* document : documents_->All()) {
        if (document->buffer.HasPath() && !document->buffer.isUntitled) {
            fileWatcher_->Watch(document->buffer.filePath);
        }
    }
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
                           document->view->SelectionByteCount(),
                           document->view->OverwriteMode());
    std::wstring language = document->view->CurrentLanguageName();
    if (document->buffer.tier != edit::FileTier::Normal) {
        language += L"  |  ";
        language += edit::TierName(document->buffer.tier);
    }
    statusBar_.SetDocumentInfo(edit::EncodingName(document->buffer.encoding),
                               edit::EolName(document->buffer.eol), language,
                               document->buffer.isModified);
}

void MainWindow::ActivateDocument(core::BufferId id) {
    if (!documents_) {
        return;
    }
    documents_->Activate(id);
    if (edit::Document* document = documents_->Active(); document != nullptr && document->view) {
        ApplyViewToggles(*document);
        ::SetFocus(document->view->Handle());
    }
    tabBar_.SetActive(id);
    LayoutChildren();
    UpdateTitle();
    UpdateStatus();
    // 兜底检查：目录通知可能因为缓冲区溢出而漏掉，切到某个标签时再确认一次
    CheckActiveDocumentForExternalChange();
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
        entry.bookmarks = document->view->AllBookmarkLines();
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
        for (const std::int32_t line : entry.bookmarks) {
            document->view->AddBookmark(line);
        }
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
    RemoveSnapshotFor(document.buffer.id);   // 内容已经落盘，快照没用了
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
    RemoveSnapshotFor(id);   // 文档关掉了（无论存没存），它的快照都不该再被拿出来问
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
            // 崩溃抢救只看"有没有未保存的改动"，所以这一刻必须同步刷新槽位：
            // 否则刚打完字就崩，抢救会以为这个文档是干净的，直接跳过。
            RefreshRescueSlots();
            break;
        case edit::EditorEvent::SavePointReached:
            document->buffer.isModified = false;
            RefreshRescueSlots();
            break;
        case edit::EditorEvent::UpdateUI:
            if (document == documents_->Active()) {
                UpdateStatus();
            }
            return;
        case edit::EditorEvent::MarginClick:
            // 点击书签边距直接切换该行的书签，比"先把光标移过去再按键"顺手
            if (note.margin == edit::EditView::kMarginBookmark && note.line >= 0) {
                document->view->ToggleBookmark(note.line);
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

edit::Document* MainWindow::ActiveDocument() const {
    return documents_ ? documents_->Active() : nullptr;
}

search::SearchConfig MainWindow::CurrentSearchConfig() const {
    bool utfMode = true;
    std::uint32_t codePage = 936;
    if (edit::Document* document = ActiveDocument(); document != nullptr) {
        // 缓冲区是文件原始字节：UTF-8 系列走 UTF 模式，ANSI 走代码页
        utfMode = document->buffer.encoding.uniMode != edit::UniMode::Ansi;
        codePage = document->buffer.encoding.codePage;
    }
    return findBar_.Config(utfMode, codePage);
}

void MainWindow::CommandFind(bool replaceMode) {
    if (ActiveDocument() == nullptr) {
        return;
    }
    findBar_.Show(replaceMode);
    LayoutChildren();
}

void MainWindow::CommandFindNext(bool forward) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || !document->view) {
        return;
    }
    if (!findBar_.Visible()) {
        findBar_.Show(false);
        LayoutChildren();
        return;
    }

    const search::SearchConfig config = CurrentSearchConfig();
    if (config.pattern.empty()) {
        findBar_.SetStatus(L"请输入查找内容");
        return;
    }

    search::InDocSearch search(*document->view);
    search::SearchOutcome outcome;
    bool wrapped = false;
    const bool found = search.FindNext(config, forward, wrapped, outcome);
    if (!outcome.Ok()) {
        findBar_.SetStatus(outcome.message);
        return;
    }
    if (!found) {
        search.ClearMarks();
        findBar_.SetStatus(L"未找到匹配项");
        return;
    }

    int count = 0;
    search::SearchOutcome markOutcome;
    search.MarkAll(config, count, markOutcome);
    std::wstring status = L"共 " + std::to_wstring(count) + L" 处匹配";
    if (wrapped) {
        status = L"已回绕， " + status;
    }
    findBar_.SetStatus(status);
}

bool MainWindow::HandleFindBarCommand(int controlId) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || !document->view) {
        return true;
    }

    switch (controlId) {
        case kFindNextButton:
            CommandFindNext(true);
            return true;
        case kFindPrevButton:
            CommandFindNext(false);
            return true;
        case kCloseButton:
            findBar_.Hide();
            if (document->view) {
                ::SetFocus(document->view->Handle());
            }
            LayoutChildren();
            return true;
        default:
            break;
    }

    const search::SearchConfig config = CurrentSearchConfig();
    if (config.pattern.empty()) {
        findBar_.SetStatus(L"请输入查找内容");
        return true;
    }

    search::InDocSearch search(*document->view);
    search::SearchOutcome outcome;

    switch (controlId) {
        case kCountButton:
        case kMarkButton: {
            int count = 0;
            const bool mark = (controlId == kMarkButton);
            const bool ok = mark ? search.MarkAll(config, count, outcome)
                                 : search.Count(config, count, outcome);
            if (!ok) {
                findBar_.SetStatus(outcome.message);
            } else if (mark) {
                findBar_.SetStatus(L"已标记 " + std::to_wstring(count) + L" 处匹配");
            } else {
                findBar_.SetStatus(L"共 " + std::to_wstring(count) + L" 处匹配");
            }
            return true;
        }
        case kReplaceButton: {
            bool replaced = false;
            if (!search.ReplaceCurrent(config, replaced, outcome)) {
                findBar_.SetStatus(outcome.message);
                return true;
            }
            findBar_.SetStatus(replaced ? L"已替换 1 处" : L"没有可替换的匹配");
            SyncTabBar();
            UpdateStatus();
            return true;
        }
        case kReplaceAllButton: {
            // 全部替换属于不可逆的大范围修改，先问一次
            const std::wstring question =
                L"将把文档中所有匹配「" + config.pattern + L"」的内容替换为「" +
                config.replaceTo + L"」。\n\n此操作可以用一次「撤销」回退。是否继续？";
            if (::MessageBoxW(hwnd_, question.c_str(), L"全部替换",
                              MB_ICONQUESTION | MB_YESNO) != IDYES) {
                return true;
            }
            int count = 0;
            if (!search.ReplaceAll(config, count, outcome)) {
                findBar_.SetStatus(outcome.message);
                return true;
            }
            findBar_.SetStatus(L"已替换 " + std::to_wstring(count) + L" 处");
            search.ClearMarks();
            SyncTabBar();
            UpdateTitle();
            UpdateStatus();
            return true;
        }
        default:
            break;
    }
    return true;   // 输入框的其它通知先不管
}

void MainWindow::HandleExternalChange(const std::wstring& path, io::FileChangeKind kind) {
    if (!documents_) {
        return;
    }
    // 只有"当前正显示的文档"才弹窗；其余的只做标记，等切过去再说，
    // 免得后台一堆标签同时被改动时弹出一串对话框。
    edit::Document* target = nullptr;
    for (edit::Document* document : documents_->All()) {
        if (document->buffer.HasPath() &&
            ::CompareStringOrdinal(document->buffer.filePath.c_str(), -1, path.c_str(), -1,
                                   TRUE) == CSTR_EQUAL) {
            target = document;
            break;
        }
    }
    if (target == nullptr) {
        return;   // 不在我们打开的范围内（可能是别的程序在同目录里改文件）
    }
    if (target != documents_->Active()) {
        return;   // 切到该标签时由兜底检查处理
    }

    // 内容其实没变（比如只是时间戳被动过）就静默刷新记录
    if (!documents_->HasExternallyChanged(*target)) {
        documents_->RefreshIdentity(*target);
        return;
    }
    CheckActiveDocumentForExternalChange(kind);
}

void MainWindow::CheckActiveDocumentForExternalChange(io::FileChangeKind kind) {
    if (!documents_) {
        return;
    }
    edit::Document* document = documents_->Active();
    if (document == nullptr || !document->buffer.HasPath()) {
        return;
    }
    if (!documents_->HasExternallyChanged(*document)) {
        return;
    }

    // 把变更类型写进提示：被删除和被修改，用户该做的事是不一样的
    const std::wstring question =
        L"文件「" + document->buffer.displayName + L"」" + io::FileChangeKindName(kind) +
        L"。\n\n"
        L"· 选择「是」重新加载磁盘上的最新内容（编辑器里未保存的改动会丢失）；\n"
        L"· 选择「否」保留当前编辑的内容，之后保存会覆盖磁盘上的版本。\n\n"
        L"是否重新加载？";
    const int answer = ::MessageBoxW(hwnd_, question.c_str(), L"文件已被外部修改",
                                     MB_ICONWARNING | MB_YESNO);
    if (answer == IDYES) {
        std::wstring error;
        if (!documents_->Reload(*document, error)) {
            ::MessageBoxW(hwnd_, error.c_str(), L"重新加载失败", MB_ICONERROR);
            return;
        }
        infra::LogInfo(L"file", L"已重新加载: " + document->buffer.filePath);
    } else {
        // 保留当前内容：刷新记录，这样下次磁盘再变才会再问一次
        documents_->RefreshIdentity(*document);
        infra::LogInfo(L"file", L"保留当前内容（外部改动被忽略）: " + document->buffer.filePath);
    }
    SyncTabBar();
    UpdateTitle();
    UpdateStatus();
}

void MainWindow::CommandColumnEdit() {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || !document->view) {
        return;
    }
    if (!document->view->IsRectangularSelection()) {
        statusBar_.SetMessage(L"请先用 Alt+鼠标拖动选出一个矩形区域");
        return;
    }

    int32_t firstLine = 0;
    int32_t lastLine = 0;
    document->view->ColumnRange(firstLine, lastLine);
    const int lineCount = lastLine - firstLine + 1;
    if (lineCount <= 0) {
        return;
    }

    ColumnDialogResult result;
    if (!ShowColumnDialog(hwnd_, lineCount, result)) {
        return;   // 用户取消
    }

    const bool utfMode = document->buffer.encoding.uniMode != edit::UniMode::Ansi;
    if (result.deleteColumn) {
        const int removed = edit::DeleteColumnSelection(*document->view);
        statusBar_.SetMessage(L"已删除 " + std::to_wstring(removed) + L" 行的列块内容");
        infra::LogInfo(L"edit", L"列块删除 " + std::to_wstring(removed) + L" 行");
    } else if (result.apply) {
        const std::vector<std::wstring> texts = edit::BuildColumnInsertTexts(result.options);
        const int applied = edit::ApplyColumnInsert(*document->view, texts, utfMode,
                                                    document->buffer.encoding.codePage);
        statusBar_.SetMessage(L"已插入 " + std::to_wstring(applied) + L" 行");
        infra::LogInfo(L"edit", L"列块插入 " + std::to_wstring(applied) + L" 行");
    }
    // 修改标记由 Scintilla 的保存点通知驱动，这里不手工改
    SyncTabBar();
    UpdateTitle();
    UpdateStatus();
}

void MainWindow::CommandToggleOverwrite() {
    if (edit::Document* document = ActiveDocument();
        document != nullptr && document->view) {
        document->view->SetOverwriteMode(!document->view->OverwriteMode());
        statusBar_.SetMessage(document->view->OverwriteMode() ? L"改写模式" : L"插入模式");
        UpdateStatus();
    }
}

void MainWindow::CommandToggleBookmark() {
    if (edit::Document* document = ActiveDocument();
        document != nullptr && document->view) {
        document->view->ToggleBookmark(document->view->CurrentLine());
    }
}

void MainWindow::CommandGotoBookmark(bool forward) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || !document->view) {
        return;
    }
    const int32_t currentLine = document->view->CurrentLine();
    // 从当前行的下一行/上一行开始找，否则会停在原地不动
    const int32_t target = forward
                               ? document->view->NextBookmarkLine(currentLine + 1, true)
                               : document->view->PrevBookmarkLine(currentLine - 1, true);
    if (target < 0) {
        statusBar_.SetMessage(L"没有书签");
        return;
    }
    document->view->ScrollToLine(target);
    document->view->SetCaretPosition(
        static_cast<int32_t>(document->view->PositionFromLine(target)));
}

void MainWindow::CommandClearBookmarks() {
    if (edit::Document* document = ActiveDocument();
        document != nullptr && document->view) {
        document->view->ClearAllBookmarks();
        statusBar_.SetMessage(L"已清除全部书签");
    }
}

void MainWindow::CommandReplaceInFiles() {
    CommandFindInFiles();
    if (HWND edit = ::GetDlgItem(searchPanel_.Handle(), kSearchPanelReplaceEdit);
        edit != nullptr) {
        ::SetFocus(edit);
        ::SendMessageW(edit, EM_SETSEL, 0, -1);
    }
}

void MainWindow::StartReplacePlan() {
    search::SearchConfig config = searchPanel_.Config();
    if (config.pattern.empty()) {
        searchPanel_.SetStatus(L"请输入查找内容");
        return;
    }
    if (config.directories.empty()) {
        if (edit::Document* document = ActiveDocument();
            document != nullptr && document->buffer.HasPath()) {
            config.directories.push_back(
                core::DocumentManager::DirectoryOf(document->buffer.filePath));
        }
        if (config.directories.empty()) {
            searchPanel_.SetStatus(L"请指定搜索目录");
            return;
        }
    }
    if (replaceJob_) {
        replaceJob_->Cancel();
        replaceJob_->Wait();
        replaceJob_.reset();
    }

    // 模式串与替换串都先单独校验一次，明显写错的东西不该丢给后台才发现
    {
        search::ByteMatcher probe;
        search::SearchOutcome outcome;
        if (!probe.Prepare(config, true, 936, outcome)) {
            searchPanel_.SetStatus(outcome.message);
            return;
        }
    }

    pendingReplaceConfig_ = config;
    searchPanel_.BeginReplacePreview();
    searchPanel_.SetRunning(true);
    searchPanel_.SetStatus(L"正在生成替换预览…");

    search::ReplacePlanCallbacks callbacks;
    callbacks.onPlan = [this](const search::FileReplacePlan& plan) {
        auto* copy = new search::FileReplacePlan(plan);
        if (::PostMessageW(hwnd_, core::kMsgReplacePlan, 0, reinterpret_cast<LPARAM>(copy)) == 0) {
            delete copy;
        }
    };
    callbacks.onProgress = [this](const search::SearchProgress&) {
        ::PostMessageW(hwnd_, core::kMsgSearchWakeup, 0, 0);
    };
    callbacks.onFinished = [this](const search::SearchProgress& progress) {
        auto* copy = new search::SearchProgress(progress);
        if (::PostMessageW(hwnd_, core::kMsgReplaceFinished, 0,
                           reinterpret_cast<LPARAM>(copy)) == 0) {
            delete copy;
        }
    };

    infra::LogInfo(L"replace", L"生成替换预览: " + config.pattern + L" -> " + config.replaceTo);
    replaceJob_ = crossReplace_->BuildPlan(config, replaceOptions_, std::move(callbacks));
}

void MainWindow::ApplyReplacements() {
    const std::vector<search::FileReplacePlan> plans = searchPanel_.CheckedPlans();
    std::size_t fileCount = 0;
    std::size_t total = 0;
    for (const auto& plan : plans) {
        if (plan.selected) {
            ++fileCount;
            total += plan.replacements;
        }
    }
    if (fileCount == 0) {
        searchPanel_.SetStatus(L"没有勾选任何文件");
        return;
    }

    // 批量改文件是不可逆操作（虽然能靠备份与版本控制挽回），必须显式确认
    const std::wstring question =
        L"将对 " + std::to_wstring(fileCount) + L" 个文件、共 " + std::to_wstring(total) +
        L" 处执行替换。\n\n"
        L"· 每个文件都是先写临时文件再原子替换，失败不会破坏原文件；\n"
        L"· 若文件在生成预览之后被其他程序改过，该文件会被跳过；\n"
        L"· 编码无法表示替换内容的文件已自动排除。\n\n是否继续？";
    const int answer = ::MessageBoxW(hwnd_, question.c_str(), L"在文件中替换",
                                     MB_ICONWARNING | MB_YESNO);
    // 批量改文件是敏感操作，把用户的确认与否记进日志，便于事后追溯
    infra::LogInfo(L"replace", answer == IDYES ? L"用户确认执行替换" : L"用户取消了替换");
    if (answer != IDYES) {
        searchPanel_.SetStatus(L"已取消");
        return;
    }

    const search::ReplaceReport report =
        search::CrossFileReplace::Apply(pendingReplaceConfig_, replaceOptions_, plans);

    std::wstring text = L"替换完成：写回 " + std::to_wstring(report.applied) + L" 个文件、共 " +
                        std::to_wstring(report.replacements) + L" 处";
    if (report.skipped > 0) {
        text += L"，跳过 " + std::to_wstring(report.skipped) + L" 个";
    }
    if (report.failed > 0) {
        text += L"，失败 " + std::to_wstring(report.failed) + L" 个";
    }
    searchPanel_.SetStatus(text);
    infra::LogInfo(L"replace", text);

    if (!report.errors.empty()) {
        std::wstring detail = L"以下文件未能替换：\n\n";
        const std::size_t shown = std::min<std::size_t>(report.errors.size(), 10);
        for (std::size_t i = 0; i < shown; ++i) {
            detail += L"· " + report.errors[i] + L"\n";
        }
        if (report.errors.size() > shown) {
            detail += L"…另有 " + std::to_wstring(report.errors.size() - shown) + L" 个\n";
        }
        ::MessageBoxW(hwnd_, detail.c_str(), L"替换结果", MB_ICONINFORMATION);
    }

    // 只有在被替换的文件确实已经打开时，才值得打断用户提醒一次；
    // 否则替换完默默收工就好。
    if (report.applied > 0) {
        bool anyOpen = false;
        for (const auto& plan : plans) {
            if (!plan.selected) {
                continue;
            }
            for (edit::Document* document : documents_->All()) {
                if (document->buffer.HasPath() && document->buffer.filePath == plan.path) {
                    anyOpen = true;
                    break;
                }
            }
            if (anyOpen) {
                break;
            }
        }
        if (anyOpen) {
            ::MessageBoxW(hwnd_,
                          L"部分被替换的文件已在标签中打开。请重新打开这些标签，"
                          L"否则编辑器里显示的仍是旧内容。",
                          L"替换完成", MB_ICONINFORMATION);
        }
    }
    searchPanel_.BeginReplacePreview();
    searchPanel_.SetApplyEnabled(false);
}

void MainWindow::CommandFindInFiles() {
    // 查找栏与搜索面板共用底部空间，同时只显示一个
    findBar_.Hide();
    searchPanel_.Show();

    if (edit::Document* document = ActiveDocument();
        document != nullptr && document->buffer.HasPath()) {
        searchPanel_.SetDirectory(core::DocumentManager::DirectoryOf(document->buffer.filePath));
    } else if (!lastDirectory_.empty()) {
        searchPanel_.SetDirectory(lastDirectory_);
    }
    if (searchPanel_.Pattern().empty()) {
        searchPanel_.SetPattern(findBar_.Pattern());
    }
    searchPanel_.SetDefaultFilter();
    LayoutChildren();

    if (HWND edit = ::GetDlgItem(searchPanel_.Handle(), kSearchPanelFind); edit != nullptr) {
        ::SetFocus(edit);
        ::SendMessageW(edit, EM_SETSEL, 0, -1);
    }
}

void MainWindow::StartFileSearch() {
    search::SearchConfig config = searchPanel_.Config();
    if (config.pattern.empty()) {
        searchPanel_.SetStatus(L"请输入查找内容");
        return;
    }
    if (config.directories.empty()) {
        // 目录留空时回落到当前文件所在目录
        if (edit::Document* document = ActiveDocument();
            document != nullptr && document->buffer.HasPath()) {
            config.directories.push_back(
                core::DocumentManager::DirectoryOf(document->buffer.filePath));
        }
        if (config.directories.empty()) {
            searchPanel_.SetStatus(L"请指定搜索目录");
            return;
        }
    }

    if (searchJob_) {
        searchJob_->Cancel();
        searchJob_->Wait();
        searchJob_.reset();
    }

    // 模式串先单独校验一次：明显写错的正则不该丢给后台才发现
    {
        search::ByteMatcher probe;
        search::SearchOutcome outcome;
        if (!probe.Prepare(config, true, 936, outcome)) {
            searchPanel_.SetStatus(outcome.message);
            return;
        }
    }

    searchPanel_.ClearResults();
    searchPanel_.SetRunning(true);
    searchPanel_.SetStatus(L"正在搜索…");

    search::CrossSearchCallbacks callbacks;
    // 回调发生在协调线程上，这里统一把结果 PostMessage 回 UI 线程，
    // 指针所有权随之转移，由 UI 线程负责释放。
    callbacks.onFileResult = [this](const search::FileSearchResult& result) {
        auto* copy = new search::FileSearchResult(result);
        if (::PostMessageW(hwnd_, core::kMsgSearchResult, 0, reinterpret_cast<LPARAM>(copy)) == 0) {
            delete copy;
        }
    };
    callbacks.onProgress = [this](const search::SearchProgress&) {
        ::PostMessageW(hwnd_, core::kMsgSearchWakeup, 0, 0);
    };
    callbacks.onFinished = [this](const search::SearchProgress& progress) {
        auto* copy = new search::SearchProgress(progress);
        if (::PostMessageW(hwnd_, core::kMsgSearchFinished, 0,
                           reinterpret_cast<LPARAM>(copy)) == 0) {
            delete copy;
        }
    };

    infra::LogInfo(L"search", L"在文件中查找: " + config.pattern + L" @ " +
                                 config.directories.front() + L"，过滤 " + config.filter);
    searchJob_ = crossSearch_->Start(config, std::move(callbacks));
}

bool MainWindow::HandleSearchPanelCommand(int controlId) {
    switch (controlId) {
        case kSearchPanelStart:
            StartFileSearch();
            return true;
        case kSearchPanelReplacePreview:
            StartReplacePlan();
            return true;
        case kSearchPanelApply:
            ApplyReplacements();
            return true;
        case kSearchPanelStop:
            if (searchJob_) {
                searchJob_->Cancel();
                searchPanel_.SetStatus(L"正在停止…");
            }
            if (replaceJob_) {
                replaceJob_->Cancel();
                searchPanel_.SetStatus(L"正在停止…");
            }
            return true;
        case kSearchPanelClose:
            searchPanel_.Hide();
            LayoutChildren();
            if (edit::Document* document = ActiveDocument();
                document != nullptr && document->view) {
                ::SetFocus(document->view->Handle());
            }
            return true;
        case kSearchPanelList:
            JumpToSearchResult();
            return true;
        default:
            return true;   // 输入框内容变化等通知先不处理
    }
}

void MainWindow::UpdateSearchProgress() {
    if (!searchJob_) {
        return;
    }
    const search::SearchProgress progress = searchJob_->Progress();
    searchPanel_.SetStatus(L"正在搜索… " + std::to_wstring(progress.filesScanned) + L" / " +
                           std::to_wstring(progress.filesTotal) + L"，命中 " +
                           std::to_wstring(progress.hits) + L" 处");
}

void MainWindow::FinishFileSearch(const search::SearchProgress& progress) {
    searchPanel_.SetRunning(false);
    std::wstring text = progress.cancelled ? L"已取消：" : L"完成：";
    text += L"扫描 " + std::to_wstring(progress.filesScanned) + L" / " +
            std::to_wstring(progress.filesTotal) + L" 个文件，命中 " +
            std::to_wstring(progress.hits) + L" 处，涉及 " +
            std::to_wstring(progress.filesMatched) + L" 个文件";
    if (progress.skippedBinary > 0) {
        text += L"，跳过二进制 " + std::to_wstring(progress.skippedBinary) + L" 个";
    }
    if (progress.failedFiles > 0) {
        text += L"，读取失败 " + std::to_wstring(progress.failedFiles) + L" 个";
    }
    searchPanel_.SetStatus(text);
    infra::LogInfo(L"search", text);
    // 释放任务；析构里会回收协调线程（此时它已经跑完）
    searchJob_.reset();
}

void MainWindow::JumpToSearchResult() {
    std::wstring path;
    int line = 0;
    if (!searchPanel_.SelectedTarget(path, line)) {
        return;
    }
    const core::OpenOutcome outcome = documents_->Open(path);
    if (outcome.status != core::OpenStatus::Ok &&
        outcome.status != core::OpenStatus::AlreadyOpen) {
        ::MessageBoxW(hwnd_, outcome.message.c_str(), L"打开文件失败", MB_ICONWARNING);
        return;
    }
    SyncTabBar();
    ActivateDocument(outcome.document->buffer.id);
    if (outcome.document != nullptr && outcome.document->view && line > 0) {
        const int32_t index = line - 1;
        outcome.document->view->SetCaretPosition(
            static_cast<int32_t>(outcome.document->view->PositionFromLine(index)));
        outcome.document->view->ScrollToLine(index);
    }
}

void MainWindow::CommandPreferences() {
    if (!ShowPreferencesDialog(hwnd_, config_)) {
        return;   // 用户取消，配置与视图都不动
    }

    // 配置变了就把运行期状态、大文件策略、新建文档默认值和新视图开关全部重算一遍。
    ApplyConfigToRuntime();
    if (documents_ != nullptr) {
        for (edit::Document* document : documents_->All()) {
            if (document != nullptr) {
                ApplyViewToggles(*document);   // 按档位恢复，不把降级项覆盖掉
            }
        }
    }
    SyncTabBar();
    LayoutChildren();
    UpdateStatus();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    ScheduleAutosave();   // 快照开关与间隔都可能刚被改过
}

void MainWindow::CommandManageShortcuts() {
    ShortcutEditResult result;
    if (!ShowShortcutDialog(hwnd_, registry_, defaultShortcuts_, &result)) {
        return;   // 取消：对话框已经把绑定回滚了
    }
    RebuildAccelerators();
    RebuildMenu();
    if (!result.changed) {
        return;
    }
    std::wstring error;
    if (!core::ShortcutStore().Save(registry_, &error)) {
        infra::LogWarn(L"shortcut", L"快捷键保存失败: " + error);
        ::MessageBoxW(hwnd_, (L"快捷键已经生效，但写入 shortcuts.xml 失败：\n" + error).c_str(),
                      L"保存快捷键失败", MB_ICONWARNING);
    }
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
