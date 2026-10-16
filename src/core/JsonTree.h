#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// JSON 结构树（M3 的"内置 JSON 能力"之一：树视图的数据来源）。
//
// 分工是刻意的：**校验交给 JsonFormat**（它那套错误信息已经写得够细，含行列），
// 这里只在"已经确认合法"的输入上走一遍、把结构和行号记下来。这样"什么叫合法 JSON"
// 只有一处定义，不会出现"格式化说不行、树视图说行"这种事。

enum class JsonKind { Object, Array, String, Number, Bool, Null };

struct JsonNode {
    JsonKind                 kind = JsonKind::Null;
    std::string              name;      // 对象的键（数组元素与根为空）
    // 标量的原文：字符串是**引号里面那一段**（含 \u4e2d 这样的转义原文，不做解释——
    // 与格式化器一个态度：显示要忠实于文件里到底是什么），数字/true/false/null 是字面量
    std::string              value;
    std::vector<JsonNode>    children;  // 对象/数组的子节点
    int                      line = 1;  // 在源文本里的行号（1 基），点击跳转要用
    int                      count = 0; // 子节点数（对象/数组）
};

struct JsonTreeResult {
    bool         ok = false;
    JsonNode     root;
    std::wstring error;                 // 失败原因（来自 JsonFormat，含行列）
    int          nodeCount = 0;
    bool         truncated = false;     // 节点太多，只建了前面一部分
};

// 节点数上限：一棵几十万节点的树既没人看得完，也会把面板撑爆
inline constexpr int kMaxJsonNodes = 20000;

JsonTreeResult ParseJsonTree(std::string_view text);

}  // namespace npcc::core
