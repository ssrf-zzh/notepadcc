#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "edit/EditView.h"
#include "infra/CrashGuard.h"
#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/SingleInstance.h"
#include "core/ConfigCenter.h"
#include "license/LicenseService.h"
#include "shell/MainWindow.h"

namespace {

struct StartupOptions {
    bool                      portable = false;
    bool                      crashTest = false;   // 自检开关：故意崩一次，验证崩溃处理
    bool                      multiInstance = false;   // 命令行强制多开（覆盖配置）
    bool                      multiInstanceSet = false;
    std::wstring              licenseFile;         // --license-file <路径>
    std::wstring              licenseImport;       // --license-import <路径>：静默导入
    bool                      licenseInfo = false; // --license-info：输出状态 JSON 后退出
    std::vector<std::wstring> filesToOpen;
};

StartupOptions ParseCommandLine() {
    StartupOptions options;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"-portable" || argument == L"--portable") {
            options.portable = true;
            continue;
        }
        if (argument == L"--crash-test") {
            options.crashTest = true;
            continue;
        }
        if (argument == L"--multi-instance") {
            options.multiInstance = true;
            options.multiInstanceSet = true;
            continue;
        }
        if (argument == L"--license-file") {
            if (i + 1 < argc) {
                options.licenseFile = argv[++i];
            }
            continue;
        }
        if (argument == L"--license-import") {
            if (i + 1 < argc) {
                options.licenseImport = argv[++i];
            }
            continue;
        }
        if (argument == L"--license-info") {
            options.licenseInfo = true;
            continue;
        }
        if (!argument.empty() && argument[0] == L'-') {
            continue;   // M0 只认 -portable，其余开关忽略
        }
        if (options.filesToOpen.size() < 32) {
            options.filesToOpen.push_back(argument);
        }
    }
    ::LocalFree(argv);
    return options;
}

// 无界面命令（--license-info）的输出。程序是 GUI 子系统，从终端启动时默认没有
// 控制台：先试标准输出句柄（管道与重定向就是它），没有再挂到父进程的控制台上。
// 一律按 UTF-8 写：持有人名字常是中文，按 ANSI 写会变成问号。
void WriteStdoutUtf8(std::string_view text) {
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;   // 没有控制台也没有重定向：状态已经写进日志了
    }
    ::SetConsoleOutputCP(CP_UTF8);
    DWORD written = 0;
    ::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const StartupOptions options = ParseCommandLine();

    npcc::infra::InitializePaths(options.portable);
    npcc::infra::InitLogging(npcc::infra::LogDir());
    // 越早安装越好：窗口还没建起来时崩了，同样要留下崩溃报告与转储
    npcc::infra::crashguard::Install(npcc::infra::BackupDir().wstring().c_str());
    npcc::infra::LogInfo(L"app", L"notepadcc 启动");
    // 启动参数记一笔：排查"开关到底生效没有"时不用再靠猜
    {
        std::wstring switches = L"参数: ";
        switches += options.portable ? L"portable " : L"";
        switches += options.crashTest ? L"crash-test " : L"";
        switches += options.multiInstance ? L"multi-instance " : L"";
        switches += options.licenseInfo ? L"license-info " : L"";
        switches += options.licenseImport.empty() ? L"" : L"license-import ";
        switches += options.licenseFile.empty() ? L"" : L"license-file ";
        switches += L"文件 " + std::to_wstring(options.filesToOpen.size()) + L" 个";
        npcc::infra::LogInfo(L"app", switches);
    }

    std::filesystem::path licenseFile;
    if (!options.licenseFile.empty()) {
        licenseFile = options.licenseFile;
    }
    const std::filesystem::path* const licenseOverride =
        licenseFile.empty() ? nullptr : &licenseFile;
    npcc::license::LicenseService& license = npcc::license::LicenseService::Instance();

    // 两个无界面命令要在单实例判定与建窗口之前处理：企业批量脚本不希望因为
    // "机器上正开着一个编辑器"就被转交给别的实例。
    if (options.licenseInfo) {
        license.Initialize(licenseOverride);
        license.BeginSession();   // 试用记账（B7）：--license-info 要能看到剩余天数
        WriteStdoutUtf8(license.StatusJson());
        npcc::infra::ShutdownLogging();
        return (license.State() == npcc::license::LicenseState::Free) ? 1 : 0;
    }
    if (!options.licenseImport.empty()) {
        license.Initialize(licenseOverride);
        license.BeginSession();   // 每次启动都记一笔（B7）
        npcc::license::LicenseError importError = npcc::license::LicenseError::None;
        std::wstring importDetail;
        const bool imported = license.Import(options.licenseImport, &importError, &importDetail);
        if (imported) {
            npcc::infra::LogInfo(L"license", importDetail);
        } else {
            npcc::infra::LogWarn(L"license", importDetail);
        }
        npcc::infra::ShutdownLogging();
        return imported ? 0 : 2;
    }

    // 许可初始化（设计 8.3）：只读本地文件 + 验签 + 机器判定，失败仅记日志，
    // 绝不阻断启动。放在建窗口之前，界面一起来就能问到状态。
    license.Initialize(licenseOverride);
    license.BeginSession();   // 试用双写 + 上次运行时间（B7，设计 5.1 / 5.2）

    // 单实例判定必须在建窗口之前：晚一步就可能出现"两个窗口同时起来"。
    // 配置要读一次才知道用户选的是单开还是多开——这里单独加载一份配置，
    // 只取这一个开关；主窗口那边还会再加载一次（文件很小，读两遍没有负担）。
    bool multiInstance = options.multiInstance;
    if (!options.multiInstanceSet) {
        npcc::core::ConfigCenter early;
        early.Load(nullptr);
        multiInstance = early.General().multiInstance;
    }
    if (!npcc::infra::ClaimSingleInstance(npcc::shell::MainWindow::kClassName, options.filesToOpen,
                                          multiInstance)) {
        // 参数已经交给已在跑的那个实例了，本进程的活干完了
        npcc::infra::LogInfo(L"app", L"已有实例在运行，本次启动只做文件转交");
        npcc::infra::ShutdownLogging();
        return 0;
    }

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&controls);

    if (!npcc::edit::RegisterEditorClasses(instance)) {
        ::MessageBoxW(nullptr, L"注册编辑控件窗口类失败。", L"notepadcc", MB_ICONERROR);
        return 1;
    }
    if (!npcc::shell::TabBar::RegisterWindowClass(instance)) {
        ::MessageBoxW(nullptr, L"注册标签栏窗口类失败。", L"notepadcc", MB_ICONERROR);
        return 1;
    }

    npcc::shell::MainWindow window;
    window.SetCrashTest(options.crashTest);
    if (!window.RegisterWindowClass(instance) || !window.Create(instance, options.filesToOpen)) {
        ::MessageBoxW(nullptr, L"创建主窗口失败。", L"notepadcc", MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(window.Handle(), showCommand);
    ::UpdateWindow(window.Handle());

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        // 全屏里按 Esc 退出（设计文档 2.4 第 6 条）。这件事只能在这里拦：编辑控件拿着焦点，
        // WM_KEYDOWN 根本不会送到主窗口去；而把 Esc 绑成全局快捷键又会在非全屏时反过来
        // 触发全屏。所以只在"确实处于全屏"时才吞掉这个键。
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
            window.ExitFullScreenOnEscape()) {
            continue;
        }
        // 快捷键可能被用户全部清空，这时 hAccel 是 NULL：
        // 不能把 NULL 递给 TranslateAccelerator，直接走普通分发。
        const HACCEL accelerators = window.Accelerators();
        if (accelerators == nullptr ||
            ::TranslateAcceleratorW(window.Handle(), accelerators, &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    npcc::infra::LogInfo(L"app", L"notepadcc 退出");
    npcc::infra::ShutdownLogging();
    ::CoUninitialize();
    return static_cast<int>(message.wParam);
}
