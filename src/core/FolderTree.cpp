#include "core/FolderTree.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace npcc::core {
namespace {

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void ReadInto(const std::wstring& directory, int depth, const FolderTreeOptions& options,
              FolderNode& node) {
    if (depth > options.maxDepth) {
        return;
    }
    // 用 FindFirstFile 而不是 std::filesystem：后者在遇到没权限的子目录时会抛异常，
    // 而"扫一个目录"这件事天生会遇到权限问题（比如系统目录、符号链接环），
    // 我们要的是"跳过读不了的，其余照常列出"
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = directory + L"\\*";
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    std::vector<FolderNode> children;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if (static_cast<int>(children.size()) >= options.maxChildren) {
            break;
        }
        FolderNode child;
        child.name = name;
        child.path = directory + L"\\" + name;
        child.isFolder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (child.isFolder) {
            ReadInto(child.path, depth + 1, options, child);
        }
        children.push_back(std::move(child));
    } while (::FindNextFileW(handle, &found) != 0);
    ::FindClose(handle);
    SortFolderNodes(children);
    node.children = std::move(children);
}

}  // namespace

void SortFolderNodes(std::vector<FolderNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const FolderNode& a, const FolderNode& b) {
        if (a.isFolder != b.isFolder) {
            return a.isFolder;   // 目录在前
        }
        // 不区分大小写比较；完全相同（只有大小写不同，如 README 与 readme）时按原串定序，
        // 免得排序结果在不同机器上不稳定
        const int folded = ::_wcsicmp(a.name.c_str(), b.name.c_str());
        if (folded != 0) {
            return folded < 0;
        }
        return a.name < b.name;
    });
}

FolderNode BuildFolderTree(const std::wstring& root, const FolderTreeOptions& options) {
    FolderNode node;
    if (root.empty() || !IsDirectory(root)) {
        return node;   // 空节点 = 界面上什么都不显示
    }
    node.name = root;
    node.path = root;
    node.isFolder = true;
    ReadInto(root, 1, options, node);
    return node;
}

std::vector<FolderNode> ReadFolderChildren(const std::wstring& directory,
                                           const FolderTreeOptions& options) {
    FolderNode node;
    if (directory.empty() || !IsDirectory(directory)) {
        return {};
    }
    node.path = directory;
    ReadInto(directory, 1, options, node);
    return node.children;
}

}  // namespace npcc::core
