#pragma once

#include <string>
#include <string_view>

namespace npcc::core {

// 更新清单 update.json（设计文档 6.7 第 1 条）：
//   { "version": "1.0.1", "notes": "说明文字", "url": "https://…/pkg.zip",
//     "sha256": "<64 位十六进制>" }
// 复用 JsonFlat 那个"只认扁平对象"的解析器（不引第三方 JSON 库，与 plugin.json 同一个理由）。
// 能力边界照旧写清楚：
//  - 四个键一个不能少、类型必须是字符串，多出来的键也算错——清单是发布方写的，
//    写坏了要当场报出来，不能"猜到一半就开工"；
//  - JsonFlat 不认 \uXXXX 转义，说明文字里要写非 ASCII 就直接放 UTF-8 原文。
struct UpdateManifest {
    std::string version;
    std::string notes;
    std::string url;
    std::string sha256;
};

// 解析失败时填 error（带原因），不改动 out。
bool ParseUpdateManifest(std::string_view utf8, UpdateManifest& out, std::wstring* error);

// 下载源策略（设计文档 6.7：强制 HTTPS）：
//  - https:// 一律允许；
//  - 明文 http 只放行 127.0.0.1 与 localhost（回归脚本的本地源用），其余一律拒绝。
// 返回 false 时 error 里是给用户看的原因。
bool IsAllowedUpdateUrl(std::string_view url, std::wstring* error);

// 64 位十六进制（大小写都算合法；比对时调用方自己统一大小写）。
bool IsValidSha256Hex(std::string_view text);

// 版本号比较：数字点分（"1.0.1"）。逐段按整数比，段数不同时缺的段当 0，
// 所以 1.0 == 1.0.0；任何一段不是纯数字（如 "1.0a"）视为**无效**——
// *valid = false，返回值没有意义。宁可不给结论，也不把 1.0a 当成 1.0。
int CompareVersions(std::string_view a, std::string_view b, bool* valid = nullptr);

// 远端版本比当前新？valid = false 表示版本号格式非法（调用方应当报错，而不是放行或拒绝）。
bool IsNewerVersion(std::string_view candidate, std::string_view current, bool* valid = nullptr);

// 十六进制规范化的共用小工具（大写变小写；只动 ASCII 字母）。
std::string LowerHexAscii(std::string_view text);

}  // namespace npcc::core
