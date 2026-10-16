#include "infra/Crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace npcc::infra {
namespace {

constexpr std::uint32_t kInitial[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr std::uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

std::uint32_t Rotr(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

}  // namespace

bool Sha256Digest::operator==(const Sha256Digest& other) const {
    return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

std::string Sha256Digest::Hex() const {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

Sha256Stream::Sha256Stream() {
    std::memcpy(state_, kInitial, sizeof(state_));
}

void Sha256Stream::Transform(const std::uint8_t block[64]) {
    std::uint32_t words[64];
    for (int i = 0; i < 16; ++i) {
        words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            Rotr(words[i - 15], 7) ^ Rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const std::uint32_t s1 =
            Rotr(words[i - 2], 17) ^ Rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[i] + words[i];
        const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256Stream::Update(const void* data, std::size_t size) {
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    totalBytes_ += size;
    if (buffered_ != 0) {
        const std::size_t take = std::min<std::size_t>(64 - buffered_, size);
        std::memcpy(buffer_ + buffered_, cursor, take);
        cursor += take;
        size -= take;
        buffered_ += take;
        if (buffered_ == 64) {
            Transform(buffer_);
            buffered_ = 0;
        }
    }
    while (size >= 64) {
        Transform(cursor);
        cursor += 64;
        size -= 64;
    }
    if (size != 0) {
        std::memcpy(buffer_, cursor, size);
        buffered_ = size;
    }
}

Sha256Digest Sha256Stream::Digest() const {
    Sha256Stream copy = *this;
    const std::uint64_t bitCount = copy.totalBytes_ * 8;
    const std::uint8_t one = 0x80;
    copy.Update(&one, 1);
    const std::uint8_t zero = 0;
    while (copy.buffered_ != 56) {
        copy.Update(&zero, 1);
    }
    std::uint8_t length[8];
    for (int i = 0; i < 8; ++i) {
        length[i] = static_cast<std::uint8_t>(bitCount >> (56 - i * 8));
    }
    copy.Update(length, 8);

    Sha256Digest out;
    for (int i = 0; i < 8; ++i) {
        out.bytes[i * 4] = static_cast<std::uint8_t>(copy.state_[i] >> 24);
        out.bytes[i * 4 + 1] = static_cast<std::uint8_t>(copy.state_[i] >> 16);
        out.bytes[i * 4 + 2] = static_cast<std::uint8_t>(copy.state_[i] >> 8);
        out.bytes[i * 4 + 3] = static_cast<std::uint8_t>(copy.state_[i]);
    }
    return out;
}

Sha256Digest Sha256(const void* data, std::size_t size) {
    Sha256Stream stream;
    stream.Update(data, size);
    return stream.Digest();
}


// ---------------------------------------------------------------- Base64

std::string Base64Encode(std::string_view bytes) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const unsigned triple = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(alphabet[(triple >> 6) & 0x3F]);
        out.push_back(alphabet[triple & 0x3F]);
        i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const unsigned value = static_cast<unsigned char>(bytes[i]) << 16;
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out += "==";
    } else if (rest == 2) {
        const unsigned value = (static_cast<unsigned char>(bytes[i]) << 16) |
                               (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out.push_back(alphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool Base64Decode(std::string_view text, std::string& out) {
    const auto isSpace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r' || ch == '\n';
    };
    auto valueOf = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    std::string filtered;
    filtered.reserve(text.size());
    for (const char ch : text) {
        if (isSpace(ch)) {
            continue;   // 换行与空格只是排版，不算内容
        }
        filtered.push_back(ch);
    }
    if (filtered.size() % 4 != 0) {
        return false;   // 长度不是 4 的倍数：多半是把残缺的内容粘进来了
    }
    std::string decoded;
    decoded.reserve(filtered.size() / 4 * 3);
    for (std::size_t i = 0; i < filtered.size(); i += 4) {
        int values[4] = {0, 0, 0, 0};
        int padding = 0;
        for (int k = 0; k < 4; ++k) {
            const char ch = filtered[i + static_cast<std::size_t>(k)];
            if (ch == '=') {
                if (k < 2 || i + 4 != filtered.size()) {
                    return false;   // 填充只能出现在末尾，且最多两个
                }
                ++padding;
                continue;
            }
            if (padding > 0) {
                return false;       // 填充之后又出现内容
            }
            values[k] = valueOf(ch);
            if (values[k] < 0) {
                return false;
            }
        }
        const unsigned triple = (static_cast<unsigned>(values[0]) << 18) |
                                (static_cast<unsigned>(values[1]) << 12) |
                                (static_cast<unsigned>(values[2]) << 6) |
                                static_cast<unsigned>(values[3]);
        decoded.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (padding < 2) {
            decoded.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            decoded.push_back(static_cast<char>(triple & 0xFF));
        }
    }
    out = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------- RSA 验签

namespace {

constexpr NTSTATUS kStatusInvalidSignature = static_cast<NTSTATUS>(0xC000A000L);

bool DecodeBase64Vector(std::string_view text, std::vector<std::uint8_t>& out,
                        const wchar_t* what, std::wstring* error) {
    std::string decoded;
    if (!Base64Decode(text, decoded) || decoded.empty()) {
        if (error != nullptr) {
            *error = std::wstring(what) + L"不是合法的 base64";
        }
        return false;
    }
    out.assign(decoded.begin(), decoded.end());
    return true;
}

std::wstring HexCode(long code) {
    wchar_t buffer[16] = {};
    ::swprintf(buffer, 16, L"0x%08lX", static_cast<unsigned long>(code));
    return buffer;
}

}  // namespace

bool RsaVerifyPkcs1Sha256(const void* data, const std::size_t size,
                          const std::string_view signatureBase64,
                          const std::string_view publicKeyBase64,
                          std::wstring* error) {
    std::vector<std::uint8_t> signature;
    std::vector<std::uint8_t> publicKey;
    if (!DecodeBase64Vector(signatureBase64, signature, L"签名", error) ||
        !DecodeBase64Vector(publicKeyBase64, publicKey, L"公钥", error)) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool verified = false;
    do {
        if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) {
            if (error != nullptr) {
                *error = L"BCrypt 打不开 RSA 算法提供者";
            }
            break;
        }
        if (::BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                                  publicKey.data(), static_cast<ULONG>(publicKey.size()), 0) != 0) {
            if (error != nullptr) {
                *error = L"公钥不是 BCrypt RSAPUBLIC_BLOB 格式";
            }
            break;
        }
        const Sha256Digest digest = Sha256(data, size);
        BCRYPT_PKCS1_PADDING_INFO padding{};
        padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        const NTSTATUS status =
            ::BCryptVerifySignature(key, &padding, const_cast<std::uint8_t*>(digest.bytes),
                                    sizeof(digest.bytes),
                                    signature.data(), static_cast<ULONG>(signature.size()),
                                    BCRYPT_PAD_PKCS1);
        if (status != 0) {
            if (error != nullptr) {
                *error = status == kStatusInvalidSignature
                             ? L"签名与清单内容不匹配"
                             : L"验签失败（BCrypt 状态 " + HexCode(status) + L"）";
            }
            break;
        }
        verified = true;
    } while (false);

    if (key != nullptr) {
        ::BCryptDestroyKey(key);
    }
    if (algorithm != nullptr) {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return verified;
}

// ---------------------------------------------------------------- 随机字节

bool RandomBytes(void* out, std::size_t size) {
    if (size == 0) {
        return true;   // 没东西要填，不必惊动系统
    }
    return ::BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

}  // namespace npcc::infra