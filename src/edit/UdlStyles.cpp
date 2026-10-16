#include "edit/UdlStyles.h"

#include <cstdio>

namespace npcc::edit::udl {

const char* StyleName(int style) {
    switch (style) {
        case kStyleDefault:      return "普通文本";
        case kStyleCommentLine:  return "注释（行）";
        case kStyleCommentBlock: return "注释（块）";
        case kStyleNumber:       return "数字";
        case kStyleString:       return "字符串";
        case kStyleOperator:     return "运算符";
        case kStyleIdentifier:   return "标识符";
        default:
            break;
    }
    if (style >= kStyleKeywordBase && style < kStyleKeywordBase + kKeywordGroupCount) {
        // 组号是 0 起的固定一段，这里拼成 "关键字 1"…"关键字 9"（给人看的是 1 起）
        static thread_local char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "关键字 %d", style - kStyleKeywordBase + 1);
        return buffer;
    }
    return "?";
}

}  // namespace npcc::edit::udl
