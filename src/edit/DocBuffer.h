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
