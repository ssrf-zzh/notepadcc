#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// 文本差异比较（设计文档第 10 章 M3 的"内置 Compare"）。
//
// 算法是**行级 LCS**：先把两边相同的前缀与后缀切掉（真实文件里往往一大半是一样的，
// 切掉之后要算的部分能小好几个数量级），中间那段再做动态规划。DP 的格子数有上限
// （见 kMaxCells）：超了就退化成"整块替换"并在结果里说明——宁可给一个粗略但**有边界**
// 的结果，也不要为了精确把界面卡住几十秒。
//
// 相邻的"删一片 + 插一片"会被配成一对对的"改动行"（左旧右新并排），这样读起来是
// "这两行不一样"，而不是"删了三行又加了三行"。

enum class DiffKind {
    Equal,     // 两边一样
    Changed,   // 一对一配对之后两边的文字不同
    Deleted,   // 只有左边有（右边留空）
    Inserted,  // 只有右边有（左边留空）
};

struct DiffRow {
    DiffKind    kind = DiffKind::Equal;
    int         leftNumber = 0;    // 1 基行号；0 表示这一侧没有对应内容
    int         rightNumber = 0;
    std::string leftText;
    std::string rightText;
};

struct DiffStats {
    int  equal = 0;
    int  changed = 0;
    int  deleted = 0;
    int  inserted = 0;
    int  leftLines = 0;
    int  rightLines = 0;
    // 比"不同"的处数：改动算一处、连续的删除/新增各算一处（与常见差异工具的口径一致）
    int  hunks = 0;
    bool coarse = false;   // 内容太大，中间那段退化成"整块替换"
};

// DP 的规模上限（格子数）。2000×2000 = 400 万格，几毫秒到几十毫秒的量级；
// 再大就值得用更好的算法（Myers 之类），而不是继续加格子
inline constexpr std::size_t kMaxDiffCells = 4 * 1000 * 1000;

std::vector<std::string> SplitLines(std::string_view text);

std::vector<DiffRow> ComputeLineDiff(const std::vector<std::string>& left,
                                     const std::vector<std::string>& right,
                                     DiffStats* stats = nullptr);

// 渲染成两栏对齐的文本（给"比较结果"那份文档用）。
//
// 对齐按**显示宽度**算：ASCII 一格、中日韩与全角标点两格（等宽字体下就是它们实际占的
// 格数）。超出列宽的尾巴会被截断并带一个记号——不截断的话两栏会互相顶掉，谁也读不了。
struct CompareRenderOptions {
    // 每栏的显示宽度（不含行号与分隔符）。**下限是 20**：再窄两栏就没法读了，
    // 渲染时会夹到这个下限（免得调用方传个位数进来，结果是一堆省略号）
    int          columnWidth = 78;
    std::string  leftName;
    std::string  rightName;
};

std::string RenderComparison(const std::vector<DiffRow>& rows, const DiffStats& stats,
                             const CompareRenderOptions& options);

// 一段 UTF-8 的显示宽度（中文按两格算）。暴露出来是为了让单测能单独钉住它
int DisplayWidth(std::string_view utf8);

}  // namespace npcc::core
