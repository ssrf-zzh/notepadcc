#include "infra/Http.h"

#include <windows.h>
#include <winhttp.h>

#include "infra/Log.h"
#include "infra/TextCodec.h"

namespace npcc::infra {
namespace {

constexpr const wchar_t* kUserAgent = L"notepadcc/1.1";

char SimpleLower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

std::wstring WinErrorText(const wchar_t* what, unsigned long code) {
    return std::wstring(what) + L"（WinHTTP 错误 " + std::to_wstring(code) + L"）";
}

// 把系统/IE 的代理配置记进日志：开着代理的机器上，"请求到底走没走代理"
// 是第一手线索。取不到就什么都不写（不构成错误）。
void LogProxyIfConfigured() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (::WinHttpGetIEProxyConfigForCurrentUser(&config) == 0) {
        return;
    }
    if (config.lpszProxy != nullptr) {
        LogInfo(L"http", std::wstring(L"使用系统代理: ") + config.lpszProxy);
    }
    if (config.lpszProxyBypass != nullptr) {
        LogInfo(L"http", std::wstring(L"代理绕过列表: ") + config.lpszProxyBypass);
    }
    if (config.lpszAutoConfigUrl != nullptr) {
        LogInfo(L"http", std::wstring(L"代理自动配置脚本: ") + config.lpszAutoConfigUrl);
    }
    if (config.lpszProxy != nullptr) {
        ::GlobalFree(config.lpszProxy);
    }
    if (config.lpszProxyBypass != nullptr) {
        ::GlobalFree(config.lpszProxyBypass);
    }
    if (config.lpszAutoConfigUrl != nullptr) {
        ::GlobalFree(config.lpszAutoConfigUrl);
    }
}

struct HttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~HttpHandles() {
        if (request != nullptr)    ::WinHttpCloseHandle(request);
        if (connection != nullptr) ::WinHttpCloseHandle(connection);
        if (session != nullptr)    ::WinHttpCloseHandle(session);
    }
};

}  // namespace

HttpUrl ParseHttpUrl(std::string_view utf8Url) {
    HttpUrl result;
    const std::string url(utf8Url);

    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        result.why = "URL 里没有协议头（要 http:// 或 https://）：" + url;
        return result;
    }
    std::string scheme = url.substr(0, schemeEnd);
    for (char& ch : scheme) {
        ch = SimpleLower(ch);
    }
    if (scheme != "http" && scheme != "https") {
        result.why = "只支持 http / https：" + url;
        return result;
    }
    result.scheme = scheme;
    result.secure = scheme == "https";

    const std::string rest = url.substr(schemeEnd + 3);
    std::string hostPort;
    std::string tail;
    const std::size_t firstDelim = rest.find_first_of("/?");
    if (firstDelim == std::string::npos) {
        hostPort = rest;
    } else {
        hostPort = rest.substr(0, firstDelim);
        tail = rest.substr(firstDelim);
    }
    if (hostPort.empty()) {
        result.why = "URL 里没有主机名：" + url;
        return result;
    }
    if (hostPort.find('@') != std::string::npos) {
        result.why = "不支持带用户信息（user@host）的 URL：" + url;
        return result;
    }

    std::string portText;
    if (hostPort[0] == '[') {   // IPv6：[::1] 或 [::1]:8080
        const std::size_t close = hostPort.find(']');
        if (close == std::string::npos) {
            result.why = "IPv6 主机少了收尾的 ]：" + url;
            return result;
        }
        result.host = hostPort.substr(1, close - 1);
        const std::string after = hostPort.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') {
                result.why = "IPv6 主机 ] 后面只允许 :端口：" + url;
                return result;
            }
            portText = after.substr(1);
        }
    } else {
        const std::size_t colon = hostPort.rfind(':');
        if (colon == std::string::npos) {
            result.host = hostPort;
        } else {
            result.host = hostPort.substr(0, colon);
            portText = hostPort.substr(colon + 1);
        }
    }

    for (char& ch : result.host) {
        ch = SimpleLower(ch);
    }
    if (result.host.empty() || result.host.find_first_of(" \t") != std::string::npos) {
        result.why = "URL 里没有合法的主机名：" + url;
        return result;
    }

    if (portText.empty()) {
        result.port = result.secure ? 443u : 80u;
    } else {
        unsigned value = 0;
        for (char ch : portText) {
            if (ch < '0' || ch > '9') {
                result.why = "端口不是数字：" + url;
                return result;
            }
            value = value * 10u + static_cast<unsigned>(ch - '0');
            if (value > 65535u) {
                result.why = "端口超出 1–65535：" + url;
                return result;
            }
        }
        if (value == 0u) {
            result.why = "端口不能是 0：" + url;
            return result;
        }
        result.port = value;
    }

    if (tail.empty()) {
        result.pathAndQuery = "/";
    } else if (tail[0] == '/') {
        result.pathAndQuery = tail;
    } else {
        result.pathAndQuery = "/" + tail;
    }
    result.loopback = result.host == "127.0.0.1" || result.host == "localhost" ||
                      result.host == "::1";
    result.valid = true;
    return result;
}

HttpResponse HttpSend(const HttpRequest& request) {
    HttpResponse result;
    const HttpUrl url = ParseHttpUrl(request.url);
    if (!url.valid) {
        result.error = WideFromUtf8(url.why);
        return result;
    }

    const std::wstring host = WideFromUtf8(url.host);
    const std::wstring path = WideFromUtf8(url.pathAndQuery);
    const std::wstring method =
        WideFromUtf8(request.method.empty() ? std::string("GET") : request.method);

    HttpHandles handles;
    // 回环地址绝不过代理：本地回归源与自检都指 127.0.0.1，机器上的代理配置不该插一脚。
    // 其余地址先试"自动代理"（系统/IE 配置，含 PAC/WPAD），失败再退回默认代理设置。
    handles.session = ::WinHttpOpen(
        kUserAgent, url.loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles.session == nullptr && !url.loopback) {
        handles.session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (handles.session == nullptr) {
        result.error = WinErrorText(L"WinHTTP 会话创建失败", ::GetLastError());
        return result;
    }
    const int timeout = request.timeoutMs > 0 ? request.timeoutMs : 10000;
    ::WinHttpSetTimeouts(handles.session, timeout, timeout, timeout, timeout);
    if (!url.loopback) {
        LogProxyIfConfigured();
    }

    handles.connection =
        ::WinHttpConnect(handles.session, host.c_str(), static_cast<INTERNET_PORT>(url.port), 0);
    if (handles.connection == nullptr) {
        result.error = WinErrorText(L"连接失败", ::GetLastError());
        return result;
    }
    handles.request = ::WinHttpOpenRequest(handles.connection, method.c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (handles.request == nullptr) {
        result.error = WinErrorText(L"请求创建失败", ::GetLastError());
        return result;
    }
    // 默认就是"禁止 https 降级到 http"；显式写出来，验证时不用去查默认值
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    ::WinHttpSetOption(handles.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                       sizeof(redirectPolicy));

    std::string headerText;
    for (const std::pair<std::string, std::string>& header : request.headers) {
        headerText += header.first + ": " + header.second + "\r\n";
    }
    if (!headerText.empty()) {
        const std::wstring wideHeaders = WideFromUtf8(headerText);
        ::WinHttpAddRequestHeaders(handles.request, wideHeaders.c_str(),
                                   static_cast<DWORD>(wideHeaders.size()),
                                   WINHTTP_ADDREQ_FLAG_ADD);
    }

    LPVOID bodyData = WINHTTP_NO_REQUEST_DATA;
    DWORD bodySize = 0;
    if (!request.body.empty()) {
        bodyData = const_cast<char*>(request.body.data());
        bodySize = static_cast<DWORD>(request.body.size());
    }
    if (::WinHttpSendRequest(handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyData, bodySize,
                             bodySize, 0) == 0 ||
        ::WinHttpReceiveResponse(handles.request, nullptr) == 0) {
        result.error = WinErrorText(L"请求发送失败", ::GetLastError());
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (::WinHttpQueryHeaders(handles.request,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                              WINHTTP_NO_HEADER_INDEX) == 0) {
        result.error = WinErrorText(L"读不到 HTTP 状态码", ::GetLastError());
        return result;
    }
    result.statusCode = static_cast<unsigned>(statusCode);

    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(handles.request, &available) == 0) {
            result.error = WinErrorText(L"读数据失败", ::GetLastError());
            return result;
        }
        if (available == 0) {
            break;   // 收完
        }
        const std::size_t offset = result.bytes.size();
        if (offset + available > request.maxBytes) {
            result.error = L"内容超过大小上限（" + std::to_wstring(request.maxBytes) +
                           L" 字节）：" + WideFromUtf8(request.url);
            return result;
        }
        result.bytes.resize(offset + available);
        DWORD read = 0;
        if (::WinHttpReadData(handles.request, result.bytes.data() + offset, available, &read) == 0) {
            result.error = WinErrorText(L"读数据失败", ::GetLastError());
            return result;
        }
        result.bytes.resize(offset + read);
    }
    result.ok = true;
    return result;
}

HttpResponse HttpGet(const std::string& utf8Url, std::size_t maxBytes, int timeoutMs) {
    HttpRequest request;
    request.method = "GET";
    request.url = utf8Url;
    request.maxBytes = maxBytes;
    request.timeoutMs = timeoutMs;
    return HttpSend(request);
}

HttpResponse HttpPostJson(const std::string& utf8Url, const std::string& jsonBody,
                          std::size_t maxBytes, int timeoutMs) {
    HttpRequest request;
    request.method = "POST";
    request.url = utf8Url;
    request.body = jsonBody;
    request.headers.emplace_back("Content-Type", "application/json");
    request.maxBytes = maxBytes;
    request.timeoutMs = timeoutMs;
    return HttpSend(request);
}

}  // namespace npcc::infra
