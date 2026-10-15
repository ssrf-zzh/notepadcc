#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "edit/Encoding.h"

namespace npcc::core {

// 一个未保存内容的快照（设计文档 8.3）。
//
// 文件格式刻意做成"文本头 + 原始字节"：头部逐行 `键=值`，空行之后就是编辑器的
// 原始字节。这样人能直接看懂快照是什么、又不必给二进制内容做 base64 之类的转义
// （大文件转义一次要多占三分之一空间，还要多一趟编解码）。
struct SnapshotInfo {
    std::filesystem::path file;        // 快照文件自身
    std::wstring          originalPath;   // 原文件路径；未命名文档为空
    std::wstring          displayName;    // 展示名（文件名或"新建 1"）
    edit::EncodingState   encoding;
    edit::EolMode         eol      = edit::EolMode::Crlf;
    bool                  untitled = true;
    std::wstring          savedAt;        // 本地时间文本，仅用于展示与排序
    std::uint64_t         length   = 0;   // 内容字节数
};

class SnapshotStore {
public:
    static std::filesystem::path BackupRoot();     // <config>\backup
    static std::filesystem::path AutosaveDir();    // <config>\backup\autosave
    // <config>\backup\crash_YYYYMMDD-HHMMSS
    static std::filesystem::path CrashDirFor(std::wstring_view stamp);
    // 当前的本地时间戳，形如 20261015-120000（字典序即时间序）
    static std::wstring Timestamp();
    static std::wstring DisplayTime();

    // 头部 + 原始字节。纯字符串函数，单独测。
    static std::string Build(const SnapshotInfo& info, std::string_view editorBytes);
    // 解析 Build 的输出。截断、缺字段、长度对不上都算失败。
    static bool Parse(std::string_view raw, SnapshotInfo& info, std::string& editorBytes);
    // 只解析头部（给 List 用，不必把内容读进来）
    static bool ParseHeader(std::string_view raw, SnapshotInfo& info, std::size_t& payloadOffset);

    // 原子写盘。返回失败原因（写不进去时不能让调用方蒙在鼓里）。
    bool Write(const SnapshotInfo& info, std::string_view editorBytes, std::wstring* error) const;
    // 扫描 autosave\ 与 crash_*\ 下的全部快照，按时间倒序（新的在前）
    std::vector<SnapshotInfo> List() const;
    bool Read(const SnapshotInfo& info, std::string& editorBytes, std::wstring* error) const;
    bool Remove(const SnapshotInfo& info) const;
    void RemoveAllAutosave() const;
    // 崩溃目录只保留最近 keep 个，其余连同里面的 minidump 一起删掉
    void PruneCrashDirs(std::size_t keep) const;
};

}  // namespace npcc::core
