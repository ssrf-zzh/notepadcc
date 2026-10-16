#pragma once

namespace npcc::core {

// 程序版本（单一出处）：关于框、更新检查的"当前版本"都用它。
// 发布新版本时改这里一处；CMake project() 的 VERSION 是给打包脚本看的。
inline constexpr const char* kAppVersionAscii = "1.0.0";
inline constexpr const wchar_t* kAppVersion = L"1.0.0";

}  // namespace npcc::core
