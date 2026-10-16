/*
 * notepadcc 示例插件（设计文档 2.10）。
 *
 * 它做三件小事，正好把宿主的几项服务都走一遍：
 *   1. 注册一条命令"插入问候语"——回放时把一段固定文本插到光标处；
 *   2. 在 Init 里问宿主"配置目录在哪"、"当前文档有多少字节"，写进日志；
 *   3. 在 Shutdown 里写一条日志，证明宿主确实按顺序调了它。
 *
 * 编译成 x64 DLL 即可，不需要跟宿主用同一个编译器（接口是纯 C 的）。
 */

#include "notepadcc_plugin.h"

#include <string>

// 默认就用当前 ABI；构建脚本会给"故意写错版本"的那个变体传别的值，
// 用来验证宿主会拒载并说清原因
#ifndef NPCC_SAMPLE_ABI
#define NPCC_SAMPLE_ABI NPCC_PLUGIN_ABI_VERSION
#endif

namespace {

const NpccHostServices* g_host = nullptr;

void InsertGreeting(void*) {
    if (g_host == nullptr || g_host->SetEditorText == nullptr) {
        return;
    }
    const std::string utf8 = "// 来自 hello 插件\n";
    g_host->SetEditorText(g_host->hostCookie, utf8.data(), utf8.size());
}

NpccCommandDesc g_command = {L"hello.insert", L"插入问候语(&G)", L"插件", &InsertGreeting,
                             nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) const NpccPluginInfo* NpccPlugin_GetInfo(void) {
    static const NpccPluginInfo info = {
        NPCC_SAMPLE_ABI,
        L"hello",
        L"1.0.0",
        L"notepadcc 示例",
        L"示例插件：注册一条命令，把一段话插到光标处",
    };
    return &info;
}

__declspec(dllexport) int NpccPlugin_Init(const NpccHostServices* host) {
    if (host == nullptr) {
        return 1;
    }
    // **双向校验**：插件也要检查宿主那张表（设计文档 2.10 第 2 条）。
    // 少了这一步，宿主换了布局而插件照旧用，就会读到野指针
    if (host->abiVersion != NPCC_PLUGIN_ABI_VERSION ||
        host->size < sizeof(NpccHostServices)) {
        return 2;
    }
    g_host = host;
    if (host->RegisterCommand != nullptr) {
        host->RegisterCommand(host->hostCookie, &g_command);
    }
    if (host->Log != nullptr) {
        wchar_t path[512] = {};
        const size_t written =
            host->GetConfigPath != nullptr
                ? host->GetConfigPath(host->hostCookie, path, sizeof(path) / sizeof(path[0]))
                : 0;
        (void)written;
        std::wstring text = L"hello 插件已初始化，配置目录 = ";
        text += path;
        host->Log(host->hostCookie, 2, text.c_str());
        if (host->GetEditorText != nullptr) {
            const size_t bytes = host->GetEditorText(host->hostCookie, nullptr, 0);
            const std::wstring size = L"当前文档 " + std::to_wstring(bytes) + L" 字节";
            host->Log(host->hostCookie, 2, size.c_str());
        }
    }
    return 0;
}

__declspec(dllexport) void NpccPlugin_Shutdown(void) {
    if (g_host != nullptr && g_host->Log != nullptr) {
        g_host->Log(g_host->hostCookie, 2, L"hello 插件已关闭");
    }
    g_host = nullptr;
}

}  // extern "C"
