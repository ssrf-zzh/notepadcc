#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// 只认"扁平对象"的极简 JSON 读取器：给 plugin.json 这种 `{"键": 值, ...}` 用。
//
// 为什么自己写而不引第三方：要读的东西就是几个字符串与数字（设计文档 7.8 的 plugin.json），
// 引一个 JSON 库进来只为这一件事不划算（设计文档 1.2 的选型原则同样如此）。
// **能力边界写清楚**：不支持嵌套对象、不支持 null、不支持转义序列里的 \uXXXX；
// 数组只支持**字符串数组**（包括空数组 `[]`），拼成逗号分隔的字符串交给调用方去切。
// 遇到不认识的写法返回失败并给出位置，而不是猜一个值出来——插件清单写坏了要看得见。
struct JsonValue {
    enum class Kind { String, Number, Boolean };
    Kind         kind = Kind::String;
    std::string  text;
    double       number = 0;
    bool         boolean = false;
};

struct JsonObject {
    std::map<std::string, JsonValue> values;   // 键是 UTF-8

    bool         Has(std::string_view key) const;
    std::string  StringOr(std::string_view key, std::string_view fallback) const;
    double       NumberOr(std::string_view key, double fallback) const;
    bool         BoolOr(std::string_view key, bool fallback) const;
};

// 解析失败时填写 error（带出错位置），不改动 out。
bool ParseJsonObject(std::string_view utf8, JsonObject& out, std::wstring* error);

}  // namespace npcc::core
