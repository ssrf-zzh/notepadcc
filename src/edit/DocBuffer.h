#pragma once

#include <cstdint>
#include <string>

#include "core/Ids.h"
#include "edit/Encoding.h"
#include "edit/FileTier.h"

namespace npcc::edit {

// 一份文档的全部状态。视图只是呈现它，不拥有它。
struct DocBuffer {
    core::BufferId id = core::kInvalidId;
    std::wstring   filePath;       // 空 = 未保存的新建文档
    std::wstring   displayName;

    bool isModified  = false;
    bool isReadOnly  = false;      // 用户锁定/只读打开
    bool isUntitled  = true;
    FileTier tier = FileTier::Normal;   // 打开时的分档，决定降级到什么程度
    // 打开/重新加载时产生的提示（例如"已进入大文件模式"），由外壳决定怎么显示
    std::wstring loadWarning;

    // 最近一次与磁盘一致时的文件身份，用于发现外部改动
    struct FileStamp {
        std::uint64_t size = 0;
        std::uint64_t lastWriteMs = 0;
        bool Valid() const noexcept { return size != 0 || lastWriteMs != 0; }
    } lastKnown;

    EncodingState encoding;
    EolMode       eol         = EolMode::Crlf;   // 保存时使用的行尾
    EolMode       detectedEol = EolMode::Crlf;   // 打开时探测到的行尾

    // 视图态（随会话保存，见设计文档 3.2）
    int32_t  caretPos          = 0;
    int32_t  firstVisibleLine  = 0;
    int32_t  zoomLevel         = 0;
    uint32_t wrapMode          = 0;              // 0 = 关闭

    bool HasPath() const noexcept { return !filePath.empty(); }
};

}  // namespace npcc::edit
