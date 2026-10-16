#include "core/UpdateVerify.h"

#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

namespace npcc::core {
namespace {

std::wstring HexCode(long code) {
    wchar_t buffer[16] = {};
    ::swprintf(buffer, 16, L"0x%08lX", static_cast<unsigned long>(code));
    return buffer;
}

}  // namespace

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
