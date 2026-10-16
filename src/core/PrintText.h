#pragma once

#include <string>
#include <string_view>

namespace npcc::core {

// 打印用的页眉/页脚文本（设计文档 2.14 第 1 条）：模板里可用的占位符
//   $(FILE_NAME) $(FILE_PATH) $(PAGE) $(PAGES) $(DATE)
// 纯函数：给模板与当前取值，返回展开后的字符串。这样"页码算错、占位符没换"
// 这类错误不靠打印一张纸才能发现。
struct PrintTextContext {
    std::wstring fileName;
    std::wstring filePath;
    int          page = 1;
    int          totalPages = 1;
    std::wstring date;       // 调用方给（格式由外壳决定）
};

std::wstring ExpandPrintTemplate(std::wstring_view pattern, const PrintTextContext& context);

}  // namespace npcc::core
