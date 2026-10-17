#pragma once

#include <string>
#include <string_view>

namespace npcc::license {

// 在线激活客户端（B6）：《notepadcc许可与激活设计.md》第 6 章的客户端侧契约。
// 服务端与主程序完全解耦（服务由 B11 另建）；这里只做"请求怎么发、响应怎么解释"，
// 落盘交给 LicenseService::ImportBytes（先验签再写盘）。
//
// 三条纪律（设计 6.1）：超时 10 秒、只重试一次；任何网络失败都不影响程序可用，
// 一律降级为"改用离线激活"；激活成功后的界面刷新由调用方负责。
//
// 为什么 JSON 读写自己来（而不是用 core/JsonFlat）：license 模块不反向依赖 core
// （分层约定）；协议只有平铺字符串字段，这个小扫描器就够用，且支持 \uXXXX 转义
// （服务端若用 Python 的 json.dumps，中文默认就是转义写出去的）。

enum class ActivationError {
    None,
    NotConfigured,        // 没有配置激活服务地址（本地判断，一个字节都不发）
    NetworkFailed,        // 连不上 / 超时（已按纪律重试一次）
    KeyNotFound,          // KEY_NOT_FOUND：授权码不存在
    KeyExhausted,         // KEY_EXHAUSTED：激活次数超出上限
    KeyDisabled,          // KEY_DISABLED：许可被停用
    FingerprintMismatch,  // FINGERPRINT_MISMATCH：与已有绑定冲突
    ServerError,          // SERVER_ERROR 或 5xx：服务端异常
    BadResponse,          // 200 但报文解释不了（不是 JSON / 缺 license / Base64 坏）
};

// 机器可读（与协议错误码对齐，日志与脚本比对用）
std::string ActivationErrorName(ActivationError error);
// 用户看的文案（设计第 6 章"客户端提示要点"逐条对应）
std::wstring DescribeActivationError(ActivationError error);

struct ActivateInput {
    std::string licenseKey;        // 用户输入的授权码（内部先归一化）
    std::string fingerprintText;   // 机器短码；采集由调用方做，业务单测不必动机器
    std::string appVersion;        // "1.0.0"
    std::string osVersion;         // "10.0.22631"
    std::string computerName;      // 计算机名；请求里只发它的哈希前 8 位（设计 4.3）
};

struct ActivationResult {
    ActivationError error = ActivationError::None;
    std::string     licenseBytes;   // 成功：Base64 解出来的许可容器原文
    std::wstring    message;        // 服务端的 message（可为空）
    std::wstring    detail;         // 失败细节（HTTP 状态、网络原因、解析原因）

    bool ok() const { return error == ActivationError::None; }
};

// 组请求体（纯函数，字段逐条对齐设计第 6 章表格）
std::string BuildActivateRequestBody(const ActivateInput& input, const std::string& requestId,
                                     const std::string& machineHash);

// 解释响应（纯函数）：status 是 HTTP 状态码，body 是响应体
ActivationResult ParseActivateResponse(unsigned status, const std::string& body);

// 扁平 JSON 的字符串字段读取（测试与解析共用）：支持 \" \\ \/ \b \f \n \r \t 与
// \uXXXX（含代理对）。找不到键、值不是字符串都返回 false。
bool JsonStringField(const std::string& json, std::string_view key, std::string& value);

// 归一化授权码：去空白 + 转大写（设计里 KEY_NOT_FOUND 的提示要点），组请求时用它
std::string NormalizeLicenseKey(std::string_view key);

// 计算机名的哈希：SHA-256 的前 8 位十六进制（设计 4.3 隐私口径）
std::string MachineHashFromComputerName(std::string_view computerName);

// 本机 OS 版本串（如 "10.0.22631"）；读不到时返回 "unknown"
std::string CurrentOsVersion();

class ActivationClient {
public:
    explicit ActivationClient(std::string serviceUrl);   // UTF-8；空 = 未配置

    ActivationResult Activate(const ActivateInput& input) const;

private:
    std::string serviceUrl_;
};

}  // namespace npcc::license
