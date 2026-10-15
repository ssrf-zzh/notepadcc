#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace npcc::infra {

// 文件关联（设计文档 2.11 的"文件关联"页 / M14）。
//
// 只做**加入"打开方式"列表**，不抢默认关联。原因有二：
//   1. Windows 8 起默认关联落在 UserChoice 上并带哈希保护，程序无法可靠设置，
//      硬写会被系统判为无效并重置——不如不写；
//   2. 静默改掉用户双击 .txt 的默认行为，是很不友好的做法。
// 想设为默认，交给用户在资源管理器里选"始终使用此应用打开"。

struct FileAssociationSpec {
    const wchar_t* extension;    // ".txt"
    const wchar_t* progId;       // "notepadcc.txt"
    const wchar_t* description;  // 显示在"打开方式"里的名字
};

const std::vector<FileAssociationSpec>& DefaultAssociations();

// root 与 classesSubKey 可指定，便于测试写到一个临时键下、不碰真实关联。
bool RegisterAssociations(HKEY root, const std::wstring& classesSubKey,
                          const std::wstring& applicationPath, std::wstring* error);
bool UnregisterAssociations(HKEY root, const std::wstring& classesSubKey, std::wstring* error);
// 已经登记了几个扩展名（用于告诉用户当前状态）
int CountRegistered(HKEY root, const std::wstring& classesSubKey);

}  // namespace npcc::infra
