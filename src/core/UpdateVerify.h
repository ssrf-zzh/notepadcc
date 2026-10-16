#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace npcc::core {

// 更新清单验签（设计文档 6.7 第 1 条）：RSA-2048 + SHA-256 + PKCS#1 v1.5。
// 用系统自带的 BCrypt，不引 OpenSSL（与"SHA-256 自己写"同一个成本判断）。
//
// data 是 update.json 的**原始字节**：签名覆盖的是文件字节，不是解析出来的字段，
// 所以校验必须发生在解析之前（先验字节，再谈字段内容）。
// signatureBase64 / publicKeyBase64 都是标准 base64；公钥是 BCrypt RSAPUBLIC_BLOB，
// 见 UpdateKey.h 与 tools/make_update_key.ps1。
bool VerifyRsaSha256Signature(const void* data, std::size_t size,
                              std::string_view signatureBase64,
                              std::string_view publicKeyBase64,
                              std::wstring* error);

// Authenticode 校验（WinVerifyTrust，默认策略 + 只查本地缓存不联网取吊销列表）。
// 通过返回 true；不通过时 error 里带上文件名之外的原因（调用方拼文件名）。
bool VerifyAuthenticode(const std::wstring& path, std::wstring* error);

// 把 WinTrust / Win32 错误码转成简短中文（常见码给名字，其它给十六进制）。
// 纯函数，单测直接核对映射——对话框与日志里看到的这句话就是它给的。
std::wstring DescribeTrustError(long code);

}  // namespace npcc::core
