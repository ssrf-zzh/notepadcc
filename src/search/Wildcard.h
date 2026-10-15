#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace npcc::search {

// 通配符匹配，支持 * 与 ?。文件名场景默认不区分大小写（Windows 语义）。
bool WildcardMatch(std::wstring_view pattern, std::wstring_view text, bool caseInsensitive = true);

// 过滤器："*.js;*.html;!node_modules"，分号分隔，!前缀表示排除。
struct FileFilter {
    std::vector<std::wstring> includes;   // 为空表示不限制
    std::vector<std::wstring> excludes;
};

FileFilter ParseFileFilter(std::wstring_view spec);

// 判断一个名字（文件名或目录名）是否被允许。
// 目录只需要过 excludes；文件需要同时过 includes 与 excludes。
bool FilterAllowsFile(const FileFilter& filter, std::wstring_view fileName);
bool FilterAllowsDirectory(const FileFilter& filter, std::wstring_view directoryName);

}  // namespace npcc::search
