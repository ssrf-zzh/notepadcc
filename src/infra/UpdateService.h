#pragma once

#include <string>
#include <string_view>

namespace npcc::infra {

// 更新检查（设计文档 6.7 第 1 条）：下载清单 → 验签 → 比版本 → 下载包 → SHA-256 →
// 解压 → 对包里的可执行文件做 Authenticode 校验。
//
// 为什么在 infra 而不是 core：这一层全是系统调用（WinHTTP / tar.exe / WinVerifyTrust），
// 没有能脱离系统单测的纯逻辑；纯逻辑在 core/UpdateManifest（清单、版本、源策略）与
// core/UpdateVerify（验签、信任错误码）里，各配了单元测试。
//
// **代理**：WinHTTP 会话用 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 打开——系统/IE 里
// 配的代理（含 PAC/WPAD）自动生效；失败再退回 DEFAULT_PROXY。开着代理时把地址写进日志。
//
// **不做的事（写清楚）**：不自动覆盖正在运行的 exe（Windows 上也做不到），
// 包解压到配置目录下，界面只提供"打开目录"，替换由用户手动完成。
struct UpdateCheckOptions {
    std::string sourceUrl;       // UTF-8；末尾有没有 '/' 都行，内部自己补
    std::string currentVersion;  // 形如 "1.0.0"
};

struct UpdateCheckReport {
    enum class Status { UpToDate, Available, Error };
    Status       status = Status::Error;
    std::string  version;      // 远端版本（拿到合法清单时填）
    std::string  notes;        // 远端说明（UTF-8 原文）
    std::wstring packageDir;   // Available：解压出来的目录（绝对路径）
    std::wstring summary;      // 给界面/状态栏的一句话
    std::wstring detail;       // Error：具体原因（回归脚本按它里面的关键词断言）
};

// 阻塞执行一次完整检查（网络等待最多几十秒）。**只允许在工作线程调用**。
UpdateCheckReport RunUpdateCheck(const UpdateCheckOptions& options);

// UTF-8 ↔ UTF-16 转换（配置里存宽字符，URL 与报文按字节走）。
std::string  Utf8FromWide(std::wstring_view text);
std::wstring WideFromUtf8(std::string_view text);

}  // namespace npcc::infra
