#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "edit/EditView.h"
#include "infra/Log.h"
#include "infra/Paths.h"
#include "shell/MainWindow.h"

namespace {

struct StartupOptions {
    bool         portable = false;
    std::wstring fileToOpen;
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
        if (!argument.empty() && argument[0] == L'-') {
            continue;   // M0 只认 -portable，其余开关忽略
        }
        if (options.fileToOpen.empty()) {
            options.fileToOpen = argument;
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
    npcc::infra::LogInfo(L"app", L"notepadcc 启动");

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);

    if (!npcc::edit::RegisterEditorClasses(instance)) {
        ::MessageBoxW(nullptr, L"注册编辑控件窗口类失败。", L"notepadcc", MB_ICONERROR);
        return 1;
    }

    npcc::shell::MainWindow window;
    if (!window.RegisterWindowClass(instance) || !window.Create(instance, options.fileToOpen)) {
        ::MessageBoxW(nullptr, L"创建主窗口失败。", L"notepadcc", MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(window.Handle(), showCommand);
    ::UpdateWindow(window.Handle());

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (::TranslateAcceleratorW(window.Handle(), window.Accelerators(), &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    npcc::infra::LogInfo(L"app", L"notepadcc 退出");
    npcc::infra::ShutdownLogging();
    ::CoUninitialize();
    return static_cast<int>(message.wParam);
}
