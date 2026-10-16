#pragma once

#include <string>
#include <string_view>

namespace npcc::core {

// 格式化结果：失败时给出**能读懂的位置**，绝不返回半截文本
struct FormatResult {
    bool         ok = false;
    std::string  text;      // 成功时的结果
    std::wstring error;     // 失败原因（含第几行第几列）

    static FormatResult Fail(std::wstring message) {
        FormatResult result;
        result.error = std::move(message);
        return result;
    }
    static FormatResult Ok(std::string out) {
        FormatResult result;
        result.ok = true;
        result.text = std::move(out);
        return result;
    }
};

// JSON 美化 / 压缩（设计文档 2.13 第 2 条）。
//
// 做法是**词法+结构重排，不是解析成对象再序列化**：字符串与数字**原样保留**——包括
// 转义写法（`\u4e2d` 不会被换成汉字）与数字字面量（`1.500` 不会被写成 `1.5`）。
// 格式化只该动空白与缩进，动到值就是改用户的数据。（这条是刻意的：一个"美化"按钮
// 把 `1e3` 变成 `1000`、把转义换成真字符，都是不可接受的行为。）
//
// indent 是每层的空格数（美化模式）；minify=true 时压成一行（字符串里的空白不动）。
FormatResult FormatJson(std::string_view text, int indent = 2, bool minify = false);

}  // namespace npcc::core
