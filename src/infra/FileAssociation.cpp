#include "infra/FileAssociation.h"

#include "infra/Log.h"

namespace npcc::infra {
namespace {

bool SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return ::RegSetValueExW(key, name, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

bool CreateKey(HKEY root, const std::wstring& path, HKEY& out) {
    DWORD disposition = 0;
    return ::RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &out,
                             &disposition) == ERROR_SUCCESS;
}

// 登记一条：ProgId 描述与打开命令，再把 ProgId 挂到扩展名的 OpenWithProgids 下。
bool RegisterOne(HKEY root, const std::wstring& classes, const FileAssociationSpec& spec,
                 const std::wstring& applicationPath, std::wstring* error) {
    const std::wstring progId = classes + L"\\" + spec.progId;
    HKEY key = nullptr;
    if (!CreateKey(root, progId, key)) {
        if (error != nullptr) {
            *error = L"无法创建注册表项 " + progId;
        }
        return false;
    }
    SetStringValue(key, nullptr, spec.description);
    ::RegCloseKey(key);

    const std::wstring command = progId + L"\\shell\\open\\command";
    if (!CreateKey(root, command, key)) {
        if (error != nullptr) {
            *error = L"无法创建注册表项 " + command;
        }
        return false;
    }
    // "%1" 要带引号：路径里有空格时不加引号会被拆成两个参数
    SetStringValue(key, nullptr, L"\"" + applicationPath + L"\" \"%1\"");
    ::RegCloseKey(key);

    const std::wstring openWith = classes + L"\\" + spec.extension + L"\\OpenWithProgids";
    if (!CreateKey(root, openWith, key)) {
        if (error != nullptr) {
            *error = L"无法创建注册表项 " + openWith;
        }
        return false;
    }
    // OpenWithProgids 下的值只需要"存在"，内容为空
    const BOOL ok = ::RegSetValueExW(key, spec.progId, 0, REG_SZ,
                                     reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t)) ==
                    ERROR_SUCCESS;
    ::RegCloseKey(key);
    if (!ok && error != nullptr) {
        *error = L"无法登记 " + std::wstring(spec.extension);
    }
    return ok != FALSE;
}

}  // namespace

const std::vector<FileAssociationSpec>& DefaultAssociations() {
    static const std::vector<FileAssociationSpec> specs = {
        {L".txt", L"notepadcc.txt", L"文本文档（notepadcc）"},
        {L".log", L"notepadcc.log", L"日志文件（notepadcc）"},
        {L".cpp", L"notepadcc.cpp", L"C++ 源文件（notepadcc）"},
        {L".h", L"notepadcc.h", L"C/C++ 头文件（notepadcc）"},
        {L".py", L"notepadcc.py", L"Python 源文件（notepadcc）"},
        {L".js", L"notepadcc.js", L"JavaScript 源文件（notepadcc）"},
        {L".json", L"notepadcc.json", L"JSON 文件（notepadcc）"},
        {L".md", L"notepadcc.md", L"Markdown 文件（notepadcc）"},
        {L".ini", L"notepadcc.ini", L"配置文件（notepadcc）"},
        {L".csv", L"notepadcc.csv", L"CSV 文件（notepadcc）"},
    };
    return specs;
}

bool RegisterAssociations(HKEY root, const std::wstring& classesSubKey,
                          const std::wstring& applicationPath, std::wstring* error) {
    if (applicationPath.empty()) {
        if (error != nullptr) {
            *error = L"程序路径为空";
        }
        return false;
    }
    int done = 0;
    for (const FileAssociationSpec& spec : DefaultAssociations()) {
        if (RegisterOne(root, classesSubKey, spec, applicationPath, error)) {
            ++done;
        }
    }
    LogInfo(L"assoc", L"已登记 " + std::to_wstring(done) + L" / " +
                          std::to_wstring(DefaultAssociations().size()) +
                          L" 个扩展名到打开方式列表");
    if (done == 0 && error != nullptr) {
        *error = L"没有任何一个扩展名登记成功";
    }
    return done > 0;
}

bool UnregisterAssociations(HKEY root, const std::wstring& classesSubKey, std::wstring* error) {
    (void)error;   // 移除失败不阻断（本来就不存在也算正常），只记日志
    bool ok = true;
    for (const FileAssociationSpec& spec : DefaultAssociations()) {
        // 先摘掉 OpenWithProgids 下的登记值，再删掉 ProgId 自己的整棵子树
        HKEY key = nullptr;
        const std::wstring openWith = classesSubKey + L"\\" + spec.extension +
                                      L"\\OpenWithProgids";
        if (::RegOpenKeyExW(root, openWith.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            ::RegDeleteValueW(key, spec.progId);
            ::RegCloseKey(key);
        }
        // 如果这个键是我们建出来的、现在空了，就一并删掉——"移除"应当把状态还原回去，
        // 而不是留下一堆空壳
        if (::RegOpenKeyExW(root, openWith.c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            DWORD subKeys = 0;
            DWORD values = 0;
            const bool empty = ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeys, nullptr,
                                                  nullptr, &values, nullptr, nullptr, nullptr,
                                                  nullptr) == ERROR_SUCCESS &&
                               subKeys == 0 && values == 0;
            ::RegCloseKey(key);
            if (empty) {
                ::RegDeleteKeyW(root, openWith.c_str());
            }
        }
        const std::wstring progId = classesSubKey + L"\\" + spec.progId;
        if (::RegDeleteTreeW(root, progId.c_str()) != ERROR_SUCCESS) {
            // 本来就不存在也算正常，只记日志
            LogInfo(L"assoc", L"注册表项不存在或删除失败: " + progId);
        }
    }
    LogInfo(L"assoc", L"已从打开方式列表移除");
    return ok;
}

int CountRegistered(HKEY root, const std::wstring& classesSubKey) {
    int count = 0;
    for (const FileAssociationSpec& spec : DefaultAssociations()) {
        HKEY key = nullptr;
        const std::wstring openWith = classesSubKey + L"\\" + spec.extension +
                                      L"\\OpenWithProgids";
        if (::RegOpenKeyExW(root, openWith.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
            continue;
        }
        DWORD type = 0;
        DWORD size = 0;
        if (::RegQueryValueExW(key, spec.progId, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
            ++count;
        }
        ::RegCloseKey(key);
    }
    return count;
}

}  // namespace npcc::infra
