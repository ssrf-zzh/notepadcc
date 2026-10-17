#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace npcc::infra {

// HTTP 客户端（B2）：WinHTTP 薄封装 + 可单测的 URL 解析。
// 设计依据《notepadcc许可与激活设计.md》8.1/8.2：许可的在线激活（ActivationClient）
// 要发 POST JSON；更新检查原先自己写了一整套 WinHTTP 代码，这次收拢到这一层——
// 一处实现、两处使用，行为（代理、超时、回环、重定向策略）只有一份。
//
// 不引入第三方库：WinHTTP 是系统组件（设计文档 1.2 的选型原则，许可模块尤其
// 不许拖依赖进来）。
//
// 分层：URL 解析是**纯字符串逻辑**（脱离网络可单测）；真正的收发在 HttpSend 里走
// WinHTTP，单元测试用进程内回环小服务器做端到端。
// 代理：回环地址（127.0.0.1 / localhost / ::1）绝不过代理——本地测试源不该被机器
// 上的代理配置插一脚；其余地址用系统/IE 代理（含 PAC/WPAD），失败退回默认设置。

// ----- URL 解析（纯函数）-----
struct HttpUrl {
    bool        valid = false;    // 解析成功才有后几项
    bool        secure = false;   // https
    bool        loopback = false; // 127.0.0.1 / localhost / [::1]
    std::string scheme;           // 小写后的 "http" / "https"
    std::string host;             // 小写化；IPv6 不带方括号
    unsigned    port = 0;         // 省略端口时补 80 / 443
    std::string pathAndQuery;     // 至少 "/"；"http://h?x=1" 会补成 "/?x=1"
    std::string why;              // 解析失败的原因（中文，可直接进日志）
};

// 只支持 http / https；带用户信息（user@host）的 URL 明确拒绝而不是悄悄解析错。
HttpUrl ParseHttpUrl(std::string_view utf8Url);

// ----- 收发 -----
struct HttpRequest {
    std::string method = "GET";     // "GET" / "POST"
    std::string url;                // UTF-8
    std::string body;               // POST 的请求体（UTF-8）
    std::vector<std::pair<std::string, std::string>> headers;   // 额外请求头
    std::size_t maxBytes = 1u << 20;   // 响应体上限，超出明确失败
    int         timeoutMs = 10000;     // 解析/连接/发送/接收四档超时
};

struct HttpResponse {
    bool                      ok = false;      // 拿到了响应；与状态码是不是 2xx 无关
    unsigned                  statusCode = 0;  // ok 时有效
    std::vector<std::uint8_t> bytes;           // 响应体
    std::wstring              error;           // ok=false 时的原因（中文）
};

// 阻塞收发一次。只允许在工作线程调用（界面线程上会冻结窗口）。
HttpResponse HttpSend(const HttpRequest& request);

HttpResponse HttpGet(const std::string& utf8Url, std::size_t maxBytes = 1u << 20,
                     int timeoutMs = 10000);

// 发 JSON：自动带 Content-Type: application/json。
HttpResponse HttpPostJson(const std::string& utf8Url, const std::string& jsonBody,
                          std::size_t maxBytes = 1u << 20, int timeoutMs = 10000);

}  // namespace npcc::infra
