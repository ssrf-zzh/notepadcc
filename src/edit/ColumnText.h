#pragma once

#include <string>
#include <vector>

namespace npcc::edit {

// 列块编辑的插入参数（设计文档 2.6 第 3 条）。
// 模板里用 \i\ 表示序号占位符，例如 "item_\i\" 或 "0x\i\"。
struct ColumnInsertOptions {
    std::wstring templateText;
    long long    start = 1;
    long long    step = 1;
    int          zeroPad = 0;      // 补零到多少位，0 = 不补
    int          base = 10;        // 10 或 16
    int          resetEvery = 0;   // 每 N 行重置序号，0 = 不重置
    int          lineCount = 0;    // 要覆盖多少行
};

// 序号占位符。写成宏是为了让对话框的提示文字与实现不会走散。
inline constexpr const wchar_t* kColumnIndexPlaceholder = L"\\i\\";

// 把数值格式化成序列文本（含补零与进制）
std::wstring FormatSequenceValue(long long value, int zeroPad, int base);

// 生成每一行要插入的文本。
// 模板里没有占位符时按字面量插入每一行（此时序号参数不生效）。
std::vector<std::wstring> BuildColumnInsertTexts(const ColumnInsertOptions& options);

}  // namespace npcc::edit
