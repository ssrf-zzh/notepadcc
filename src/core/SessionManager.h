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
};

struct SessionSnapshot {
    std::uint32_t                schemaVersion = 1;
    std::vector<SessionDocument> documents;   // 顺序即标签顺序
};

// 会话文件（<配置根>/session.xml）的读写。
//
// 只记录**有路径**的文档：未命名文档在退出时已经被逐一询问过"保存还是丢弃"，
// 重启再把它恢复出来会与用户当时的选择相矛盾。
//
// M1 记录：路径、光标位置、首个可见行、缩放级别、哪个是活动标签。
// 设计文档 7.4 中列出的书签、折叠状态、标签颜色、只读标记、分屏状态留待 M2。
class SessionManager {
public:
    static std::wstring SessionPath();

    static bool Save(const SessionSnapshot& snapshot, std::wstring* error);
    static bool Load(SessionSnapshot& snapshot, std::wstring* error);
};

}  // namespace npcc::core
