#pragma once

#include <string>

namespace npcc::core {

// 更新清单的 RSA 验签（设计文档 6.7 第 1 条）已下沉到 infra::Crypto（B1：
// 算法与许可验签共用一处实现，也修掉 infra 层更新服务反向引用 core 的问题）。
// 本文件只留 Authenticode 校验与错误码文案——它们绑在 Windows 信任链上，
// 不属于"字节级加密设施"。

// Authenticode 校验（WinVerifyTrust，默认策略 + 只查本地缓存不联网取吊销列表）。
// 通过返回 true；不通过时 error 里带上文件名之外的原因（调用方拼文件名）。
bool VerifyAuthenticode(const std::wstring& path, std::wstring* error);

// 把 WinTrust / Win32 错误码转成简短中文（常见码给名字，其它给十六进制）。
// 纯函数，单测直接核对映射——对话框与日志里看到的这句话就是它给的。
std::wstring DescribeTrustError(long code);

}  // namespace npcc::core
