#include "shell/MainWindow.h"

#include <commdlg.h>
#include <shellapi.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "infra/Log.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"
#include "shell/MenuBuilder.h"

namespace npcc::shell {
namespace {

// 命令 ID：高 4 位类别 + 低 12 位序号（见 core/Ids.h 的 MakeCmdId）
enum : npcc::core::CmdId {
    kCmdFileNew    = npcc::core::MakeCmdId(1, 1),
    kCmdFileOpen   = npcc::core::MakeCmdId(1, 2),
    kCmdFileSave   = npcc::core::MakeCmdId(1, 3),
    kCmdFileSaveAs = npcc::core::MakeCmdId(1, 4),
    kCmdFileExit   = npcc::core::MakeCmdId(1, 5),

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

    kCmdHelpAbout = npcc::core::MakeCmdId(12, 1),

    // Scintilla 原生命令：不新增语义，直接转发
    kSciRedo      = 2011,
    kSciSelectAll = 2013,
    kSciUndo      = 2176,
    kSciCut       = 2177,
    kSciCopy      = 2178,
    kSciPaste     = 2179,
};

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace

MainWindow::~MainWindow() {
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

bool MainWindow::Create(HINSTANCE instance, const std::wstring& initialFile) {
    instance_ = instance;
    hwnd_ = ::CreateWindowExW(0, kClassName, L"notepadcc", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                              nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    if (!initialFile.empty()) {
        OpenPath(initialFile);
    }
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
            view_.Create(hwnd_, 1);
            view_.ApplyLightTheme();
            statusBar_.Create(hwnd_, 2);

            RegisterCommands();
            menu_ = BuildMainMenu(registry_);
            if (menu_ != nullptr) {
                ::SetMenu(hwnd_, menu_);
            }
            RebuildAccelerators();

            ::DragAcceptFiles(hwnd_, TRUE);
            ResetDocument();
            LayoutChildren();
            ::SetFocus(view_.Handle());
            return 0;
        }
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_SETFOCUS:
            ::SetFocus(view_.Handle());
            return 0;
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
            if (header->hwndFrom != view_.Handle()) {
                break;
            }
            edit::EditorNotification note;
            if (!edit::EditView::TranslateNotify(header, note)) {
                break;
            }
            switch (note.event) {
                case edit::EditorEvent::UpdateUI:
                    UpdateStatus();
                    break;
                case edit::EditorEvent::SavePointLeft:
                    doc_.isModified = true;
                    UpdateTitle();
                    UpdateStatus();
                    break;
                case edit::EditorEvent::SavePointReached:
                    doc_.isModified = false;
                    UpdateTitle();
                    UpdateStatus();
                    break;
                default:
                    break;
            }
            break;
        }
        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            if (::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0) {
                std::vector<wchar_t> buffer(4096, L'\0');
                if (::DragQueryFileW(drop, 0, buffer.data(),
                                     static_cast<UINT>(buffer.size())) > 0) {
                    OpenPath(buffer.data());
                }
            }
            ::DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            if (ConfirmDiscardChanges()) {
                ::DestroyWindow(hwnd_);
            }
            return 0;
        case WM_DESTROY:
            view_.Destroy();
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

core::CmdContext MainWindow::Context() {
    core::CmdContext context;
    context.buffer = &doc_;
    context.view = &view_;
    return context;
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
    add(kCmdFileExit, CmdCategory::File, L"cmd.file.exit", L"退出(&X)",
        core::kCmdNone, [this](const CmdContext&) { ::PostMessageW(hwnd_, WM_CLOSE, 0, 0); });

    // ---- 编辑 ----
    add(kCmdEditUndo, CmdCategory::Edit, L"cmd.edit.undo", L"撤销(&U)",
        needsWritable, [this](const CmdContext&) { view_.Send(kSciUndo); });
    add(kCmdEditRedo, CmdCategory::Edit, L"cmd.edit.redo", L"重做(&R)",
        needsWritable, [this](const CmdContext&) { view_.Send(kSciRedo); });
    add(kCmdEditCut, CmdCategory::Edit, L"cmd.edit.cut", L"剪切(&T)",
        needsWritable, [this](const CmdContext&) { view_.Send(kSciCut); });
    add(kCmdEditCopy, CmdCategory::Edit, L"cmd.edit.copy", L"复制(&C)",
        needsBuffer, [this](const CmdContext&) { view_.Send(kSciCopy); });
    add(kCmdEditPaste, CmdCategory::Edit, L"cmd.edit.paste", L"粘贴(&P)",
        needsWritable, [this](const CmdContext&) { view_.Send(kSciPaste); });
    add(kCmdEditSelectAll, CmdCategory::Edit, L"cmd.edit.selectAll", L"全选(&L)",
        needsBuffer, [this](const CmdContext&) { view_.Send(kSciSelectAll); });

    // ---- 视图 ----
    add(kCmdViewZoomIn, CmdCategory::View, L"cmd.view.zoomIn", L"放大(&I)",
        needsBuffer, [this](const CmdContext&) { view_.SetZoom(view_.ZoomLevel() + 1); });
    add(kCmdViewZoomOut, CmdCategory::View, L"cmd.view.zoomOut", L"缩小(&O)",
        needsBuffer, [this](const CmdContext&) { view_.SetZoom(view_.ZoomLevel() - 1); });
    add(kCmdViewZoomReset, CmdCategory::View, L"cmd.view.zoomReset", L"恢复默认缩放(&Z)",
        needsBuffer, [this](const CmdContext&) { view_.SetZoom(0); });
    add(kCmdViewToggleWrap, CmdCategory::View, L"cmd.view.toggleWrap", L"自动换行(&W)",
        core::kCmdCheckable, [this](const CmdContext&) {
            wrapEnabled_ = !wrapEnabled_;
            view_.SetWrapMode(wrapEnabled_);
        });
    add(kCmdViewToggleSpaces, CmdCategory::View, L"cmd.view.toggleSpaces", L"显示空白符号(&S)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showWhitespace_ = !showWhitespace_;
            view_.SetShowWhitespace(showWhitespace_);
        });
    add(kCmdViewToggleEol, CmdCategory::View, L"cmd.view.toggleEol", L"显示行尾符(&E)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showEol_ = !showEol_;
            view_.SetShowEol(showEol_);
        });
    add(kCmdViewToggleNumbers, CmdCategory::View, L"cmd.view.toggleNumbers", L"显示行号(&N)",
        core::kCmdCheckable, [this](const CmdContext&) {
            showLineNumbers_ = !showLineNumbers_;
            view_.ShowLineNumbers(showLineNumbers_);
        });

    // ---- 帮助 ----
    add(kCmdHelpAbout, CmdCategory::Help, L"cmd.help.about", L"关于 notepadcc(&A)",
        core::kCmdNone, [this](const CmdContext&) { CommandAbout(); });

    // 勾选态统一由命令注册表回答，视图不参与判断（设计文档 1.3 规则 1）
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

    // ---- 默认快捷键（全部可在 M1 的"管理快捷键"中重绑） ----
    const uint8_t ctrl = core::kModCtrl;
    const uint8_t ctrlShift = core::kModCtrl | core::kModShift;
    const uint8_t ctrlAlt = core::kModCtrl | core::kModAlt;
    registry_.BindShortcut(kCmdFileNew, ctrl, 'N', true);
    registry_.BindShortcut(kCmdFileOpen, ctrl, 'O', true);
    registry_.BindShortcut(kCmdFileSave, ctrl, 'S', true);
    registry_.BindShortcut(kCmdFileSaveAs, ctrlShift, 'S', true);
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
}

void MainWindow::RebuildAccelerators() {
    std::vector<ACCEL> entries;
    for (const auto& binding : registry_.AllBindings()) {
        if (binding.cmdId > 0xFFFF) {
            continue;   // ACCEL.cmd 是 WORD，超范围的命令不进加速键表
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
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    statusBar_.Resize(client);
    RECT viewRect = client;
    viewRect.bottom = client.bottom - statusBar_.Height();
    if (viewRect.bottom < 0) {
        viewRect.bottom = 0;
    }
    view_.Resize(viewRect);
}

void MainWindow::UpdateTitle() const {
    std::wstring title = L"notepadcc - ";
    title += doc_.isUntitled ? L"新建文档" : doc_.displayName;
    if (doc_.isModified) {
        title += L" *";
    }
    ::SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::UpdateStatus() const {
    statusBar_.SetPosition(view_.CurrentLine(), view_.CurrentColumn(),
                           view_.SelectionByteCount());
    statusBar_.SetDocumentInfo(edit::EncodingName(doc_.encoding), edit::EolName(doc_.eol),
                               view_.CurrentLanguageName(), doc_.isModified);
}

void MainWindow::ResetDocument() {
    doc_ = edit::DocBuffer{};
    doc_.id = static_cast<core::BufferId>(nextBufferId_++);
    doc_.isUntitled = true;
    doc_.isModified = false;
    doc_.encoding.uniMode = edit::UniMode::Utf8;
    doc_.encoding.confidence = 100;
    doc_.eol = edit::EolMode::Crlf;

    view_.SetCodePage(edit::kCodePageUtf8);
    view_.SetEolMode(doc_.eol);
    view_.SetReadOnly(false);
    view_.SetText({});
    view_.ApplyLanguageForPath(std::wstring());
    view_.ClearUndoBuffer();
    view_.MarkSavePoint();

    UpdateTitle();
    UpdateStatus();
}

bool MainWindow::OpenPath(const std::wstring& path) {
    const io::ReadResult read = io::ReadWholeFile(path);
    if (!read.ok) {
        infra::LogWarn(L"file", L"打开失败: " + path + L" / " + read.message);
        ::MessageBoxW(hwnd_, read.message.c_str(), L"打开文件失败", MB_ICONWARNING);
        return false;
    }

    const edit::LoadResult loaded = edit::DetectAndPrepare(read.bytes, doc_.encoding.codePage);

    doc_.filePath = path;
    doc_.displayName = FileNameOf(path);
    doc_.isUntitled = false;
    doc_.isReadOnly = false;
    doc_.encoding = loaded.encoding;
    doc_.eol = loaded.eol;
    doc_.detectedEol = loaded.eol;

    view_.SetCodePage(doc_.encoding.uniMode == edit::UniMode::Ansi ? doc_.encoding.codePage
                                                                 : edit::kCodePageUtf8);
    view_.SetEolMode(doc_.eol);
    view_.SetText(loaded.editorBytes);
    view_.ApplyLanguageForPath(path);
    view_.ClearUndoBuffer();
    view_.MarkSavePoint();
    view_.SetCaretPosition(0);
    view_.SetFirstVisibleLine(0);

    doc_.isModified = false;
    lastDirectory_ = DirectoryOf(path);

    infra::LogInfo(L"file", L"已打开: " + path);
    UpdateTitle();
    UpdateStatus();
    return true;
}

bool MainWindow::SaveToPath(const std::wstring& path) {
    std::string editorBytes = view_.GetAllText();
    if (doc_.eol != edit::EolMode::Mixed) {
        editorBytes = edit::NormalizeEol(editorBytes, doc_.eol);
    }
    const std::string outBytes = edit::EncodeForSave(editorBytes, doc_.encoding);
    if (outBytes.empty() && !editorBytes.empty()) {
        ::MessageBoxW(hwnd_, L"按当前编码转换内容失败，文件未写入。", L"保存失败", MB_ICONERROR);
        return false;
    }

    const io::WriteResult result = io::WriteFileAtomic(path, outBytes);
    if (!result.ok) {
        infra::LogError(L"file", L"保存失败: " + path + L" / " + result.message);
        ::MessageBoxW(hwnd_, result.message.c_str(), L"保存失败", MB_ICONERROR);
        return false;
    }

    doc_.filePath = path;
    doc_.displayName = FileNameOf(path);
    doc_.isUntitled = false;
    doc_.isModified = false;
    lastDirectory_ = DirectoryOf(path);
    view_.MarkSavePoint();

    infra::LogInfo(L"file", L"已保存: " + path);
    UpdateTitle();
    UpdateStatus();
    return true;
}

void MainWindow::CommandNew() {
    if (ConfirmDiscardChanges()) {
        ResetDocument();
    }
}

void MainWindow::CommandOpen() {
    if (!ConfirmDiscardChanges()) {
        return;
    }
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
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (::GetOpenFileNameW(&dialog) != 0) {
        OpenPath(buffer.data());
    }
}

bool MainWindow::CommandSave() {
    if (!doc_.HasPath()) {
        return CommandSaveAs();
    }
    return SaveToPath(doc_.filePath);
}

bool MainWindow::CommandSaveAs() {
    std::vector<wchar_t> buffer(32768, L'\0');
    if (!doc_.displayName.empty()) {
        std::wcsncpy(buffer.data(), doc_.displayName.c_str(), buffer.size() - 1);
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
    return SaveToPath(buffer.data());
}

void MainWindow::CommandAbout() {
    ::MessageBoxW(hwnd_,
                  L"notepadcc 1.0.0（M0 骨架版）\n\n"
                  L"原生 Win32 + Scintilla 的轻量文本 / 源码编辑器。\n"
                  L"已完成：多编码打开与保存、原子写盘、语法着色、"
                  L"命令注册表与可重绑快捷键。",
                  L"关于 notepadcc", MB_ICONINFORMATION);
}

bool MainWindow::ConfirmDiscardChanges() {
    if (!doc_.isModified) {
        return true;
    }
    const int answer = ::MessageBoxW(hwnd_, L"当前文档尚未保存，是否保存？", L"notepadcc",
                                     MB_ICONQUESTION | MB_YESNOCANCEL);
    if (answer == IDYES) {
        return CommandSave();
    }
    return answer != IDCANCEL;
}

}  // namespace npcc::shell
