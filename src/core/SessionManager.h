#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace npcc::core {

struct SessionDocument {
    std::wstring filePath;
    std::int32_t caretPos = 0;
    std::int32_t firstVisibleLine = 0;
    std::int32_t zoomLevel = 0;
    bool         isActive = false;
    std::vector<std::int32_t> bookmarks;   // 行号为 0 基，与 Scintilla 一致
    // 折叠起来的折叠头行号（同样是 0 基）。只记行号，不记"折了几层"：
    // 层级是 lexer 现算的，行号才是跟着内容走的东西。
    std::vector<std::int32_t> folds;
};

struct SessionSnapshot {
    std::uint32_t                schemaVersion = 1;
    std::vector<SessionDocument> documents;   // 顺序即标签顺序
    // 分屏状态（设计文档 7.4 的 <ViewGroup>，6.3 的落地形态）。只在分屏时才写出去：
    // 元素在不在，就代表分屏开着没开着，不必再加一个 split 属性。
    bool                         split = false;
    int                          splitRatioPercent = 50;
    bool                         splitSyncScroll = true;
    int                          splitActiveSlot = 0;
};

// 会话文件（<配置根>/session.xml）的读写。
//
// 只记录**有路径**的文档：未命名文档在退出时已经被逐一询问过"保存还是丢弃"，
// 重启再把它恢复出来会与用户当时的选择相矛盾。
//
// M1 记录：路径、光标位置、首个可见行、缩放级别、哪个是活动标签、书签行号、折叠的头部行号。
// 设计文档 7.4 中列出的标签颜色、只读标记、分屏状态留待 M2。
class SessionManager {
public:
    static std::wstring SessionPath();

    static bool Save(const SessionSnapshot& snapshot, std::wstring* error);
    static bool Load(SessionSnapshot& snapshot, std::wstring* error);
};

}  // namespace npcc::core
