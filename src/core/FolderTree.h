#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace npcc::core {

// 文件夹工作区的目录树模型（设计文档 2.11 的"文件夹工作区"面板）。
//
// 模型与界面分开：这一层只吃"一个目录"，吐出"该显示哪些节点、按什么顺序"，
// 不碰任何窗口——所以它能被单元测试直接驱动（造目录、比对结果），
// 界面那层只负责把它画进树控件。
struct FolderNode {
    std::wstring             name;       // 显示名（就是文件名/目录名）
    std::wstring             path;       // 完整路径
    bool                     isFolder = false;
    std::vector<FolderNode>  children;

    bool Empty() const noexcept { return name.empty() && path.empty(); }
};

struct FolderTreeOptions {
    // 递归深度上限。1 = 只列根目录下的直接子项。设计文档里没有这条限制，
    // 但一台机器上的 node_modules 能让"扫一棵树"变成几秒的事——默认给一个上限，
    // 用户展开到那一层时可以再往下（界面按需展开，见 FolderPanel）
    int maxDepth = 8;
    // 一个目录最多列多少个子项（同上：防超大目录把界面卡住）
    int maxChildren = 2000;
};

// 读出 root 下的目录树。root 不存在或不是目录时返回空节点（不算错误——用户可能
// 删掉了那个目录，这种情况界面该显示"空"，而不是报错弹窗）。
// 排序规则：目录在前、文件在后，各自按名字**不区分大小写**排（与资源管理器一致）。
FolderNode BuildFolderTree(const std::wstring& root, const FolderTreeOptions& options = {});

// 只读一层（按需展开时用）
std::vector<FolderNode> ReadFolderChildren(const std::wstring& directory,
                                           const FolderTreeOptions& options = {});

// 排序（供上面两个函数与测试共用；暴露出来是为了让"顺序"这件事只有一个实现）
void SortFolderNodes(std::vector<FolderNode>& nodes);

}  // namespace npcc::core
