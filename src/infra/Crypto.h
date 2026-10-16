#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace npcc::infra {

// 字节级加密设施（B1）：SHA-256、Base64、RSA-2048 PKCS#1 v1.5 验签、随机字节。
// 设计依据《notepadcc许可与激活设计.md》8.1：更新验签与许可验签共用这一处实现，
// 算法全走系统组件（BCrypt）与自研 SHA-256，不引入第三方库（设计文档 1.2 的选型原则）。
//
// 原先的实现散在 core/Sha256、edit/TextTools 与 core/UpdateVerify 里：infra 层的
// 更新服务要反向引用 core 才拿得到 SHA-256，方向是反的，这次一并收拢到这一层。

// ----- SHA-256（FIPS 180-4）-----
// 自己实现而不引第三方库：只有"内容摘要"这一条需求，为它拖一个加密库进来不划算
// （与 JsonFlat 自己写是同一个理由，见设计文档 1.2 的选型原则）。
// 按"流式"组织：下载大文件时边收边算，不必把整个包放进内存。
struct Sha256Digest {
    std::uint8_t bytes[32] = {};

    bool operator==(const Sha256Digest& other) const;
    bool operator!=(const Sha256Digest& other) const { return !(*this == other); }

    std::string Hex() const;   // 小写十六进制（64 字符）
};

class Sha256Stream {
public:
    Sha256Stream();

    void Update(const void* data, std::size_t size);

    // 取当前摘要（不改动内部状态，取完可以继续 Update）
    Sha256Digest Digest() const;

private:
    void Transform(const std::uint8_t block[64]);

    std::uint32_t state_[8];
    std::uint64_t totalBytes_ = 0;
    std::uint8_t  buffer_[64] = {};
    std::size_t   buffered_ = 0;
};

Sha256Digest Sha256(const void* data, std::size_t size);

// ----- Base64（RFC 4648 标准字母表，带填充）-----
// 解码容忍空白与换行（贴进来的内容常有排版换行），但长度不是 4 的倍数、
// 含非法字符、填充位置不对一律明确失败、不改动 out——与文本工具的界面行为一致。
std::string Base64Encode(std::string_view bytes);
bool Base64Decode(std::string_view text, std::string& out);

// ----- RSA-2048 + SHA-256 + PKCS#1 v1.5 验签（BCrypt）-----
// data 是被签名的**原始字节**：签名覆盖的是字节本身，不是解析出来的字段，
// 所以校验必须发生在解析之前（先验字节，再谈字段内容）。更新清单与许可文件
// 都遵循这条顺序。
// signatureBase64 / publicKeyBase64 都是标准 base64；公钥是 BCrypt RSAPUBLIC_BLOB，
// 见 core/UpdateKey.h 与 tools/make_update_key.ps1。
bool RsaVerifyPkcs1Sha256(const void* data, std::size_t size,
                          std::string_view signatureBase64,
                          std::string_view publicKeyBase64,
                          std::wstring* error);

// ----- 随机字节（BCryptGenRandom，系统 CSPRNG）-----
// 生成失败返回 false——不要"退回 rand()"静默降级，让调用方明确失败。
bool RandomBytes(void* out, std::size_t size);

}  // namespace npcc::infra