#include "search/Wildcard.h"

#include <algorithm>
#include <cwctype>

namespace npcc::search {
namespace {

wchar_t Fold(wchar_t ch, bool caseInsensitive) {
    return caseInsensitive ? static_cast<wchar_t>(::towlower(ch)) : ch;
}

}  // namespace

bool WildcardMatch(std::wstring_view pattern, std::wstring_view text, bool caseInsensitive) {
    // 迭代式实现，避免递归在超长模式上爆栈
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t starP = std::wstring_view::npos;
    std::size_t starT = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' ||
                                   Fold(pattern[p], caseInsensitive) ==
                                       Fold(text[t], caseInsensitive))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            starP = p++;
            starT = t;
        } else if (starP != std::wstring_view::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') {
        ++p;
    }
    return p == pattern.size();
}

FileFilter ParseFileFilter(std::wstring_view spec) {
    FileFilter filter;
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t end = spec.find(L';', start);
        std::wstring token(spec.substr(start, end == std::wstring_view::npos
                                                  ? std::wstring_view::npos
                                                  : end - start));
        // 去掉首尾空白
        const auto notSpace = [](wchar_t ch) { return ::iswspace(ch) == 0; };
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), notSpace));
        token.erase(std::find_if(token.rbegin(), token.rend(), notSpace).base(), token.end());

        if (!token.empty()) {
            if (token[0] == L'!') {
                if (token.size() > 1) {
                    filter.excludes.push_back(token.substr(1));
                }
            } else {
                filter.includes.push_back(std::move(token));
            }
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    return filter;
}

bool FilterAllowsFile(const FileFilter& filter, std::wstring_view fileName) {
    for (const std::wstring& exclude : filter.excludes) {
        if (WildcardMatch(exclude, fileName)) {
            return false;
        }
    }
    if (filter.includes.empty()) {
        return true;
    }
    for (const std::wstring& include : filter.includes) {
        if (WildcardMatch(include, fileName)) {
            return true;
        }
    }
    return false;
}

bool FilterAllowsDirectory(const FileFilter& filter, std::wstring_view directoryName) {
    for (const std::wstring& exclude : filter.excludes) {
        if (WildcardMatch(exclude, directoryName)) {
            return false;
        }
    }
    return true;
}

}  // namespace npcc::search
