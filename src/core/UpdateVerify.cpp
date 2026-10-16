#include "core/UpdateVerify.h"

#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <cstdint>
#include <vector>

#include "core/Sha256.h"
#include "edit/TextTools.h"

namespace npcc::core {
namespace {

constexpr NTSTATUS kStatusInvalidSignature = static_cast<NTSTATUS>(0xC000A000L);

bool DecodeBase64(const std::string_view text, std::vector<std::uint8_t>& out,
                  const wchar_t* what, std::wstring* error) {
    std::string decoded;
    if (!edit::Base64Decode(text, decoded) || decoded.empty()) {
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

bool VerifyRsaSha256Signature(const void* data, const std::size_t size,
                              const std::string_view signatureBase64,
                              const std::string_view publicKeyBase64,
                              std::wstring* error) {
    std::vector<std::uint8_t> signature;
    std::vector<std::uint8_t> publicKey;
    if (!DecodeBase64(signatureBase64, signature, L"签名", error) ||
        !DecodeBase64(publicKeyBase64, publicKey, L"公钥", error)) {
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

bool VerifyAuthenticode(const std::wstring& path, std::wstring* error) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;                       // 不给用户弹"要不要信任"
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    // 只查本地证书缓存：更新检查不该在取吊销列表上卡住（离线机器上也一样工作）
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = ::WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(nullptr, &action, &data);           // 释放上面那次校验的状态

    if (status == ERROR_SUCCESS) {
        return true;
    }
    if (error != nullptr) {
        *error = DescribeTrustError(status);
    }
    return false;
}

std::wstring DescribeTrustError(long code) {
    switch (static_cast<unsigned long>(code)) {
        case 0x800B0100UL:
            return L"文件没有数字签名（或签名结构不完整）";
        case 0x800B0101UL:
            return L"签名证书已过期";
        case 0x800B0109UL:
            return L"证书链不受信任（根证书不在受信任列表里）";
        case 0x800B010AUL:
            return L"证书链构造失败";
        case 0x800B0004UL:
            return L"签名主体不受信任";
        case 0x80096010UL:
            return L"签名摘要不匹配（文件被改过）";
        case 0x800B0111UL:
            return L"证书被显式标记为不信任";
        default:
            return L"WinVerifyTrust 错误码 " + HexCode(code);
    }
}

}  // namespace npcc::core
