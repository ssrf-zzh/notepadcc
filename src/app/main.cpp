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
#include "shell/MainWindow.h"

namespace {

struct StartupOptions {
    bool                      portable = false;
    bool                      crashTest = false;   // 自检开关：故意崩一次，验证崩溃处理
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
