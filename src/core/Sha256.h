#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace npcc::core {

// SHA-256（FIPS 180-4）。自己实现而不引第三方库：更新校验只需要这一条算法，
// 为它拖一个加密库进来不划算（与 JsonFlat 自己写是同一个理由，见设计文档 1.2 的选型原则）。
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

}  // namespace npcc::core
