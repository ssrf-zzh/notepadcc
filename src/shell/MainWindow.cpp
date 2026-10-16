#include "shell/MainWindow.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>   // SHChangeNotify（文件关联变更后通知资源管理器）
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

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
#include "edit/TextTools.h"
#include "core/CommandText.h"
#include "search/ByteMatcher.h"
#include "shell/MenuBuilder.h"
#include "shell/TabBar.h"
#include "shell/ColumnDialog.h"
#include "shell/PreferencesDialog.h"
#include "shell/ShortcutDialog.h"
#include "shell/StyleConfigDialog.h"
#include "shell/MacroDialog.h"
#include "core/ExportHtml.h"
#include "core/ExportRtf.h"
#include "core/JsonFormat.h"
#include "core/XmlFormat.h"
#include "core/PrintText.h"
#include "core/PrintPaging.h"
#include "shell/PluginAdminDialog.h"
#include "core/ShortcutStore.h"
#include "infra/SingleInstance.h"
#include "infra/FileAssociation.h"
#include "infra/Paths.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"

namespace npcc::shell {
namespace {

// 命令 ID：高 4 位类别 + 低 12 位序号（见 core/Ids.h 的 MakeCmdId）
// 定时器 ID
constexpr UINT_PTR kTimerAutosave = 1;
constexpr UINT_PTR kTimerCrashTest = 2;
constexpr UINT_PTR kTimerFunctionList = 3;   // 函数列表的节流定时器（一次性）

enum : npcc::core::CmdId {
    kCmdFileNew     = npcc::core::MakeCmdId(1, 1),
    kCmdFileOpen    = npcc::core::MakeCmdId(1, 2),
    kCmdFileSave    = npcc::core::MakeCmdId(1, 3),
    kCmdFileSaveAs  = npcc::core::MakeCmdId(1, 4),
    kCmdFileClose   = npcc::core::MakeCmdId(1, 5),
    kCmdFileCloseAll= npcc::core::MakeCmdId(1, 6),
    kCmdFileExit    = npcc::core::MakeCmdId(1, 7),
    kCmdFilePrint      = npcc::core::MakeCmdId(1, 8),
    kCmdFileExportHtml = npcc::core::MakeCmdId(1, 9),
    kCmdFileExportRtf  = npcc::core::MakeCmdId(1, 10),

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
    kCmdViewFoldToggle    = npcc::core::MakeCmdId(4, 8),
    kCmdViewFoldAll       = npcc::core::MakeCmdId(4, 9),
    kCmdViewUnfoldAll     = npcc::core::MakeCmdId(4, 10),
    kCmdViewSplitToggle   = npcc::core::MakeCmdId(4, 11),
    kCmdViewCyclePane     = npcc::core::MakeCmdId(4, 12),
    kCmdViewScrollSync    = npcc::core::MakeCmdId(4, 13),
    kCmdViewDarkMode      = npcc::core::MakeCmdId(4, 14),
    kCmdViewWorkspaceOpen    = npcc::core::MakeCmdId(4, 15),
    kCmdViewWorkspaceClose   = npcc::core::MakeCmdId(4, 16),
    kCmdViewWorkspaceRefresh = npcc::core::MakeCmdId(4, 17),
    kCmdViewWorkspaceOpenSel = npcc::core::MakeCmdId(4, 18),
    kCmdViewFunctionList     = npcc::core::MakeCmdId(4, 19),
    kCmdViewFunctionJump     = npcc::core::MakeCmdId(4, 20),

    // 宏（8 类）
    kCmdMacroRecord       = npcc::core::MakeCmdId(8, 1),
    kCmdMacroPlayLast     = npcc::core::MakeCmdId(8, 3),
    kCmdMacroSaveLast     = npcc::core::MakeCmdId(8, 5),
    kCmdMacroRunMultiple  = npcc::core::MakeCmdId(8, 6),

    // 格式（5 类）：行尾转换
    kCmdFormatEolCrlf = npcc::core::MakeCmdId(5, 1),
    kCmdFormatEolLf   = npcc::core::MakeCmdId(5, 2),
    kCmdFormatEolCr   = npcc::core::MakeCmdId(5, 3),

    // 编码（13 类）：转换编码
    kCmdEncodingAnsi    = npcc::core::MakeCmdId(13, 1),
    kCmdEncodingUtf8    = npcc::core::MakeCmdId(13, 2),
    kCmdEncodingUtf8Bom = npcc::core::MakeCmdId(13, 3),
    kCmdEncodingUtf16Le = npcc::core::MakeCmdId(13, 4),
    kCmdEncodingUtf16Be = npcc::core::MakeCmdId(13, 5),

    // 工具（14 类）：行操作与文本转换
    kCmdToolDuplicateLine   = npcc::core::MakeCmdId(14, 1),
    kCmdToolDeleteLine      = npcc::core::MakeCmdId(14, 2),
    kCmdToolMoveLineUp      = npcc::core::MakeCmdId(14, 3),
    kCmdToolMoveLineDown    = npcc::core::MakeCmdId(14, 4),
    kCmdToolSortAsc         = npcc::core::MakeCmdId(14, 5),
    kCmdToolSortDesc        = npcc::core::MakeCmdId(14, 6),
    kCmdToolSortCaseInsAsc  = npcc::core::MakeCmdId(14, 7),
    kCmdToolSortCaseInsDesc = npcc::core::MakeCmdId(14, 8),
    kCmdToolSortLengthAsc   = npcc::core::MakeCmdId(14, 9),
    kCmdToolSortLengthDesc  = npcc::core::MakeCmdId(14, 10),
    kCmdToolRemoveEmpty     = npcc::core::MakeCmdId(14, 11),
    kCmdToolRemoveDupFirst  = npcc::core::MakeCmdId(14, 12),
    kCmdToolRemoveDupLast   = npcc::core::MakeCmdId(14, 13),
    kCmdToolReverseLines    = npcc::core::MakeCmdId(14, 14),
    kCmdToolTrimTrailing    = npcc::core::MakeCmdId(14, 15),
    kCmdToolTrimLeading     = npcc::core::MakeCmdId(14, 16),
    kCmdToolAddLineNumbers  = npcc::core::MakeCmdId(14, 17),
    kCmdToolStripLineNumbers= npcc::core::MakeCmdId(14, 18),
    kCmdToolUpperCase       = npcc::core::MakeCmdId(14, 19),
    kCmdToolLowerCase       = npcc::core::MakeCmdId(14, 20),
    kCmdToolUrlEncode       = npcc::core::MakeCmdId(14, 21),
    kCmdToolUrlDecode       = npcc::core::MakeCmdId(14, 22),
    kCmdToolBase64Encode    = npcc::core::MakeCmdId(14, 23),
    kCmdToolBase64Decode    = npcc::core::MakeCmdId(14, 24),
    kCmdToolHtmlEscape      = npcc::core::MakeCmdId(14, 25),
    kCmdToolHtmlUnescape    = npcc::core::MakeCmdId(14, 26),
    kCmdToolToggleComment   = npcc::core::MakeCmdId(14, 27),
    kCmdToolBlockComment    = npcc::core::MakeCmdId(14, 28),
    kCmdToolExportLanguages = npcc::core::MakeCmdId(14, 29),
    kCmdToolStyleConfigurator = npcc::core::MakeCmdId(14, 30),
    kCmdToolExportUdlSample = npcc::core::MakeCmdId(14, 31),
    kCmdToolFormatJson      = npcc::core::MakeCmdId(14, 32),
    kCmdToolMinifyJson      = npcc::core::MakeCmdId(14, 33),
    kCmdToolFormatXml       = npcc::core::MakeCmdId(14, 34),
    kCmdEditAutoComplete = npcc::core::MakeCmdId(2, 33),

    kCmdWindowNext = npcc::core::MakeCmdId(11, 1),
    kCmdWindowPrev = npcc::core::MakeCmdId(11, 2),

    kCmdSettingsPreferences = npcc::core::MakeCmdId(7, 1),
    kCmdSettingsShortcuts   = npcc::core::MakeCmdId(7, 2),
    kCmdSettingsRegisterAssoc   = npcc::core::MakeCmdId(7, 3),
    kCmdSettingsUnregisterAssoc = npcc::core::MakeCmdId(7, 4),
    kCmdSettingsPlugins         = npcc::core::MakeCmdId(7, 5),

    kCmdHelpAbout = npcc::core::MakeCmdId(12, 1),

    // Scintilla 原生命令：不新增语义，直接转发
    kSciRedo      = 2011,
    kSciSelectAll = 2013,
    kSciUndo      = 2176,
    kSciCut       = 2177,
    kSciCopy      = 2178,
    kSciPaste     = 2179,
    kSciGotoLine  = 2024,   // SCI_GOTOLINE（函数列表跳转用）
    kSciReplaceSel = 2170,  // SCI_REPLACESEL（插件替换选区用）
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
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;   // 分隔条要能接双击
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
            FolderPanel::RegisterWindowClass(::GetModuleHandleW(nullptr));
            folderPanel_.Create(hwnd_, 3);
            FunctionListPanel::RegisterWindowClass(::GetModuleHandleW(nullptr));
            functionListPanel_.Create(hwnd_, 4);
            functionPool_ = std::make_unique<infra::ThreadPool>();
            findBar_.Create(hwnd_);
            searchPanel_.Create(hwnd_);
            documents_ = std::make_unique<core::DocumentManager>(hwnd_);
            // 语言定义要在打开文件之前加载好，否则第一个文件会按内置表着色
            std::wstring languageError;
            if (!languages_.Load(&languageError)) {
                infra::LogWarn(L"langs", L"语言定义加载失败，改用内置表: " + languageError);
            }
            documents_->SetLanguages(&languages_);
            std::wstring styleError;
            if (!styles_.Load(&styleError)) {
                infra::LogWarn(L"styles", L"样式方案加载失败，改用默认配色: " + styleError);
            }
            // 用哪套主题来自 config.xml；名字不认识就停在浅色（并把配置改回 light，
            // 免得每次启动都带着一个无效值）
            {
                const std::wstring& theme = config_.General().theme;
                if (!styles_.SetActiveTheme(theme)) {
                    infra::LogWarn(L"styles", L"配置里的主题名不可用，改用浅色: " + theme);
                    config_.General().theme = L"light";
                    config_.RequestSave();
                }
                darkMode_ = styles_.IsDark();
                documents_->SetPalette(darkMode_ ? edit::EditorPalette::Dark()
                                                 : edit::EditorPalette::Light());
            }
            documents_->SetStyles(&styles_);
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
            // 插件要在快捷键之前加载：插件命令也要能吃到 shortcuts.xml 里的绑定
            LoadPlugins();
            std::wstring shortcutError;
            std::vector<std::wstring> shortcutWarnings;
            if (!shortcutStore_.Load(registry_, &shortcutError, &shortcutWarnings)) {
                infra::LogWarn(L"shortcut", L"快捷键配置加载失败，改用默认方案: " + shortcutError);
            }
            // 宏也要在菜单建出来之前注册好，否则宏菜单里空空如也
            // （读文件时的警告由 ShortcutStore 自己记日志，这里不重复）
            RegisterMacroCommands();
            // 函数列表面板：开关记在配置里
            functionListVisible_ = config_.General().functionListVisible;
            functionListPanel_.SetVisible(functionListVisible_);
            // 工作区目录来自配置：目录不在了就当"没有工作区"，配置留着不动
            if (!config_.General().workspaceFolder.empty()) {
                std::error_code ec;
                if (std::filesystem::is_directory(config_.General().workspaceFolder, ec)) {
                    ApplyWorkspace(config_.General().workspaceFolder, false);
                } else {
                    infra::LogWarn(L"workspace", L"配置里的工作区目录打不开，本次不显示面板: " +
                                                    config_.General().workspaceFolder);
                }
            }
            menu_ = BuildMainMenu(registry_);
            if (menu_ != nullptr) {
                ::SetMenu(hwnd_, menu_);
            }
            RebuildAccelerators();

            ::DragAcceptFiles(hwnd_, TRUE);
            // 单实例交接：告诉它文件该往哪儿送（可能已经有排队的了）
            infra::SetSingleInstanceTarget(hwnd_);
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
        case WM_PAINT: {
            // 主窗口自己只剩分隔条要画：视图区域被两个 Scintilla 子窗口盖住了
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd_, &paint);
            if (dividerRect_.right > dividerRect_.left) {
                RECT strip = dividerRect_;
                ::FillRect(dc, &strip, reinterpret_cast<HBRUSH>(COLOR_BTNSHADOW + 1));
            }
            ::EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT point{};
            ::GetCursorPos(&point);
            ::ScreenToClient(hwnd_, &point);
            if (PointInDivider(point)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PointInDivider(point)) {
                splitDragging_ = true;
                ::SetCapture(hwnd_);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (splitDragging_) {
                DragDividerTo(GET_X_LPARAM(lParam));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (splitDragging_) {
                splitDragging_ = false;
                ::ReleaseCapture();
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PointInDivider(point)) {
                // 双击分隔条：在 50% 与"把当前一侧放大到 75%"之间切换
                if (splitRatioPercent_ == 50) {
                    splitRatioPercent_ = splitFocusSecondary_ ? 25 : 75;
                } else {
                    splitRatioPercent_ = 50;
                }
                const RECT previous = dividerRect_;
                LayoutChildren();
                ::InvalidateRect(hwnd_, &previous, TRUE);
                ::InvalidateRect(hwnd_, &dividerRect_, TRUE);
                return 0;
            }
            break;
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
            const core::ExecResult executed = registry_.Execute(id, context);
            if (executed == core::ExecResult::Disabled && id >= 0x1000) {
                // "点了没反应"的另一种来路：命令注册了但当前不该可用。同样要留痕，
                // 否则只能靠人肉二分猜。
                infra::LogWarn(L"cmd", L"命令被禁用，未执行: " + std::to_wstring(id));
            }
            if (executed == core::ExecResult::NotFound) {
                // 只记"像命令"的 ID：控件 id（文档视图是 100 起）走 WM_COMMAND 是正常的，
                // 记进来只会刷屏。命令 ID 的高 4 位是类别，所以从 4096 起。
                if (id >= 0x1000) {
                    infra::LogWarn(L"cmd", L"收到未注册的命令 ID: " + std::to_wstring(id));
                }
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
            if (header->hwndFrom == folderPanel_.Handle()) {
                HandleFolderNotify(*reinterpret_cast<const FolderNotify*>(lParam));
                return 0;
            }
            if (header->hwndFrom == functionListPanel_.Handle()) {
                HandleFunctionNotify(*reinterpret_cast<const FunctionNotify*>(lParam));
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
        case core::kMsgFunctionListReady: {
            // 后台解析的结果：指针所有权在这里接过来（与跨文件搜索同一约定）
            std::unique_ptr<std::vector<core::FunctionSymbol>> symbols(
                reinterpret_cast<std::vector<core::FunctionSymbol>*>(lParam));
            if (symbols != nullptr) {
                ApplyFunctionSymbols(static_cast<std::uint64_t>(wParam), std::move(*symbols));
            }
            return 0;
        }
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
        case core::kMsgOpenFiles: {
            // 别的实例把文件交过来了（或者刚启动时被双击了文件）
            const std::vector<std::wstring> pending = infra::TakePendingFiles();
            for (const std::wstring& path : pending) {
                OpenDocumentPath(path);
            }
            // 用户刚双击了一个文件，把窗口提到前面来
            if (::IsIconic(hwnd_) != 0) {
                ::ShowWindow(hwnd_, SW_RESTORE);
            }
            ::SetForegroundWindow(hwnd_);
            return 0;
        }
        case core::kMsgAppActivate:
            // 另一个实例只是又一次被启动（没带文件）：把本窗口提到前面
            if (::IsIconic(hwnd_) != 0) {
                ::ShowWindow(hwnd_, SW_RESTORE);
            }
            ::SetForegroundWindow(hwnd_);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerAutosave) {
                WriteSnapshots();
                return 0;
            }
            if (wParam == kTimerCrashTest) {
                ::KillTimer(hwnd_, kTimerCrashTest);
                infra::LogInfo(L"app", L"自检开关：现在故意触发一次访问违例");
                infra::crashguard::RaiseTestCrash();   // 自检开关：这里之后不会再返回
                infra::LogWarn(L"app", L"自检开关：异常没有发生（RaiseTestCrash 返回了）");
                return 0;
            }
            if (wParam == kTimerFunctionList) {
                StartFunctionParse();   // 里面会 KillTimer：这是一次性定时器
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
    // 工具菜单里的动作（行操作、排序、去重、空白、大小写、编解码、注释）都是
    // "只依赖文档当前内容"的纯操作，所以都能进宏；撤销/重做、剪贴板、打开/保存这些
    // 依赖历史、剪贴板或用户交互的**不进宏**（回放时会做出与录制时不同的结果）
    const uint32_t needsWritableRecordable = needsWritable | core::kCmdRecordable;

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
    add(kCmdFilePrint, CmdCategory::File, L"cmd.file.print", L"打印(&P)...", needsBuffer,
        [this](const CmdContext&) { CommandPrint(); });
    add(kCmdFileExportHtml, CmdCategory::File, L"cmd.file.exportHtml",
        L"导出为 HTML(&H)...", needsBuffer,
        [this](const CmdContext&) { CommandExportHtml(); });
    add(kCmdFileExportRtf, CmdCategory::File, L"cmd.file.exportRtf", L"导出为 RTF(&R)...",
        needsBuffer, [this](const CmdContext&) { CommandExportRtf(); });

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
    add(kCmdEditAutoComplete, CmdCategory::Edit, L"cmd.edit.autoComplete",
        L"自动完成(&A)", needsWritable,
        [this](const CmdContext&) { CommandAutoComplete(); });

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
    add(kCmdViewFoldToggle, CmdCategory::View, L"cmd.view.foldToggle",
        L"折叠/展开当前层级(&F)", needsBuffer, [this](const CmdContext&) {
            if (edit::Document* document = documents_->Active();
                document != nullptr && document->view) {
                document->view->ToggleFoldAtCaret();
            }
        });
    add(kCmdViewFoldAll, CmdCategory::View, L"cmd.view.foldAll", L"折叠全部(&L)", needsBuffer,
        [this](const CmdContext&) {
            if (edit::Document* document = documents_->Active();
                document != nullptr && document->view) {
                document->view->FoldAll();
            }
        });
    add(kCmdViewUnfoldAll, CmdCategory::View, L"cmd.view.unfoldAll", L"展开全部(&E)",
        needsBuffer, [this](const CmdContext&) {
            if (edit::Document* document = documents_->Active();
                document != nullptr && document->view) {
                document->view->UnfoldAll();
            }
        });
    add(kCmdViewSplitToggle, CmdCategory::View, L"cmd.view.splitToggle",
        L"分屏查看（克隆当前文档）(&P)", core::kCmdCheckable | needsBuffer,
        [this](const CmdContext&) { CommandToggleSplit(); });
    add(kCmdViewCyclePane, CmdCategory::View, L"cmd.view.cyclePane", L"切换到另一视图(&W)",
        core::kCmdNone, [this](const CmdContext&) { CommandCyclePane(); });
    add(kCmdViewScrollSync, CmdCategory::View, L"cmd.view.scrollSync", L"分屏同步滚动(&Y)",
        core::kCmdCheckable, [this](const CmdContext&) {
            splitScrollSync_ = !splitScrollSync_;
        });
    add(kCmdViewDarkMode, CmdCategory::View, L"cmd.view.darkMode", L"暗色模式(&D)",
        core::kCmdCheckable, [this](const CmdContext&) { CommandToggleDarkMode(); });

    // ---- 宏 ----
    add(kCmdMacroRecord, CmdCategory::Macro, L"cmd.macro.record", L"开始/停止录制宏(&R)",
        core::kCmdCheckable,
        [this](const CmdContext&) { CommandToggleMacroRecord(); });
    add(kCmdMacroPlayLast, CmdCategory::Macro, L"cmd.macro.playLast", L"播放上次的宏(&P)",
        core::kCmdNone, [this](const CmdContext&) { CommandPlayLastMacro(); });
    add(kCmdMacroRunMultiple, CmdCategory::Macro, L"cmd.macro.runMultiple",
        L"重复执行上次的宏(&L)...", core::kCmdNone,
        [this](const CmdContext&) { CommandRunMacroMultiple(); });
    add(kCmdMacroSaveLast, CmdCategory::Macro, L"cmd.macro.saveLast", L"保存录制的宏(&S)...",
        core::kCmdNone, [this](const CmdContext&) { FlushRecordingToStore(); });

    // ---- 视图：文件夹工作区 ----
    add(kCmdViewWorkspaceOpen, CmdCategory::View, L"cmd.view.workspaceOpen",
        L"打开文件夹作为工作区(&F)...", core::kCmdNone,
        [this](const CmdContext&) { CommandOpenWorkspace(); });
    add(kCmdViewWorkspaceClose, CmdCategory::View, L"cmd.view.workspaceClose",
        L"关闭工作区(&C)", core::kCmdNone,
        [this](const CmdContext&) { CommandCloseWorkspace(); });
    add(kCmdViewWorkspaceRefresh, CmdCategory::View, L"cmd.view.workspaceRefresh",
        L"刷新工作区(&R)", core::kCmdNone,
        [this](const CmdContext&) { CommandRefreshWorkspace(); });
    add(kCmdViewWorkspaceOpenSel, CmdCategory::View, L"cmd.view.workspaceOpenSelected",
        L"打开选中的文件(&O)", core::kCmdNone,
        [this](const CmdContext&) { CommandOpenWorkspaceSelection(); });
    add(kCmdViewFunctionList, CmdCategory::View, L"cmd.view.functionList", L"函数列表(&F)",
        core::kCmdCheckable, [this](const CmdContext&) { CommandToggleFunctionList(); });
    add(kCmdViewFunctionJump, CmdCategory::View, L"cmd.view.functionJump",
        L"跳到选中的符号(&J)", core::kCmdNone,
        [this](const CmdContext&) { functionListPanel_.ActivateSelection(); });

    // ---- 格式（行尾转换）----
    add(kCmdFormatEolCrlf, CmdCategory::Format, L"cmd.format.eolCrlf", L"转为 Windows 行尾（CRLF）",
        needsWritable, [this](const CmdContext&) { CommandConvertEol(edit::EolMode::Crlf); });
    add(kCmdFormatEolLf, CmdCategory::Format, L"cmd.format.eolLf", L"转为 Unix 行尾（LF）",
        needsWritable, [this](const CmdContext&) { CommandConvertEol(edit::EolMode::Lf); });
    add(kCmdFormatEolCr, CmdCategory::Format, L"cmd.format.eolCr", L"转为 Mac 行尾（CR）",
        needsWritable, [this](const CmdContext&) { CommandConvertEol(edit::EolMode::Cr); });

    // ---- 编码（转为 XX 编码）----
    add(kCmdEncodingAnsi, CmdCategory::Encoding, L"cmd.encoding.ansi",
        L"转为 ANSI（按默认代码页）(&A)", needsWritable,
        [this](const CmdContext&) { CommandConvertEncoding(edit::UniMode::Ansi); });
    add(kCmdEncodingUtf8, CmdCategory::Encoding, L"cmd.encoding.utf8", L"转为 UTF-8(&U)",
        needsWritable, [this](const CmdContext&) { CommandConvertEncoding(edit::UniMode::Utf8); });
    add(kCmdEncodingUtf8Bom, CmdCategory::Encoding, L"cmd.encoding.utf8Bom",
        L"转为 UTF-8（带 BOM）(&B)", needsWritable,
        [this](const CmdContext&) { CommandConvertEncoding(edit::UniMode::Utf8Bom); });
    add(kCmdEncodingUtf16Le, CmdCategory::Encoding, L"cmd.encoding.utf16Le",
        L"转为 UTF-16 LE(&L)", needsWritable,
        [this](const CmdContext&) { CommandConvertEncoding(edit::UniMode::Utf16LE); });
    add(kCmdEncodingUtf16Be, CmdCategory::Encoding, L"cmd.encoding.utf16Be",
        L"转为 UTF-16 BE(&E)", needsWritable,
        [this](const CmdContext&) { CommandConvertEncoding(edit::UniMode::Utf16BE); });

    // ---- 工具：行操作与文本转换（设计文档 2.13）----
    // 用分组分隔线代替子菜单：本版的菜单是"一个类别一个平铺菜单"，
    // 嵌套子菜单要等菜单定制那一版。分组顺序是 行操作 → 排序 → 去重 → 空白 → 行号 → 大小写 → 编码。
    // 分隔线也要占一个命令 ID。**必须给它显式的保留号**：用"自动分配"的话，
    // 它领到的号正好是紧随其后的那条命令的显式号，于是那条命令被当成重复注册丢弃
    // （表现：菜单里有这一项，点了没反应）。保留号放在没人用的第 15 类里。
    int separatorIndex = 0;
    auto addSeparator = [&](const wchar_t* key) {
        add(core::MakeCmdId(15, static_cast<uint16_t>(++separatorIndex)), CmdCategory::Tools, key,
            L"-", core::kCmdNone, [](const CmdContext&) {});
    };
    auto addTool = [&](core::CmdId id, const wchar_t* key, const wchar_t* text,
                       std::function<std::string(std::string_view, std::uint32_t)> tool) {
        add(id, CmdCategory::Tools, key, text, needsWritableRecordable,
            [this, tool = std::move(tool), text](const CmdContext&) {
                ApplyTextTool(tool, text);
            });
    };
    auto addLineOp = [&](core::CmdId id, const wchar_t* key, const wchar_t* text,
                         void (edit::EditView::*operation)() const) {
        add(id, CmdCategory::Tools, key, text, needsWritableRecordable,
            [this, operation](const CmdContext&) {
                if (edit::Document* document = ActiveDocument();
                    document != nullptr && document->view) {
                    (document->view.get()->*operation)();
                }
            });
    };

    addLineOp(kCmdToolDuplicateLine, L"cmd.tool.duplicateLine", L"复制当前行(&D)",
              &edit::EditView::DuplicateCurrentLine);
    addLineOp(kCmdToolDeleteLine, L"cmd.tool.deleteLine", L"删除当前行(&L)",
              &edit::EditView::DeleteCurrentLine);
    addLineOp(kCmdToolMoveLineUp, L"cmd.tool.moveLineUp", L"上移当前行(&U)",
              &edit::EditView::MoveCurrentLineUp);
    addLineOp(kCmdToolMoveLineDown, L"cmd.tool.moveLineDown", L"下移当前行(&N)",
              &edit::EditView::MoveCurrentLineDown);

    addSeparator(L"cmd.tool.sepSort");
    addTool(kCmdToolSortAsc, L"cmd.tool.sortAsc", L"升序排序(&A)",
            [](std::string_view text, std::uint32_t) {
                return edit::SortLines(text, edit::SortOrder::Ascending, true);
            });
    addTool(kCmdToolSortDesc, L"cmd.tool.sortDesc", L"降序排序(&Z)",
            [](std::string_view text, std::uint32_t) {
                return edit::SortLines(text, edit::SortOrder::Descending, true);
            });
    addTool(kCmdToolSortCaseInsAsc, L"cmd.tool.sortCaseInsAsc", L"升序排序（忽略大小写）",
            [](std::string_view text, std::uint32_t) {
                return edit::SortLines(text, edit::SortOrder::Ascending, false);
            });
    addTool(kCmdToolSortCaseInsDesc, L"cmd.tool.sortCaseInsDesc", L"降序排序（忽略大小写）",
            [](std::string_view text, std::uint32_t) {
                return edit::SortLines(text, edit::SortOrder::Descending, false);
            });
    addTool(kCmdToolSortLengthAsc, L"cmd.tool.sortLengthAsc", L"按长度升序排序",
            [](std::string_view text, std::uint32_t) {
                return edit::SortLinesByLength(text, edit::SortOrder::Ascending);
            });
    addTool(kCmdToolSortLengthDesc, L"cmd.tool.sortLengthDesc", L"按长度降序排序",
            [](std::string_view text, std::uint32_t) {
                return edit::SortLinesByLength(text, edit::SortOrder::Descending);
            });

    addSeparator(L"cmd.tool.sepDup");
    addTool(kCmdToolRemoveEmpty, L"cmd.tool.removeEmpty", L"删除空行(&E)",
            [](std::string_view text, std::uint32_t) { return edit::RemoveEmptyLines(text); });
    addTool(kCmdToolRemoveDupFirst, L"cmd.tool.removeDupFirst", L"删除重复行（保留首次出现）",
            [](std::string_view text, std::uint32_t) {
                return edit::RemoveDuplicateLines(text, false);
            });
    addTool(kCmdToolRemoveDupLast, L"cmd.tool.removeDupLast", L"删除重复行（保留最后一次）",
            [](std::string_view text, std::uint32_t) {
                return edit::RemoveDuplicateLines(text, true);
            });
    addTool(kCmdToolReverseLines, L"cmd.tool.reverseLines", L"反转行序(&R)",
            [](std::string_view text, std::uint32_t) { return edit::ReverseLines(text); });

    addSeparator(L"cmd.tool.sepSpace");
    addTool(kCmdToolTrimTrailing, L"cmd.tool.trimTrailing", L"去掉行尾空白",
            [](std::string_view text, std::uint32_t) {
                return edit::TrimTrailingWhitespace(text);
            });
    addTool(kCmdToolTrimLeading, L"cmd.tool.trimLeading", L"去掉行首空白",
            [](std::string_view text, std::uint32_t) {
                return edit::TrimLeadingWhitespace(text);
            });
    addTool(kCmdToolAddLineNumbers, L"cmd.tool.addLineNumbers", L"加上行号",
            [](std::string_view text, std::uint32_t) {
                return edit::AddLineNumbers(text, 1, 1, 0);
            });
    addTool(kCmdToolStripLineNumbers, L"cmd.tool.stripLineNumbers", L"去掉行号",
            [](std::string_view text, std::uint32_t) {
                return edit::StripLineNumbers(text);
            });

    addSeparator(L"cmd.tool.sepCase");
    addTool(kCmdToolUpperCase, L"cmd.tool.upperCase", L"转为大写(&U)",
            [](std::string_view text, std::uint32_t codePage) {
                return edit::ToUpperCase(text, codePage);
            });
    addTool(kCmdToolLowerCase, L"cmd.tool.lowerCase", L"转为小写(&O)",
            [](std::string_view text, std::uint32_t codePage) {
                return edit::ToLowerCase(text, codePage);
            });

    addSeparator(L"cmd.tool.sepEncode");
    addTool(kCmdToolUrlEncode, L"cmd.tool.urlEncode", L"URL 编码",
            [](std::string_view text, std::uint32_t) { return edit::UrlEncode(text); });
    add(kCmdToolUrlDecode, CmdCategory::Tools, L"cmd.tool.urlDecode", L"URL 解码",
        needsWritable, [this](const CmdContext&) { CommandDecodeText(false); });
    addTool(kCmdToolBase64Encode, L"cmd.tool.base64Encode", L"Base64 编码",
            [](std::string_view text, std::uint32_t) { return edit::Base64Encode(text); });
    add(kCmdToolBase64Decode, CmdCategory::Tools, L"cmd.tool.base64Decode", L"Base64 解码",
        needsWritable, [this](const CmdContext&) { CommandDecodeText(true); });
    addTool(kCmdToolHtmlEscape, L"cmd.tool.htmlEscape", L"HTML 实体转义",
            [](std::string_view text, std::uint32_t) { return edit::EscapeHtml(text); });
    addTool(kCmdToolHtmlUnescape, L"cmd.tool.htmlUnescape", L"HTML 实体还原",
            [](std::string_view text, std::uint32_t) { return edit::UnescapeHtml(text); });

    addSeparator(L"cmd.tool.sepComment");
    add(kCmdToolToggleComment, CmdCategory::Tools, L"cmd.tool.toggleComment",
        L"注释/取消注释(&Q)", needsWritableRecordable,
        [this](const CmdContext&) { CommandToggleComment(false); });
    add(kCmdToolBlockComment, CmdCategory::Tools, L"cmd.tool.blockComment",
        L"区块注释/取消(&B)", needsWritableRecordable,
        [this](const CmdContext&) { CommandToggleComment(true); });
    add(kCmdToolExportLanguages, CmdCategory::Tools, L"cmd.tool.exportLanguages",
        L"导出语言定义到配置目录", core::kCmdNone,
        [this](const CmdContext&) { CommandExportLanguageDefs(); });
    add(kCmdToolStyleConfigurator, CmdCategory::Tools, L"cmd.tool.styleConfigurator",
        L"样式配置器（配色）(&S)...", core::kCmdNone,
        [this](const CmdContext&) { CommandStyleConfigurator(); });
    add(kCmdToolExportUdlSample, CmdCategory::Tools, L"cmd.tool.exportUdlSample",
        L"导出 UDL 示例到配置目录", core::kCmdNone,
        [this](const CmdContext&) { CommandExportUdlSample(); });

    addSeparator(L"cmd.tool.sepFormat");
    add(kCmdToolFormatJson, CmdCategory::Tools, L"cmd.tool.formatJson", L"格式化 JSON(&J)",
        needsWritable, [this](const CmdContext&) { CommandFormatStructured(true, false); });
    add(kCmdToolMinifyJson, CmdCategory::Tools, L"cmd.tool.minifyJson", L"压缩 JSON(&M)",
        needsWritable, [this](const CmdContext&) { CommandFormatStructured(true, true); });
    add(kCmdToolFormatXml, CmdCategory::Tools, L"cmd.tool.formatXml", L"美化 XML(&X)",
        needsWritable, [this](const CmdContext&) { CommandFormatStructured(false, false); });

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
    add(kCmdSettingsRegisterAssoc, CmdCategory::Settings, L"cmd.settings.registerAssoc",
        L"加入打开方式列表(&A)", core::kCmdNone,
        [this](const CmdContext&) { CommandRegisterAssociations(true); });
    add(kCmdSettingsUnregisterAssoc, CmdCategory::Settings, L"cmd.settings.unregisterAssoc",
        L"从打开方式列表移除(&R)", core::kCmdNone,
        [this](const CmdContext&) { CommandRegisterAssociations(false); });
    add(kCmdSettingsPlugins, CmdCategory::Settings, L"cmd.settings.plugins",
        L"插件管理器(&G)...", core::kCmdNone,
        [this](const CmdContext&) { CommandPluginManager(); });

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
    bindChecked(kCmdViewSplitToggle, [this] { return splitEnabled_; });
    bindChecked(kCmdViewScrollSync, [this] { return splitScrollSync_; });
    bindChecked(kCmdViewDarkMode, [this] { return darkMode_; });
    bindChecked(kCmdMacroRecord, [this] { return macroRecorder_.Recording(); });
    bindChecked(kCmdViewWorkspaceClose, [this] { return !folderPanel_.Root().empty(); });
    bindChecked(kCmdViewWorkspaceOpenSel, [this] { return !folderPanel_.SelectedPath().empty(); });
    bindChecked(kCmdViewFunctionList, [this] { return functionListVisible_; });
    bindChecked(kCmdViewFunctionJump, [this] { return functionListPanel_.SelectedLine() >= 0; });

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
    registry_.BindShortcut(kCmdFilePrint, ctrl, 'P', true);   // 与对标产品一致
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
    registry_.BindShortcut(kCmdViewFoldToggle, ctrlAlt, 'F', true);
    registry_.BindShortcut(kCmdViewFoldAll, core::kModAlt, '0', true);
    registry_.BindShortcut(kCmdViewUnfoldAll, core::kModAlt | core::kModShift, '0', true);
    registry_.BindShortcut(kCmdViewSplitToggle, ctrlAlt, 'V', true);
    // 设计附录 A 把 Ctrl+Alt+M 给了"移到另一视图"（还没实现），这里先用 F6 切换焦点
    registry_.BindShortcut(kCmdViewCyclePane, core::kModNone, VK_F6, true);
    registry_.BindShortcut(kCmdWindowNext, ctrl, VK_TAB, true);
    registry_.BindShortcut(kCmdWindowPrev, ctrlShift, VK_TAB, true);
    registry_.BindShortcut(kCmdWindowNext, ctrl, VK_NEXT, false);
    registry_.BindShortcut(kCmdWindowPrev, ctrl, VK_PRIOR, false);
    registry_.BindShortcut(kCmdSettingsPreferences, ctrl, VK_OEM_COMMA, true);
    // 附录 A 里属于行操作的默认键（其余工具命令不给默认键，避免和打字冲突）
    registry_.BindShortcut(kCmdToolDuplicateLine, ctrl, 'D', true);
    registry_.BindShortcut(kCmdToolDeleteLine, ctrl, 'L', true);
    registry_.BindShortcut(kCmdToolMoveLineUp, ctrlShift, VK_UP, true);
    registry_.BindShortcut(kCmdToolMoveLineDown, ctrlShift, VK_DOWN, true);
    registry_.BindShortcut(kCmdToolUpperCase, ctrlShift, 'U', true);
    registry_.BindShortcut(kCmdToolLowerCase, ctrl, 'U', true);
    registry_.BindShortcut(kCmdToolToggleComment, ctrl, 'Q', true);
    registry_.BindShortcut(kCmdToolBlockComment, ctrlShift, 'Q', true);
    registry_.BindShortcut(kCmdEditAutoComplete, ctrl, VK_SPACE, true);
    // 宏：与对标产品的默认键一致（附录 A）
    registry_.BindShortcut(kCmdMacroRecord, ctrlShift, 'R', true);
    registry_.BindShortcut(kCmdMacroPlayLast, ctrlShift, 'P', true);
    registry_.BindShortcut(kCmdMacroRunMultiple, ctrlShift, 'L', true);
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

    // 左侧一列：文件夹工作区在上、函数列表在下（两个都开就各占一半）。
    // 编辑区（含分屏）在剩下的区域里排。没有真正的 DockHost，所以这里是"固定左栏"，
    // 宽度与分割方式都写死——见设计说明书 2.4 第 5 条记的差异
    const bool folderShown = folderPanel_.Width() > 0 && folderPanel_.Handle() != nullptr;
    const bool functionsShown = functionListVisible_ && functionListPanel_.Handle() != nullptr;
    // 宽度问面板自己（它按自己的 DPI 缩放）；两个都开时用左边的那个值
    const int panelWidth = folderShown ? folderPanel_.Width()
                                       : (functionsShown ? functionListPanel_.Width() : 0);
    if (panelWidth > 0) {
        RECT column{};
        column.left = 0;
        column.top = tabHeight;
        column.right = panelWidth;
        column.bottom = client.bottom - bottomInset;
        if (column.bottom > column.top) {
            if (folderShown && functionsShown) {
                const int split = column.top + (column.bottom - column.top) / 2;
                RECT top = column;
                top.bottom = split;
                RECT bottom = column;
                bottom.top = split + 1;
                folderPanel_.Resize(top);
                functionListPanel_.Resize(bottom);
            } else if (folderShown) {
                folderPanel_.Resize(column);
            } else {
                functionListPanel_.Resize(column);
            }
        }
    }

    RECT viewRect{};
    viewRect.left = panelWidth;
    viewRect.top = tabHeight;
    viewRect.right = client.right;
    viewRect.bottom = client.bottom - bottomInset;
    if (viewRect.bottom < viewRect.top) {
        viewRect.bottom = viewRect.top;
    }

    // 分屏：把视图区域左右切开，中间留一条可拖动的分隔条。
    // 分隔条由主窗口自己画/自己接鼠标——它只是两块之间的空白，不需要额外的子窗口。
    dividerRect_ = RECT{0, 0, 0, 0};
    if (splitEnabled_ && documents_->SplitEnabled() && viewRect.right - viewRect.left > 120) {
        const int width = viewRect.right - viewRect.left;
        constexpr int kDividerWidth = 5;
        int left = width * splitRatioPercent_ / 100;
        left = std::max(80, std::min(width - 80 - kDividerWidth, left));

        RECT primary = viewRect;
        primary.right = viewRect.left + left;
        RECT secondary = viewRect;
        secondary.left = primary.right + kDividerWidth;
        dividerRect_.left = primary.right;
        dividerRect_.right = secondary.left;
        dividerRect_.top = viewRect.top;
        dividerRect_.bottom = viewRect.bottom;

        documents_->ResizeActive(primary);
        documents_->SplitResize(secondary);
    } else {
        documents_->ResizeActive(viewRect);
    }
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
    // 活动文档可能换了，克隆视图要跟着指过去
    SyncSplitView();
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
        FocusActivePane();   // 分屏时焦点可能落在另一侧
    }
    tabBar_.SetActive(id);
    LayoutChildren();
    UpdateTitle();
    UpdateStatus();
    // 兜底检查：目录通知可能因为缓冲区溢出而漏掉，切到某个标签时再确认一次
    CheckActiveDocumentForExternalChange();
    // 换标签之后函数列表要跟着换（节流：连续切标签只解析最后一个）
    ScheduleFunctionParse();
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
    snapshot.split = splitEnabled_;
    snapshot.splitRatioPercent = splitRatioPercent_;
    snapshot.splitSyncScroll = splitScrollSync_;
    snapshot.splitActiveSlot = splitFocusSecondary_ ? 1 : 0;
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
        entry.folds = document->view->FoldedLines();
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
        // 折叠放在最后：折叠会改变可见行数，先把滚动位置安顿好再折起来
        document->view->RestoreFoldedLines(entry.folds);
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
    // 分屏状态放在最后恢复：它要在文档都开好、活动文档定下来之后才接得上
    if (snapshot.split) {
        splitEnabled_ = true;
        splitRatioPercent_ = snapshot.splitRatioPercent;
        splitScrollSync_ = snapshot.splitSyncScroll;
        splitFocusSecondary_ = snapshot.splitActiveSlot == 1;
        SyncSplitView();
        LayoutChildren();
        FocusActivePane();
    }
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
    // 克隆视图不属于任何文档（它只是"看着"活动文档），所以先认出它，
    // 再把它的通知归到活动文档上——两边本来就是同一份文档。
    edit::EditView* clone = SecondaryView();
    const bool fromClone = clone != nullptr && clone->Valid() && header.hwndFrom == clone->Handle();
    edit::Document* document = fromClone ? documents_->Active() : DocumentOfView(header.hwndFrom);
    if (document == nullptr) {
        return;
    }

    // 关键：修改标记记在"发出通知的那个文档"上，而不是当前活动文档。
    // 打开文件时会先给隐藏的控件 SetText，此时活动文档还不是它。
    switch (note.event) {
        case edit::EditorEvent::CharAdded:
            // 宏录制：敲进去的字按空闲合并成一步（设计文档 5.7 第 4 条）。
            // 只从活动文档录：录到别的标签上会得到"看着对、回放跑偏"的宏
            if (macroRecorder_.Recording() && document == documents_->Active()) {
                macroRecorder_.RecordChar(note.character, static_cast<int64_t>(::GetTickCount64()));
            }
            break;
        case edit::EditorEvent::SavePointLeft:
            document->buffer.isModified = true;
            // 文档变了：函数列表要重算（节流 300 ms，见 ScheduleFunctionParse）
            if (document == documents_->Active()) {
                ScheduleFunctionParse();
            }
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
            // 分屏同步滚动：按"首可见行"对齐另一侧。
            // 先比一次再设：两边行号已经一样就别动，否则会来回互相触发。
            if (splitEnabled_ && splitScrollSync_ &&
                (note.updated & 0x4 /* SC_UPDATE_V_SCROLL */) != 0) {
                edit::EditView* source = fromClone ? clone : document->view.get();
                edit::EditView* other = fromClone ? document->view.get() : clone;
                if (source != nullptr && other != nullptr && other->Valid()) {
                    const int32_t line = source->FirstVisibleLine();
                    if (other->FirstVisibleLine() != line) {
                        other->SetFirstVisibleLine(line);
                    }
                }
            }
            return;
        case edit::EditorEvent::FocusIn:
            // 鼠标点进哪一侧，就把"当前侧"记成哪一侧，快捷键才知道该往哪儿用
            splitFocusSecondary_ = fromClone;
            break;
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
    infra::LogInfo(L"watch", L"收到文件改动通知: " + path);
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
        infra::LogWarn(L"watch", L"改动通知对应的文件不在已打开的文档里: " + path);
        return;   // 不在我们打开的范围内（可能是别的程序在同目录里改文件）
    }
    if (target != documents_->Active()) {
        infra::LogInfo(L"watch", L"改动来自非活动标签，留到切过去时再问: " + path);
        return;   // 切到该标签时由兜底检查处理
    }

    // 内容其实没变（比如只是时间戳被动过）就静默刷新记录
    if (!documents_->HasExternallyChanged(*target)) {
        documents_->RefreshIdentity(*target);
        // 这一条以前是静默的，结果"外部明明改了却没有任何反应"完全无从查起。
        // 记一行，至少能看出通知到了、只是身份比对认为没变。
        infra::LogInfo(L"watch", L"收到改动通知但身份未变（大小与时间戳一致），只刷新记录: " +
                                    path);
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
        infra::LogInfo(L"watch", L"兜底检查：当前没有带路径的活动文档，跳过");
        return;
    }
    if (!documents_->HasExternallyChanged(*document)) {
        infra::LogInfo(L"watch", L"兜底检查：文件身份未变，跳过: " + document->buffer.filePath);
        return;
    }
    infra::LogInfo(L"watch", L"兜底检查：确认外部改动，准备询问用户: " +
                                document->buffer.filePath);

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

    // 目录优先级：**工作区根** > 当前文件所在目录 > 上次用过的目录。
    // 工作区排第一是因为它本身就是用户明确选的"我在这儿干活"，在项目里搜是常见意图
    // （对标产品的文件夹工作区插件也是这么接的）；没有工作区时保持原来的规则
    if (!folderPanel_.Root().empty()) {
        searchPanel_.SetDirectory(folderPanel_.Root());
    } else if (edit::Document* document = ActiveDocument();
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

void MainWindow::ApplyTextTool(
    const std::function<std::string(std::string_view, std::uint32_t)>& tool,
    const wchar_t* description) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    if (document->buffer.isReadOnly) {
        ::MessageBoxW(hwnd_, L"这份文档是只读打开的，无法改内容。", L"工具",
                      MB_ICONINFORMATION);
        return;
    }
    // 大小写这类"按字符理解"的变换要知道缓冲区用什么代码页解释
    const std::uint32_t codePage = document->buffer.encoding.uniMode == edit::UniMode::Ansi
                                       ? document->buffer.encoding.codePage
                                       : edit::kCodePageUtf8;
    const std::wstring name = core::StripAccelerator(description);
    // 没有变化时不留撤销步骤、也不把文档标成已修改——用户按了没反应总比留下假改动好
    if (!document->view->ApplyTextTransform(
            [&tool, codePage](std::string_view text) { return tool(text, codePage); })) {
        infra::LogInfo(L"tools", L"内容没有变化，已跳过: " + name);
        return;
    }
    infra::LogInfo(L"tools", L"已执行: " + name);
    SyncTabBar();
    UpdateStatus();
}

const edit::LanguageDef* MainWindow::ActiveLanguage() const {
    edit::Document* document = ActiveDocument();
    if (document == nullptr) {
        return nullptr;
    }
    const std::string extension =
        edit::LanguageRegistry::ExtensionOfFile(document->buffer.filePath);
    return languages_.FindByExtension(extension);
}

void MainWindow::CommandToggleComment(bool block) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    const edit::LanguageDef* language = ActiveLanguage();
    if (language == nullptr) {
        ::MessageBoxW(hwnd_, L"认不出这份文档的语言，无法判断该用什么注释符。", L"注释",
                      MB_ICONINFORMATION);
        return;
    }
    if (block) {
        if (language->blockCommentStart.empty() || language->blockCommentEnd.empty()) {
            ::MessageBoxW(hwnd_, (L"语言「" + language->displayName +
                                  L"」没有定义块注释符（可在导出后的 langs.xml 里补上）。")
                                     .c_str(),
                          L"块注释", MB_ICONINFORMATION);
            return;
        }
        const std::string start = language->blockCommentStart;
        const std::string end = language->blockCommentEnd;
        ApplyTextTool(
            [start, end](std::string_view text, std::uint32_t) {
                return edit::ToggleBlockComment(text, start, end);
            },
            L"区块注释");
        return;
    }
    if (language->lineComment.empty()) {
        ::MessageBoxW(hwnd_, (L"语言「" + language->displayName +
                              L"」没有定义行注释符（可在导出后的 langs.xml 里补上）。")
                                 .c_str(),
                      L"注释", MB_ICONINFORMATION);
        return;
    }
    const std::string prefix = language->lineComment;
    ApplyTextTool(
        [prefix](std::string_view text, std::uint32_t) {
            return edit::ToggleLineComment(text, prefix);
        },
        L"注释/取消注释");
}

void MainWindow::CommandAutoComplete() {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    edit::EditView& view = *document->view;
    if (document->buffer.isReadOnly) {
        return;   // 只读文档连补全都没意义
    }

    // 先算"光标前已经输入的前缀"：文档提词只收以它开头的词，这样大文件上也只是
    // 一遍线性扫描，不必把整篇的词都收下来（5 MB 的文件曾因此卡住界面 54 秒）。
    const int32_t caret = view.CaretPosition();
    const int32_t wordStart = view.WordStartPosition();
    const std::string prefix =
        wordStart < caret ? view.TextRange(wordStart, caret) : std::string();

    // 来源一：文档内已出现的词（最快，也最贴近用户当前在写什么）
    std::vector<std::string> words = edit::CollectWords(
        view.TextRange(0, static_cast<int32_t>(view.ByteLength())), 3, 4000, prefix);

    // 来源二：语言词库文件 <配置目录>\api\<语言>.api —— 一行一个词或几个词都行，
    // '#' 开头算注释。词库缺失是常态，不报错。
    const std::wstring language = view.LanguageName();
    if (!language.empty() && language != L"null") {
        const std::filesystem::path apiPath =
            infra::ConfigRoot() / L"api" / (language + L".api");
        if (::GetFileAttributesW(apiPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const io::ReadResult read = io::ReadWholeFile(apiPath.wstring(), 8ull * 1024 * 1024);
            if (read.ok) {
                const std::vector<std::string> apiWords = edit::ParseWordList(read.bytes);
                words.insert(words.end(), apiWords.begin(), apiWords.end());
            } else {
                infra::LogWarn(L"complete", L"词库读取失败: " + apiPath.wstring());
            }
        }
    }
    if (words.empty()) {
        return;
    }

    // 词库那边的词还没筛过前缀（词库通常很小，筛一遍不值几个钱）
    const std::vector<std::string> candidates = edit::FilterByPrefix(words, prefix, 300);
    if (candidates.empty()) {
        view.CancelCompletion();
        return;
    }
    view.ShowCompletion(candidates);
    infra::LogInfo(L"complete", L"自动完成：前缀 \"" + std::wstring(prefix.begin(), prefix.end()) +
                                    L"\" 给出 " + std::to_wstring(candidates.size()) + L" 个候选");
}

void MainWindow::ApplyStyleSchemeToAllViews() {
    if (documents_ == nullptr) {
        return;
    }
    // 控件皮肤跟着主题走（边距、光标、选区、记号），语法颜色由 ApplyLanguageForPath 负责。
    // 顺序无所谓：ApplySchemeDefaults 里会做 STYLECLEARALL，但它自己会把皮肤重铺一遍。
    const edit::EditorPalette palette =
        styles_.IsDark() ? edit::EditorPalette::Dark() : edit::EditorPalette::Light();
    for (edit::Document* document : documents_->All()) {
        if (document == nullptr || document->view == nullptr) {
            continue;
        }
        document->view->ApplyPalette(palette);
        document->view->ApplyLanguageForPath(document->buffer.filePath, &languages_, &styles_);
    }
    // 克隆视图是独立的 Scintilla 控件，也得重新上色
    if (edit::EditView* clone = SecondaryView(); clone != nullptr && clone->Valid()) {
        clone->ApplyPalette(palette);
        if (edit::Document* active = documents_->Active(); active != nullptr) {
            clone->ApplyLanguageForPath(active->buffer.filePath, &languages_, &styles_);
        }
    }
    tabBar_.SetDark(styles_.IsDark());
}

namespace {

// 宽字符 → UTF-8 字节（回放"插入文本"那一步要用）
std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          need, nullptr, nullptr);
    return out;
}

}  // namespace

void MainWindow::CommandToggleMacroRecord() {
    if (macroRecorder_.Recording()) {
        FlushRecordingToStore();
        return;
    }
    // 录制期间把注册表的执行事件接过来。放在 Execute() 那个唯一入口上，
    // 菜单、快捷键、宏自己走的都是这条路
    macroRecorder_.Start();
    registry_.SetRecordHook([this](const core::Command& command) {
        macroRecorder_.RecordCommand(command);
    });
    statusBar_.SetMacroRecording(true);
    infra::LogInfo(L"macro", L"开始录制宏");
}

void MainWindow::FlushRecordingToStore() {
    if (!macroRecorder_.Recording()) {
        return;
    }
    registry_.SetRecordHook(nullptr);
    macroRecorder_.Stop();
    statusBar_.SetMacroRecording(false);
    const std::vector<core::MacroAction>& actions = macroRecorder_.Actions();
    if (actions.empty()) {
        ::MessageBoxW(hwnd_, L"这段录制里没有任何步骤（可能只是点了菜单或按了 Esc）。",
                      L"录制宏", MB_ICONINFORMATION);
        return;
    }
    core::Macro macro;
    macro.actions = actions;
    const std::wstring suggested = L"Macro " + std::to_wstring(shortcutStore_.Macros().size() + 1);
    if (!AskMacroName(hwnd_, suggested, macro.name)) {
        return;   // 取消：丢弃这段录制（已经停止录制了，状态是干净的）
    }
    lastMacro_ = macro;   // 录完即可播放/保存
    shortcutStore_.AddOrReplaceMacro(macro);
    std::wstring error;
    if (!shortcutStore_.Save(registry_, &error)) {
        infra::LogWarn(L"macro", L"宏保存失败: " + error);
        ::MessageBoxW(hwnd_, (L"宏已生效，但写入 shortcuts.xml 失败：\n" + error).c_str(),
                      L"保存宏失败", MB_ICONWARNING);
    }
    RegisterMacroCommands();
    RebuildMenu();
    statusBar_.SetMessage(L"宏「" + macro.name + L"」已保存（" +
                          std::to_wstring(macro.actions.size()) + L" 步）");
    infra::LogInfo(L"macro", L"宏已保存: " + macro.name + L"，共 " +
                                std::to_wstring(macro.actions.size()) + L" 步");
}

void MainWindow::CommandPlayLastMacro() {
    if (lastMacro_.Empty()) {
        ::MessageBoxW(hwnd_, L"还没有可播放的宏：先录制一段，或从宏菜单里执行一条已保存的宏。",
                      L"播放宏", MB_ICONINFORMATION);
        return;
    }
    RunMacro(lastMacro_, 1, true);
}

void MainWindow::CommandRunMacroMultiple() {
    if (lastMacro_.Empty()) {
        ::MessageBoxW(hwnd_, L"还没有可执行的宏：先录制一段，或从宏菜单里执行一条已保存的宏。",
                      L"执行宏", MB_ICONINFORMATION);
        return;
    }
    MacroRunOptions options;
    if (!AskMacroRunOptions(hwnd_, lastMacro_.name.empty() ? L"(未命名)" : lastMacro_.name,
                            options)) {
        return;
    }
    RunMacro(lastMacro_, options.repeatCount, options.stopOnError);
}

void MainWindow::CommandRunSavedMacro(std::size_t index) {
    const std::vector<core::Macro>& macros = shortcutStore_.Macros();
    if (index >= macros.size()) {
        return;
    }
    lastMacro_ = macros[index];   // 之后"播放上次的宏"就是这一条
    RunMacro(lastMacro_, 1, true);
}

void MainWindow::RunMacro(const core::Macro& macro, int repeatCount, bool stopOnError) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        ::MessageBoxW(hwnd_, L"没有打开的文档，宏没有地方可跑。", L"执行宏", MB_ICONINFORMATION);
        return;
    }
    if (macroRecorder_.Recording()) {
        // 录制中回放会把回放的命令又录进宏里，越滚越大
        ::MessageBoxW(hwnd_, L"正在录制宏：请先停止录制再执行。", L"执行宏", MB_ICONINFORMATION);
        return;
    }

    core::MacroRunner runner;
    runner.runCommand = [this](core::CmdId id) {
        const core::Command* command = registry_.Find(id);
        if (command == nullptr) {
            return false;   // 宏里记的命令在当前版本不存在（比如换了版本）
        }
        core::CmdContext context{};
        if (edit::Document* active = ActiveDocument(); active != nullptr) {
            context.buffer = &active->buffer;
            context.view = active->view.get();
        }
        return registry_.Execute(id, context) == core::ExecResult::Ok;
    };
    runner.sendMessage = [document](const core::MacroAction& action) {
        if (document->view != nullptr && document->view->Valid()) {
            document->view->Send(action.message, static_cast<uintptr_t>(action.wParam),
                                 action.lParam);
        }
    };
    runner.insertText = [document](std::wstring_view text) {
        if (document->view != nullptr && document->view->Valid()) {
            document->view->InsertTextAt(document->view->CaretPosition(), ToUtf8(text));
        }
    };
    // 每步之间抽一次消息：否则回放期间窗口完全没反应，连"按 Esc 中断"都做不到。
    // 代价是这段时间里点菜单能插进来——真要做成"事务式"回放得把编辑操作排队到结束后，
    // 那是另一件事（记在设计说明书 2.9 的差异里）。
    runner.keepGoing = [this] {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                return false;
            }
            if (message.message == WM_QUIT) {
                ::PostQuitMessage(static_cast<int>(message.wParam));
                return false;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        return true;
    };

    statusBar_.SetMessage(L"正在执行宏…（按 Esc 中断）");
    const core::MacroRunResult result = core::RunMacro(macro, runner, repeatCount, stopOnError);
    UpdateStatus();
    infra::LogInfo(L"macro", L"宏执行结束: " + (result.ok ? L"全部成功" : result.message) +
                                 L"，完成 " + std::to_wstring(result.completed) + L" 遍");
    if (!result.ok) {
        std::wstring text = result.message;
        if (result.failedRepeat > 1) {
            text += L"（第 " + std::to_wstring(result.failedRepeat) + L" 遍里出的问题）";
        }
        text += L"\n\n已经完成的 " + std::to_wstring(result.completed) + L" 遍不会回退。";
        ::MessageBoxW(hwnd_, text.c_str(), L"执行宏", MB_ICONWARNING);
    }
}

void MainWindow::RegisterMacroCommands() {
    // 每条宏一个命令：菜单里显示宏名，也能像别的命令一样绑快捷键
    if (macroCommandBase_ == 0) {
        macroCommandBase_ = 128;   // 宏类（8）里前 128 个号留给内置的宏命令
    }
    const std::vector<core::Macro>& macros = shortcutStore_.Macros();
    for (std::size_t index = 0; index < macros.size(); ++index) {
        const core::CmdId id = core::MakeCmdId(static_cast<uint8_t>(core::CmdCategory::Macro),
                                              static_cast<uint16_t>(macroCommandBase_ + index));
        core::Command command;
        command.id = id;
        command.category = core::CmdCategory::Macro;
        command.key = L"macro." + macros[index].name;
        command.text = macros[index].name;
        command.flags = core::kCmdNone;   // 宏自己不该被录进另一条宏（会套娃）
        const std::size_t captured = index;
        command.execute = [this, captured](const core::CmdContext&) { CommandRunSavedMacro(captured); };
        if (!registry_.UpdateCommand(id, command.text, command.execute)) {
            registry_.Register(command);
        }
        // 宏自带的快捷键（文件里写的）也一并生效
        if (macros[index].keyCode != 0 && macros[index].modifiers != core::kModNone) {
            registry_.BindShortcut(id, macros[index].modifiers, macros[index].keyCode, true);
        }
    }
}

bool MainWindow::OpenDocumentPath(const std::wstring& path) {
    const core::OpenOutcome outcome = documents_->Open(path);
    if (outcome.status != core::OpenStatus::Ok &&
        outcome.status != core::OpenStatus::AlreadyOpen) {
        infra::LogWarn(L"file", L"打开失败: " + path + L" / " + outcome.message);
        ::MessageBoxW(hwnd_, outcome.message.c_str(), L"打开文件失败", MB_ICONWARNING);
        return false;
    }
    lastDirectory_ = core::DocumentManager::DirectoryOf(path);
    SyncTabBar();
    if (edit::Document* document = documents_->Active(); document != nullptr) {
        ActivateDocument(document->buffer.id);
    }
    return true;
}

void MainWindow::ApplyWorkspace(const std::wstring& folder, bool persist) {
    folderPanel_.SetRoot(folder);
    if (persist) {
        config_.General().workspaceFolder = folder;
        config_.RequestSave();
    }
    // 面板占掉左侧之后，编辑区要重新排一次
    LayoutChildren();
    if (!folder.empty()) {
        statusBar_.SetMessage(L"工作区：" + folder);
        infra::LogInfo(L"workspace", L"工作区已打开: " + folder);
    }
}

void MainWindow::CommandOpenWorkspace() {
    // 用系统的"选择文件夹"对话框（Vista 之后的样式；XP 时代的 SHBrowseForFolder 太丑）。
    // 免管理员、也不需要自己画一个目录选择器
    BROWSEINFOW browse{};
    browse.hwndOwner = hwnd_;
    browse.lpszTitle = L"选择要作为工作区打开的文件夹：";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE item = ::SHBrowseForFolderW(&browse);
    if (item == nullptr) {
        return;   // 用户取消
    }
    wchar_t buffer[MAX_PATH] = {};
    const bool ok = ::SHGetPathFromIDListW(item, buffer) != 0;
    ::CoTaskMemFree(item);
    if (!ok) {
        return;
    }
    ApplyWorkspace(buffer, true);
}

void MainWindow::CommandCloseWorkspace() {
    if (folderPanel_.Root().empty()) {
        return;
    }
    ApplyWorkspace(std::wstring(), true);
}

void MainWindow::CommandRefreshWorkspace() {
    if (folderPanel_.Root().empty()) {
        return;
    }
    folderPanel_.Refresh();
}

void MainWindow::CommandOpenWorkspaceSelection() {
    folderPanel_.ActivateSelection();
}

void MainWindow::HandleFolderNotify(const FolderNotify& notification) {
    if (notification.action == FolderAction::OpenFile) {
        OpenDocumentPath(notification.path);
    }
}

void MainWindow::HandleFunctionNotify(const FunctionNotify& notification) {
    if (notification.action != FunctionAction::JumpToLine) {
        return;
    }
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    document->view->Send(kSciGotoLine, static_cast<uintptr_t>(notification.line));
    // 焦点交回编辑器：从列表点一下跳过去，接着就要打字了
    ::SetFocus(document->view->Handle());
    UpdateStatus();
}

bool MainWindow::CollectStyledDocument(std::vector<core::StyledByte>& bytes,
                                       std::vector<core::ExportStyle>& styles,
                                       core::ExportOptions& options) const {
    bytes.clear();
    styles.clear();
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return false;
    }
    edit::EditView& view = *document->view;
    std::string text;
    std::vector<int> numbers;
    const int32_t length = static_cast<int32_t>(view.ByteLength());
    view.ReadStyledText(0, length, text, numbers);
    if (text.size() != numbers.size()) {
        // 长度对不上说明读回的过程被打断了（理论上不会），宁可如实报错也不要导出半截
        return false;
    }
    bytes.reserve(text.size());
    std::vector<bool> seen(256, false);
    for (std::size_t index = 0; index < text.size(); ++index) {
        bytes.push_back(core::StyledByte{text[index], numbers[index]});
        if (numbers[index] >= 0 && numbers[index] < 256) {
            seen[static_cast<std::size_t>(numbers[index])] = true;
        }
    }
    // 样式外观从**控件**里读，而不是从配色文件里推：这样导出的颜色与屏幕上看到的一致
    // （包括用户在样式配置器里改过的、以及逐样式号的覆盖）
    for (std::size_t index = 0; index < seen.size(); ++index) {
        if (!seen[index]) {
            continue;
        }
        core::ExportStyle style;
        style.number = static_cast<int>(index);
        style.fore = edit::StyleScheme::FromScintillaColour(static_cast<std::uint32_t>(
            view.StyleForeColor(style.number)));
        style.bold = view.StyleBold(style.number);
        style.italic = view.StyleItalic(style.number);
        styles.push_back(style);
    }
    options.title = "notepadcc 导出";
    if (document->buffer.HasPath()) {
        // 走 UTF-8 转换，不能按字节截宽字符——中文路径会变成乱码（导出文件里的 <title>
        // 与页眉都吃这个值）
        options.title = ToUtf8(document->buffer.filePath);
    }
    options.pageFore = edit::StyleScheme::FromScintillaColour(
        static_cast<std::uint32_t>(view.DefaultForeColor()));
    options.pageBack = edit::StyleScheme::FromScintillaColour(
        static_cast<std::uint32_t>(view.DefaultBackColor()));
    options.fontFace = "Consolas";
    options.fontSize = 11;
    return true;
}

void MainWindow::CommandExportHtml() {
    std::vector<core::StyledByte> bytes;
    std::vector<core::ExportStyle> styles;
    core::ExportOptions options;
    if (!CollectStyledDocument(bytes, styles, options)) {
        ::MessageBoxW(hwnd_, L"没有打开的文档，或者读不回带样式的文本。", L"导出 HTML",
                      MB_ICONINFORMATION);
        return;
    }
    // 默认落在**文档旁边**、用文档名加新后缀：导出的东西通常就是要放在源码旁边的，
    // 让用户每次都从头选目录没有道理（也让自检脚本不必去驱动系统对话框里的路径输入）
    std::wstring suggested = L"export.html";
    std::wstring initialDir;
    if (edit::Document* document = ActiveDocument(); document != nullptr &&
                                               document->buffer.HasPath()) {
        initialDir = core::DocumentManager::DirectoryOf(document->buffer.filePath);
        suggested = core::DocumentManager::FileNameOf(document->buffer.filePath);
        const std::size_t dot = suggested.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            suggested.resize(dot);
        }
        suggested += L".html";
    }
    wchar_t file[MAX_PATH] = {};
    ::wcsncpy_s(file, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"HTML 文件 (*.html)\0*.html\0所有文件\0*.*\0";
    dialog.lpstrTitle = L"导出为 HTML";   // 显式标题：既让用户看清在做什么，也让自检脚本找得到这个框
    dialog.lpstrFile = file;
    dialog.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"html";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (::GetSaveFileNameW(&dialog) == 0) {
        return;   // 用户取消
    }
    const std::string html = core::BuildHtml(bytes, styles, options);
    const io::WriteResult written = io::WriteFileAtomic(file, html);
    if (!written.ok) {
        ::MessageBoxW(hwnd_, (L"写入失败：\n" + written.message).c_str(), L"导出 HTML",
                      MB_ICONWARNING);
        return;
    }
    infra::LogInfo(L"export", L"已导出 HTML: " + std::wstring(file) + L"，" +
                                 std::to_wstring(html.size()) + L" 字节");
    statusBar_.SetMessage(L"已导出 HTML：" + std::wstring(file));
}

void MainWindow::CommandExportRtf() {
    std::vector<core::StyledByte> bytes;
    std::vector<core::ExportStyle> styles;
    core::ExportOptions options;
    if (!CollectStyledDocument(bytes, styles, options)) {
        ::MessageBoxW(hwnd_, L"没有打开的文档，或者读不回带样式的文本。", L"导出 RTF",
                      MB_ICONINFORMATION);
        return;
    }
    std::wstring suggested = L"export.rtf";
    std::wstring initialDir;
    if (edit::Document* document = ActiveDocument(); document != nullptr &&
                                               document->buffer.HasPath()) {
        initialDir = core::DocumentManager::DirectoryOf(document->buffer.filePath);
        suggested = core::DocumentManager::FileNameOf(document->buffer.filePath);
        const std::size_t dot = suggested.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            suggested.resize(dot);
        }
        suggested += L".rtf";
    }
    wchar_t file[MAX_PATH] = {};
    ::wcsncpy_s(file, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"RTF 文件 (*.rtf)\0*.rtf\0所有文件\0*.*\0";
    dialog.lpstrTitle = L"导出为 RTF";
    dialog.lpstrFile = file;
    dialog.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"rtf";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (::GetSaveFileNameW(&dialog) == 0) {
        return;
    }
    const std::string rtf = core::BuildRtf(bytes, styles, options);
    const io::WriteResult written = io::WriteFileAtomic(file, rtf);
    if (!written.ok) {
        ::MessageBoxW(hwnd_, (L"写入失败：\n" + written.message).c_str(), L"导出 RTF",
                      MB_ICONWARNING);
        return;
    }
    infra::LogInfo(L"export", L"已导出 RTF: " + std::wstring(file));
    statusBar_.SetMessage(L"已导出 RTF：" + std::wstring(file));
}

void MainWindow::CommandPrint() {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    edit::EditView& view = *document->view;
    const int32_t length = static_cast<int32_t>(view.ByteLength());
    if (length <= 0) {
        ::MessageBoxW(hwnd_, L"这份文档是空的。", L"打印", MB_ICONINFORMATION);
        return;
    }

    // 打印对话框交给系统：打印机、份数、页范围都由用户选。程序**从不自动打印**——
    // 测试里也只验证到"对话框弹出并能取消"，绝不往真打印机上送纸
    PRINTDLGW print{};
    print.lStructSize = sizeof(print);
    print.hwndOwner = hwnd_;
    print.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION;
    if (::PrintDlgW(&print) == 0) {
        const DWORD extended = ::CommDlgExtendedError();
        if (extended != 0) {
            // 没有打印机、驱动出错之类：明说，不要静默什么都不做
            ::MessageBoxW(hwnd_, (L"打印对话框打不开（可能没有安装打印机）。\n错误码：" +
                                  std::to_wstring(extended))
                                     .c_str(),
                          L"打印", MB_ICONWARNING);
        }
        return;   // 用户取消
    }
    HDC printerDc = print.hDC;

    // 页面几何：设备的像素 → twips（1/1440 英寸）。Scintilla 的排版接口用 twips
    const int dpiX = ::GetDeviceCaps(printerDc, LOGPIXELSX);
    const int dpiY = ::GetDeviceCaps(printerDc, LOGPIXELSY);
    const int pageWidth = ::GetDeviceCaps(printerDc, HORZRES);
    const int pageHeight = ::GetDeviceCaps(printerDc, VERTRES);
    const auto toTwipsX = [dpiX](int value) { return value * 1440 / (dpiX > 0 ? dpiX : 96); };
    const auto toTwipsY = [dpiY](int value) { return value * 1440 / (dpiY > 0 ? dpiY : 96); };
    constexpr int kMarginMm = 15;
    constexpr int kHeaderMm = 8;
    const int marginX = toTwipsX(::MulDiv(kMarginMm, dpiX, 25));   // mm → 像素 → twips
    const int marginY = toTwipsY(::MulDiv(kMarginMm, dpiY, 25));
    const int headerBand = toTwipsY(::MulDiv(kHeaderMm, dpiY, 25));
    const int pageWidthTwips = toTwipsX(pageWidth);
    const int pageHeightTwips = toTwipsY(pageHeight);

    edit::EditView::PrintRange range{};
    range.hdc = printerDc;
    range.hdcTarget = printerDc;
    range.pageLeft = 0;
    range.pageTop = 0;
    range.pageRight = pageWidthTwips;
    range.pageBottom = pageHeightTwips;
    range.rcLeft = marginX;
    range.rcTop = marginY + headerBand;
    range.rcRight = pageWidthTwips - marginX;
    range.rcBottom = pageHeightTwips - marginY;

    view.SetPrintColourMode(darkMode_ ? edit::EditView::kPrintBlackOnWhite
                                      : edit::EditView::kPrintColourOnWhite);
    view.SetPrintWrapMode(edit::EditView::kPrintWrapWord);
    view.SetPrintMagnification(0);

    // 第一遍只测量：算出总页数（页眉页脚里的 $(PAGES) 要用）。
    // 外层的收集与防死循环在 core::CollectPageStarts 里，单测直接钉它
    const std::vector<int32_t> pageStarts =
        core::CollectPageStarts(length, [&view, &range, length](int32_t start) {
            range.start = start;
            range.end = length;
            return view.FormatRange(false, range);
        });
    if (pageStarts.empty()) {
        ::DeleteDC(printerDc);
        ::MessageBoxW(hwnd_, L"这份文档没有可打印的内容。", L"打印", MB_ICONINFORMATION);
        return;
    }

    DOCINFOW info{};
    info.cbSize = sizeof(info);
    const std::wstring title = document->buffer.HasPath() ? document->buffer.filePath
                                                          : document->buffer.displayName;
    info.lpszDocName = title.c_str();
    info.lpszOutput = print.Flags & PD_PRINTTOFILE ? L"PRINT:" : nullptr;
    if (::StartDocW(printerDc, &info) <= 0) {
        ::DeleteDC(printerDc);
        ::MessageBoxW(hwnd_, L"开始打印任务失败（StartDoc）。", L"打印", MB_ICONWARNING);
        return;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t dateText[64] = {};
    ::swprintf_s(dateText, L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);
    const std::wstring headerPattern = L"$(FILE_NAME)";
    const std::wstring footerPattern = L"第 $(PAGE) 页 / 共 $(PAGES) 页    $(DATE)";
    const int totalPages = static_cast<int>(pageStarts.size());
    HFONT font = ::CreateFontW(-::MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    const HGDIOBJ oldFont = ::SelectObject(printerDc, font);
    ::SetBkMode(printerDc, TRANSPARENT);
    ::SetTextColor(printerDc, RGB(0x40, 0x40, 0x40));

    bool started = false;
    for (int page = 0; page < totalPages; ++page) {
        if (::StartPage(printerDc) <= 0) {
            break;
        }
        started = true;
        core::PrintTextContext context;
        context.fileName = core::DocumentManager::FileNameOf(title);
        context.filePath = title;
        context.page = page + 1;
        context.totalPages = totalPages;
        context.date = dateText;
        RECT band{};
        band.left = marginX;
        band.right = pageWidthTwips - marginX;
        band.top = marginY;
        band.bottom = marginY + headerBand;
        std::wstring header = core::ExpandPrintTemplate(headerPattern, context);
        ::DrawTextW(printerDc, header.c_str(), static_cast<int>(header.size()), &band,
                    DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        band.top = pageHeightTwips - marginY;
        band.bottom = pageHeightTwips;
        std::wstring footer = core::ExpandPrintTemplate(footerPattern, context);
        ::DrawTextW(printerDc, footer.c_str(), static_cast<int>(footer.size()), &band,
                    DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);

        range.start = pageStarts[static_cast<std::size_t>(page)];
        range.end = length;
        view.FormatRange(true, range);
        ::EndPage(printerDc);
    }
    ::SelectObject(printerDc, oldFont);
    ::DeleteObject(font);
    if (started) {
        ::EndDoc(printerDc);
    } else {
        ::AbortDoc(printerDc);
    }
    ::DeleteDC(printerDc);
    infra::LogInfo(L"print", L"打印任务结束: " + title + L"，共 " +
                              std::to_wstring(totalPages) + L" 页");
    statusBar_.SetMessage(L"已交给打印机：" + std::to_wstring(totalPages) + L" 页");
}

void MainWindow::CommandToggleFunctionList() {
    functionListVisible_ = !functionListVisible_;
    functionListPanel_.SetVisible(functionListVisible_);
    config_.General().functionListVisible = functionListVisible_;
    config_.RequestSave();
    LayoutChildren();
    if (functionListVisible_) {
        // 开面板就立刻解析一次，不用等用户打字
        StartFunctionParse();
    }
}

void MainWindow::ScheduleFunctionParse() {
    if (!functionListVisible_) {
        return;
    }
    // 节流：每次改动都重置这个一次性定时器，停手 300 ms 才真的解析（设计文档 2.6 第 5 条）
    ::SetTimer(hwnd_, kTimerFunctionList, 300, nullptr);
}

void MainWindow::StartFunctionParse() {
    ::KillTimer(hwnd_, kTimerFunctionList);
    if (!functionListVisible_ || documents_ == nullptr) {
        return;
    }
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        functionListPanel_.SetSymbols({});
        return;
    }
    // 大文件档不列函数（设计文档 6.1 的 T2/T3：那一档要省的正是"扫全文"）
    if (document->buffer.tier != edit::FileTier::Normal) {
        functionListPanel_.SetSymbols({});
        statusBar_.SetMessage(L"函数列表：大文件档不列函数");
        return;
    }
    const std::wstring language = document->view->LanguageName();
    if (!core::HasFunctionExtractor(language)) {
        functionListPanel_.SetSymbols({});
        statusBar_.SetMessage(L"函数列表：这种语言列不出符号");
        return;
    }
    std::string text = document->view->GetAllText();
    // 与自动完成同一个理由：解析放在后台线程，但**不能把整个文档原样搬两份**。
    // 这里给一个上限（4 MB），超过就当"太大不列"——真要看结构也是先把文件拆开
    constexpr std::size_t kMaxParseBytes = 4ull * 1024 * 1024;
    if (text.size() > kMaxParseBytes) {
        functionListPanel_.SetSymbols({});
        statusBar_.SetMessage(L"函数列表：文件超过 4 MB，不列符号");
        return;
    }
    const std::uint64_t requestId = ++functionRequestId_;
    HWND target = hwnd_;
    // 解析在后台线程：Scintilla 只能在 UI 线程访问，所以文本是**一份拷贝**；
    // 结果用一条消息递回来，指针所有权一并转移（与跨文件搜索同一条约定）
    functionPool_->Submit([language, text = std::move(text), requestId, target]() mutable {
        auto symbols = std::make_unique<std::vector<core::FunctionSymbol>>(
            core::ExtractFunctionList(language, text));
        if (::PostMessageW(target, core::kMsgFunctionListReady,
                           static_cast<WPARAM>(requestId),
                           reinterpret_cast<LPARAM>(symbols.get())) == 0) {
            return;   // 窗口没了：指针随 unique_ptr 一起释放
        }
        symbols.release();   // 所有权交给 UI 线程
    });
}

void MainWindow::ApplyFunctionSymbols(std::uint64_t requestId,
                                      std::vector<core::FunctionSymbol> symbols) {
    // 迟到的结果直接丢：用户可能已经改了文件或者换了标签
    if (requestId != functionRequestId_ || !functionListVisible_) {
        return;
    }
    const std::size_t count = symbols.size();
    functionListPanel_.SetSymbols(symbols);
    statusBar_.SetMessage(count == 0 ? L"函数列表：没有找到符号"
                                     : L"函数列表：" + std::to_wstring(count) + L" 个符号");
}

// ---------------------------------------------------------------- 插件宿主（设计文档 2.10）

bool MainWindow::RegisterPluginCommand(const std::wstring& pluginName, const std::wstring& key,
                                       const std::wstring& name, void (*execute)(void*),
                                       void* pluginCookie) {
    if (execute == nullptr) {
        return false;
    }
    // 命令键带上插件名：不同插件可以各自用一个 "insert"，不会互相撞
    const std::wstring fullKey = pluginName.empty() ? key : pluginName + L"." + key;
    core::Command command;
    command.category = core::CmdCategory::Plugin;
    command.key = L"plugin." + fullKey;
    command.text = pluginName.empty() ? name : (name + L"（" + pluginName + L"）");
    // 插件命令**可以进宏**（对标产品也允许录），但插件自己不能再录别人
    command.flags = core::kCmdRecordable;
    command.execute = [execute, pluginCookie](const core::CmdContext&) { execute(pluginCookie); };
    const core::CmdId id = registry_.Register(std::move(command));
    return id != core::kCmdNone;
}

std::wstring MainWindow::ConfigPath() const {
    return infra::ConfigRoot().wstring();
}

std::string MainWindow::EditorText() const {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return {};
    }
    return document->view->GetAllText();
}

bool MainWindow::ReplaceSelection(std::string_view utf8) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr || document->buffer.isReadOnly) {
        return false;
    }
    document->view->Send(kSciReplaceSel, 0, reinterpret_cast<intptr_t>(std::string(utf8).c_str()));
    return true;
}

void MainWindow::ShowMessage(const std::wstring& text) {
    ::MessageBoxW(hwnd_, text.c_str(), L"插件", MB_ICONINFORMATION);
}

void MainWindow::Log(int level, const std::wstring& text) {
    switch (level) {
        case 0: infra::LogError(L"plugin", text); break;
        case 1: infra::LogWarn(L"plugin", text); break;
        default: infra::LogInfo(L"plugin", text); break;
    }
}

void MainWindow::LoadPlugins() {
    plugins_.LoadAll(*this, &registry_);
    const std::size_t loaded = plugins_.LoadedCount();
    if (!plugins_.Plugins().empty()) {
        infra::LogInfo(L"plugin", L"插件扫描完成：加载 " + std::to_wstring(loaded) + L" 个，共发现 " +
                                     std::to_wstring(plugins_.Plugins().size()) + L" 个");
    }
}

void MainWindow::CommandPluginManager() {
    if (ShowPluginManager(hwnd_, plugins_)) {
        // 启用/禁用只写文件，**下次启动生效**（不能安全卸库，见 PluginHost 的说明），
        // 所以这里只提示一句，不假装立刻生效
        statusBar_.SetMessage(L"插件启用/禁用已保存，重启程序后生效");
    }
}

void MainWindow::CommandToggleDarkMode() {
    darkMode_ = !darkMode_;
    // 主题名落进 config.xml；配色本身（两套）在 stylers.xml 里
    config_.General().theme = darkMode_ ? L"dark" : L"light";
    config_.RequestSave();
    if (!styles_.SetActiveTheme(config_.General().theme)) {
        // 理论上不会发生（内置两套主题永远在），真发生了就明说，不要静默停在旧主题上
        infra::LogWarn(L"styles", L"配置里的主题名找不到，继续用当前主题: " +
                                     config_.General().theme);
    }
    // 之后新建的标签也要用同一套皮肤（DocumentManager 建视图时会用）
    documents_->SetPalette(darkMode_ ? edit::EditorPalette::Dark() : edit::EditorPalette::Light());
    ApplyStyleSchemeToAllViews();
}

void MainWindow::CommandStyleConfigurator() {
    if (!ShowStyleConfigurator(hwnd_, languages_, styles_)) {
        return;   // 取消：什么都不动
    }
    ApplyStyleSchemeToAllViews();
    std::wstring error;
    if (!styles_.Save(&error)) {
        infra::LogWarn(L"styles", L"样式方案保存失败: " + error);
        ::MessageBoxW(hwnd_, (L"配色已应用，但写入 stylers.xml 失败：\n" + error).c_str(),
                      L"保存样式方案失败", MB_ICONWARNING);
    }
}

void MainWindow::CommandExportLanguageDefs() {
    const std::filesystem::path path = edit::LanguageRegistry::Path();
    const bool exists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        const int answer = ::MessageBoxW(
            hwnd_,
            (L"配置目录里已经有 langs.xml：\n" + path.wstring() +
             L"\n\n覆盖它会丢掉你改过的内容。要用当前生效的定义覆盖吗？")
                .c_str(),
            L"导出语言定义", MB_ICONWARNING | MB_YESNO);
        if (answer != IDYES) {
            return;
        }
    }
    std::wstring error;
    if (!languages_.Save(&error)) {
        ::MessageBoxW(hwnd_, (L"导出失败：\n" + error).c_str(), L"导出语言定义", MB_ICONWARNING);
        return;
    }
    // 导出之后用户可以改这份文件，下次启动生效；这里把"怎么用"说清楚
    ::MessageBoxW(hwnd_,
                  (L"语言定义已写到：\n" + path.wstring() +
                   L"\n\n可以编辑它来改扩展名归属、关键字、注释符；文件里出现的语言覆盖内置同名"
                   L"语言，没出现的仍用内置值。改完重启生效。")
                      .c_str(),
                  L"导出语言定义", MB_ICONINFORMATION);
}

void MainWindow::CommandExportUdlSample() {
    const std::filesystem::path path = edit::UdlLoader::Path();
    const bool exists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        const int answer = ::MessageBoxW(
            hwnd_,
            (L"配置目录里已经有 userlangs.xml：\n" + path.wstring() +
             L"\n\n覆盖它会丢掉你自己写的语言定义。要用示例覆盖吗？")
                .c_str(),
            L"UDL 示例", MB_ICONWARNING | MB_YESNO);
        if (answer != IDYES) {
            return;
        }
    }
    const io::WriteResult written = io::WriteFileAtomic(path.wstring(), edit::UdlLoader::SampleXml());
    if (!written.ok) {
        ::MessageBoxW(hwnd_, (L"写入失败：\n" + written.message).c_str(), L"UDL 示例",
                      MB_ICONWARNING);
        return;
    }
    ::MessageBoxW(
        hwnd_,
        (L"UDL 示例已写到：\n" + path.wstring() +
         L"\n\n把它改成你自己的语言：name 是稳定标识，ext 是认领的扩展名，"
         L"<Keywords>/<LineComment>/<StringDelim>/<Fold> 各管一件事。"
         L"改完重启生效——用这个扩展名打开文件就能看到效果。")
            .c_str(),
        L"UDL 示例", MB_ICONINFORMATION);
}

void MainWindow::CommandDecodeText(bool base64) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    if (document->buffer.isReadOnly) {
        ::MessageBoxW(hwnd_, L"这份文档是只读打开的，无法改内容。", L"工具",
                      MB_ICONINFORMATION);
        return;
    }
    // 解码会失败（粘贴的内容不合法），所以不能走"永不失败"的变换接口：
    // 先自己解一次，失败就把原因说清楚，成功再交给统一的变换流程写入
    std::string decoded;
    const std::string original = document->view->TextRange(
        document->view->SelectionStart() == document->view->SelectionEnd()
            ? 0
            : std::min(document->view->SelectionStart(), document->view->SelectionEnd()),
        document->view->SelectionStart() == document->view->SelectionEnd()
            ? document->view->ByteLength()
            : std::max(document->view->SelectionStart(), document->view->SelectionEnd()));
    const bool ok = base64 ? edit::Base64Decode(original, decoded) : edit::UrlDecode(original, decoded);
    if (!ok) {
        ::MessageBoxW(hwnd_, base64
                                 ? L"选中的内容不是合法的 Base64（长度必须是 4 的倍数，"
                                   L"且只含 A–Z a–z 0–9 + / 与结尾的 =）。"
                                 : L"选中的内容里有不完整的百分号转义（% 后面要跟两位十六进制）。",
                      L"无法解码", MB_ICONWARNING);
        return;
    }
    ApplyTextTool(
        [base64](std::string_view text, std::uint32_t) {
            std::string out;
            if (base64 ? edit::Base64Decode(text, out) : edit::UrlDecode(text, out)) {
                return out;
            }
            return std::string(text);   // 上面已经检过一次，这里只是兜底：原样返回等于"没有变化"
        },
        base64 ? L"Base64 解码" : L"URL 解码");
}

// JSON / XML 格式化（设计文档 2.13）。
// 与其它文本转换的区别在于"可能失败"：内容不合法时不但不能改文档，
// 还要把"第几行第几列"讲清楚。所以这里先自己格式化一次当校验，
// 成功之后才交给 ApplyTextTool 那条统一路径写入（保证一次操作正好一步撤销）。
void MainWindow::CommandFormatStructured(bool json, bool minify) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    const wchar_t* title = json ? (minify ? L"压缩 JSON" : L"格式化 JSON") : L"美化 XML";
    if (document->buffer.isReadOnly) {
        ::MessageBoxW(hwnd_, L"这份文档是只读打开的，无法改内容。", title,
                      MB_ICONINFORMATION);
        return;
    }
    // 缩进固定 2 空格：本版的样式配置器管的是配色，缩进宽度还没做成可配置项。
    // 做成"每次都问一遍"的对话框只是把麻烦挪个地方，等首选项里有了这一项再读配置。
    constexpr int kIndent = 2;
    const auto format = [json, minify](std::string_view text) {
        return json ? core::FormatJson(text, kIndent, minify)
                    : core::FormatXml(text, kIndent);
    };
    // 取"选区（无选区则整篇）"，与 ApplyTextTool 的取法保持一致
    const int32_t selectionStart = document->view->SelectionStart();
    const int32_t selectionEnd = document->view->SelectionEnd();
    const int32_t start = selectionStart == selectionEnd
                              ? 0
                              : std::min(selectionStart, selectionEnd);
    const int32_t end = selectionStart == selectionEnd
                            ? document->view->ByteLength()
                            : std::max(selectionStart, selectionEnd);
    const std::string original = document->view->TextRange(start, end);

    const core::FormatResult probe = format(original);
    if (!probe.ok) {
        ::MessageBoxW(hwnd_, (probe.error + L"\n文档内容没有改动。").c_str(), title,
                      MB_ICONWARNING);
        infra::LogInfo(L"tools", std::wstring(L"格式化失败：") + probe.error);
        return;
    }
    // 格式化器统一以"一个换行"结尾。被格式化的区域本来没有结尾换行时不要凭空添一行——
    // 用户可能只选了一段对象，后面还紧跟着别的内容。
    const bool hadTailNewline =
        !original.empty() && (original.back() == '\n' || original.back() == '\r');
    ApplyTextTool(
        [format, hadTailNewline](std::string_view text, std::uint32_t) {
            core::FormatResult result = format(text);
            if (!result.ok) {
                return std::string(text);   // 上面已经检过一次，这里只是兜底：原样返回等于"没有变化"
            }
            if (!hadTailNewline && !result.text.empty() && result.text.back() == '\n') {
                result.text.pop_back();
            }
            return result.text;
        },
        title);
}

void MainWindow::CommandRegisterAssociations(bool registerThem) {
    std::wstring exePath(MAX_PATH, L'\0');
    const DWORD written = ::GetModuleFileNameW(nullptr, exePath.data(),
                                              static_cast<DWORD>(exePath.size()));
    exePath.resize(written);
    if (exePath.empty()) {
        ::MessageBoxW(hwnd_, L"取不到程序自身的路径，无法登记文件关联。", L"文件关联",
                      MB_ICONWARNING);
        return;
    }
    constexpr const wchar_t* kClasses = L"Software\\Classes";
    std::wstring error;
    if (registerThem) {
        if (!infra::RegisterAssociations(HKEY_CURRENT_USER, kClasses, exePath, &error)) {
            ::MessageBoxW(hwnd_, (L"加入打开方式列表失败：\n" + error).c_str(), L"文件关联",
                          MB_ICONWARNING);
            return;
        }
    } else {
        infra::UnregisterAssociations(HKEY_CURRENT_USER, kClasses, &error);
    }
    // 通知资源管理器刷新关联缓存，否则"打开方式"里要等一会儿才看得到变化
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    const std::wstring message =
        registerThem
            ? L"已把 notepadcc 登记到常见文本与源码扩展名的「打开方式」列表里。\n\n"
              L"说明：这里**没有**改动默认关联。Windows 8 起默认关联带哈希保护，程序改不动，"
              L"硬写也会被系统重置。想让它成为默认，请在资源管理器里右键该文件 →"
              L"「打开方式」→「选择其他应用」→ 勾选「始终使用此应用打开」。"
            : L"已把 notepadcc 从「打开方式」列表里移除。默认关联不受影响。";
    ::MessageBoxW(hwnd_, message.c_str(), L"文件关联", MB_ICONINFORMATION);
}

void MainWindow::WriteConvertBackup(const std::wstring& path) const {
    if (path.empty()) {
        return;
    }
    const std::wstring backup = path + L".bak";
    if (::GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // 不覆盖已有的备份：最早那一份才是用户最可能想找回来的"原样"
        infra::LogInfo(L"convert", L"已存在转换备份，保留最早的一份: " + backup);
        return;
    }
    if (::CopyFileW(path.c_str(), backup.c_str(), TRUE) == 0) {
        // 备份失败不该阻断转换（转换本身还能转回来），但必须留下痕迹
        infra::LogWarn(L"convert", L"生成转换备份失败（错误码 " +
                                      std::to_wstring(::GetLastError()) + L"）: " + backup);
        return;
    }
    infra::LogInfo(L"convert", L"已生成转换备份: " + backup);
}

void MainWindow::CommandConvertEol(edit::EolMode target) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    if (document->buffer.isReadOnly) {
        ::MessageBoxW(hwnd_, L"这份文档是只读打开的，无法转换行尾。", L"转换为行尾",
                      MB_ICONINFORMATION);
        return;
    }
    if (document->buffer.eol == target) {
        return;   // 已经是这个行尾，不白改一次（那会把文档标成已修改）
    }
    document->view->ConvertLineEndings(target);
    document->buffer.eol = target;
    document->buffer.detectedEol = target;
    document->buffer.isModified = true;
    infra::LogInfo(L"convert", L"行尾已转为 " + edit::EolName(target) + L": " +
                                   document->buffer.displayName);
    SyncTabBar();
    UpdateStatus();
}

void MainWindow::CommandConvertEncoding(edit::UniMode target) {
    edit::Document* document = ActiveDocument();
    if (document == nullptr || document->view == nullptr) {
        return;
    }
    if (document->buffer.isReadOnly) {
        ::MessageBoxW(hwnd_, L"这份文档是只读打开的，无法转换编码。", L"转换编码",
                      MB_ICONINFORMATION);
        return;
    }
    // 目标代码页：本来就按某个代码页解释就用那个，否则用新建文档的默认代码页
    const std::uint32_t codePage = document->buffer.encoding.uniMode == edit::UniMode::Ansi
                                       ? document->buffer.encoding.codePage
                                       : config_.NewFile().defaultCodePage;
    const std::string current = document->view->GetAllText();
    const edit::ConvertResult result =
        edit::ConvertTo(current, document->buffer.encoding, target, codePage);
    if (!result.ok) {
        ::MessageBoxW(hwnd_, result.problem.c_str(), L"无法转换编码", MB_ICONWARNING);
        return;
    }
    if (document->buffer.encoding.uniMode == target &&
        document->buffer.encoding.hasBom == result.encoding.hasBom) {
        return;   // 已经是这个编码，不用改
    }
    // 备份放在改动之前：转换是高风险操作，磁盘上那一份得先留住
    if (config_.Backup().keepConvertBackup) {
        WriteConvertBackup(document->buffer.filePath);
    }
    document->view->SetCodePage(target == edit::UniMode::Ansi ? codePage : edit::kCodePageUtf8);
    if (result.editorBytes != current) {
        // 只有 ANSI 与 UTF-8 之间才真的换字节；UTF-16 那几种缓冲区本来就是 UTF-8
        document->view->SetText(result.editorBytes);
    }
    document->buffer.encoding = result.encoding;
    document->buffer.isModified = true;
    infra::LogInfo(L"convert", L"编码已转为 " + edit::EncodingName(result.encoding) + L": " +
                                   document->buffer.displayName);
    SyncTabBar();
    UpdateStatus();
}

edit::EditView* MainWindow::SecondaryView() const {
    return documents_ != nullptr ? documents_->SplitView() : nullptr;
}

void MainWindow::SyncSplitView() {
    if (documents_ == nullptr) {
        return;
    }
    if (!splitEnabled_) {
        documents_->SplitDisableView();
        dividerRect_ = RECT{0, 0, 0, 0};
        return;
    }
    if (!documents_->SplitEnableView()) {
        splitEnabled_ = false;
        return;
    }
    edit::Document* document = documents_->Active();
    edit::EditView* clone = documents_->SplitView();
    if (document == nullptr || document->view == nullptr || clone == nullptr) {
        return;
    }
    // 文档换了（或刚建出来），克隆视图要重新指向它
    documents_->SplitAttach(*document);
    // 克隆视图是独立的 Scintilla 控件：显示开关、字号缩放、语法着色都得再来一遍，
    // 否则它会用一份默认样式显示同一份文档，看起来像另一个程序。
    ApplyEditConfigToView(*clone);
    clone->ApplyLanguageForPath(document->buffer.filePath, &languages_, &styles_);
    clone->SetFirstVisibleLine(document->view->FirstVisibleLine());
}

void MainWindow::FocusActivePane() {
    if (splitFocusSecondary_) {
        if (edit::EditView* view = SecondaryView(); view != nullptr && view->Valid()) {
            ::SetFocus(view->Handle());
            return;
        }
    }
    if (documents_ != nullptr) {
        if (edit::Document* document = documents_->Active();
            document != nullptr && document->view) {
            ::SetFocus(document->view->Handle());
        }
    }
}

void MainWindow::CommandToggleSplit() {
    splitEnabled_ = !splitEnabled_;
    if (!splitEnabled_) {
        splitFocusSecondary_ = false;
    }
    SyncSplitView();
    LayoutChildren();
    FocusActivePane();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::CommandCyclePane() {
    if (!splitEnabled_ || SecondaryView() == nullptr) {
        return;   // 没分屏就没什么可切的
    }
    splitFocusSecondary_ = !splitFocusSecondary_;
    FocusActivePane();
}

bool MainWindow::PointInDivider(POINT point) const {
    return dividerRect_.right > dividerRect_.left &&
           ::PtInRect(&dividerRect_, point) != 0;
}

void MainWindow::DragDividerTo(int x) {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    if (client.right <= 0) {
        return;
    }
    // 用视图区域的宽度算比例，才能和 LayoutChildren 里的换算对上
    const int top = tabBar_.Height();
    const int width = client.right;
    if (width <= 1 || client.bottom <= top) {
        return;
    }
    int percent = (x * 100) / width;
    percent = std::max(10, std::min(90, percent));
    if (percent == splitRatioPercent_) {
        return;
    }
    const RECT previous = dividerRect_;
    splitRatioPercent_ = percent;
    LayoutChildren();
    ::InvalidateRect(hwnd_, &previous, TRUE);
    ::InvalidateRect(hwnd_, &dividerRect_, TRUE);
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
    // 用外壳持有的那个 store：它手里有当前这份宏表，一起写出去。
    // （用一个临时对象会写出"只有快捷键、没有宏"的文件——那是数据丢失，不是小毛病）
    if (!shortcutStore_.Save(registry_, &error)) {
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
