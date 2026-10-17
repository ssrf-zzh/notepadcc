#pragma once

#include <string>
#include <string_view>

namespace npcc::infra {

// UTF-8 ↔ UTF-16 转换（配置里存宽字符，URL、报文与签名数据按字节走）。
// MultiByteToWideChar / WideCharToMultiByte 的薄封装：只做转换、不做合法性
// 校验（输入来自配置、URL 与验签通过的内容）；失败返回空串，不抛异常。
std::string  Utf8FromWide(std::wstring_view text);
std::wstring WideFromUtf8(std::string_view text);

}  // namespace npcc::infra