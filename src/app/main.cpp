#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "edit/EditView.h"
#include "infra/CrashGuard.h"
#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/SingleInstance.h"
#include "core/ConfigCenter.h"
#include "shell/MainWindow.h"

namespace {

struct StartupOptions {
    bool                      portable = false;
    bool                      crashTest = false;   // 自检开关：故意崩一次，验证崩溃处理
    bool                      multiInstance = false;   // 命令行强制多开（覆盖配置）
    bool                      multiInstanceSet = false;
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
        switches += L"文件 " + std::to_wstring(options.filesToOpen.size()) + L" 个";
        npcc::infra::LogInfo(L"app", switches);
    }

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
