#include "infra/SingleInstance.h"

#include <mutex>

#include "infra/Log.h"

namespace npcc::infra {
namespace {

constexpr const wchar_t* kMutexName = L"Local\\notepadcc-single-instance";
constexpr const wchar_t* kRelayClassName = L"notepadccSingleInstanceRelay";

std::mutex                g_mutex;
std::vector<std::wstring> g_pending;
HWND                      g_target = nullptr;
HWND                      g_relay = nullptr;
HANDLE                    g_instanceMutex = nullptr;

// WM_COPYDATA 的载荷：一串以 NUL 分隔、以空串收尾的宽字符路径
void AppendPaths(std::vector<std::wstring>& out, const char* bytes, std::size_t size) {
    const wchar_t* cursor = reinterpret_cast<const wchar_t*>(bytes);
    const wchar_t* end = reinterpret_cast<const wchar_t*>(bytes + size);
    while (cursor < end && *cursor != L'\0') {
        const std::wstring path(cursor);
        cursor += path.size() + 1;
        if (!path.empty()) {
            out.push_back(path);
        }
    }
}

LRESULT CALLBACK RelayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->lpData == nullptr || data->cbData == 0) {
                return FALSE;
            }
            std::vector<std::wstring> received;
            AppendPaths(received, static_cast<const char*>(data->lpData), data->cbData);
            if (received.empty()) {
                return TRUE;
            }
            HWND target = nullptr;
            {
                std::lock_guard<std::mutex> guard(g_mutex);
                for (const std::wstring& path : received) {
                    g_pending.push_back(path);
                }
                target = g_target;
            }
            LogInfo(L"instance", L"收到另一个实例转交的 " +
                                     std::to_wstring(received.size()) + L" 个文件");
            if (target != nullptr) {
                // 用 PostMessage 而不是在这里同步开工：对方可能在等我们的回复，
                // 打开文件这种事不该堵在消息处理里。
                ::PostMessageW(target, WM_APP + 1 /* kMsgOpenFiles */, 0, 0);
            }
            // 返回 TRUE 让系统知道我们收下了这次数据
            return TRUE;
        }
        case WM_DESTROY:
            g_relay = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void CreateRelayWindow() {
    if (g_relay != nullptr) {
        return;
    }
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &RelayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kRelayClassName;
    ::RegisterClassExW(&wc);
    // 普通隐藏窗口，**不能用 HWND_MESSAGE**：消息专用窗口无法被 FindWindow 找到，
    // 而另一个实例正是靠 FindWindow 找它。不显示、不进任务栏就够了。
    g_relay = ::CreateWindowExW(0, kRelayClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance,
                                nullptr);
    if (g_relay == nullptr) {
        LogWarn(L"instance", L"创建单实例交接窗口失败，多开时文件会交给新的实例");
    }
}

bool HandOver(const std::wstring& className, const std::vector<std::wstring>& files) {
    // 交接窗口比主窗口建得早，所以"找得到交接窗口"就是对方已经起来的可靠信号。
    // 第一个实例可能还在启动，等一小会儿：双击文件是最常见的用法，
    // 多等几百毫秒，比"刚启动完又开一个窗口"要好得多。
    HWND relay = nullptr;
    for (int attempt = 0; attempt < 30 && relay == nullptr; ++attempt) {
        relay = ::FindWindowW(kRelayClassName, nullptr);
        if (relay == nullptr) {
            ::Sleep(100);
        }
    }
    if (relay == nullptr) {
        LogWarn(L"instance", L"已有实例在运行但找不到它的交接窗口，本次不转交");
        return false;
    }
    if (files.empty()) {
        // 没有文件要开，只把对方的窗口提到前面来
        if (HWND target = ::FindWindowW(className.c_str(), nullptr); target != nullptr) {
            ::PostMessageW(target, WM_APP + 7 /* kMsgAppActivate */, 0, 0);
        }
        return true;
    }
    const std::string payload = PackPaths(files);
    COPYDATASTRUCT data{};
    data.dwData = 1;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = const_cast<char*>(payload.data());
    DWORD_PTR ignored = 0;
    const LRESULT sent = ::SendMessageTimeoutW(relay, WM_COPYDATA,
                                               reinterpret_cast<WPARAM>(relay),
                                               reinterpret_cast<LPARAM>(&data),
                                               SMTO_ABORTIFHUNG, 5000, &ignored);
    if (sent == 0) {
        LogWarn(L"instance", L"把文件转交给已有实例失败");
        return false;
    }
    LogInfo(L"instance", L"已把 " + std::to_wstring(files.size()) + L" 个文件转交给已有实例");
    return true;
}

}  // namespace

std::string PackPaths(const std::vector<std::wstring>& paths) {
    std::string bytes;
    for (const std::wstring& path : paths) {
        if (path.empty()) {
            continue;
        }
        bytes.append(reinterpret_cast<const char*>(path.data()), path.size() * sizeof(wchar_t));
        bytes.append(sizeof(wchar_t), '\0');   // 分隔（也是结尾）
    }
    bytes.append(sizeof(wchar_t), '\0');       // 空串收尾：解析到这里停
    return bytes;
}

std::vector<std::wstring> UnpackPaths(std::string_view bytes) {
    std::vector<std::wstring> paths;
    if (bytes.size() >= sizeof(wchar_t)) {
        AppendPaths(paths, bytes.data(), bytes.size());
    }
    return paths;
}

bool ClaimSingleInstance(const std::wstring& className, const std::vector<std::wstring>& files,
                         bool multiInstance) {
    if (multiInstance) {
        return true;   // 用户明确要求多开，谁也不抢
    }
    // 具名互斥体是判断"有没有别的实例"最省事的办法：进程退出时内核自动释放，
    // 所以不存在"上次崩溃留下的锁"这种问题。
    const HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex == nullptr) {
        LogWarn(L"instance", L"创建单实例互斥体失败，本次按多开处理");
        return true;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(mutex);
        HandOver(className, files);
        return false;
    }
    g_instanceMutex = mutex;   // 持有到进程结束，故意不释放
    CreateRelayWindow();
    return true;
}

void SetSingleInstanceTarget(HWND mainWindow) {
    bool hasPending = false;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_target = mainWindow;
        hasPending = !g_pending.empty();
    }
    if (hasPending) {
        // 交接发生在主窗口建好之前（程序刚启动时被双击了文件），补一次投递
        ::PostMessageW(mainWindow, WM_APP + 1 /* kMsgOpenFiles */, 0, 0);
    }
}

std::vector<std::wstring> TakePendingFiles() {
    std::lock_guard<std::mutex> guard(g_mutex);
    std::vector<std::wstring> taken;
    taken.swap(g_pending);
    return taken;
}

}  // namespace npcc::infra
