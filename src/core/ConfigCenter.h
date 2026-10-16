#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "edit/Encoding.h"

namespace npcc::core {

struct GeneralConfig {
    std::wstring uiLanguage = L"zh-CN";
    bool         restoreSessionOnStart = true;
    bool         multiInstance = false;
    std::uint32_t recentFileCount = 20;
    // 主题名：只认 "light" / "dark"（配色本身在 stylers.xml 里，这里只记"用哪一套"）。
    // 放配置里而不是 stylers.xml 里：用户可以把配色文件整份换成别人的，
    // 而"我要深色"是这台机器上的使用偏好，不该被换文件带走。
    std::wstring theme = L"light";
    // 文件夹工作区的根目录（空 = 不用工作区）。放配置里而不是会话里：
    // "我常在这个目录下干活"是使用偏好，不该因为某次没保存会话就丢掉。
    std::wstring workspaceFolder;
};

struct EditConfig {
    bool          tabToSpaces = false;
    std::uint32_t tabWidth = 4;
    bool          autoIndent = true;
    bool          smartIndent = true;
    bool          showWhitespace = false;
    bool          showEol = false;
    bool          showLineNumbers = true;
    bool          showIndentGuide = true;
    bool          caretLineHighlight = true;
    bool          wrapLongLines = false;
    std::uint32_t zoomLevel = 0;
};

struct NewFileConfig {
    edit::UniMode defaultUniMode = edit::UniMode::Utf8;
    std::uint32_t defaultCodePage = 936;
    edit::EolMode defaultEol = edit::EolMode::Crlf;
};

struct BackupConfig {
    bool          enabled = true;
    std::uint32_t intervalSeconds = 30;
    std::uint32_t keepSnapshots = 10;
    // 编码转换前给文件留一份 <名字>.bak（设计文档 6.2：转换是不可逆操作）
    bool          keepConvertBackup = true;
};

struct LargeFileConfig {
    std::uint64_t tier1MaxBytes = 8ull * 1024 * 1024;
    std::uint64_t tier2MaxBytes = 64ull * 1024 * 1024;
    std::uint64_t refuseAboveBytes = 512ull * 1024 * 1024;
};

struct SearchDefaults {
    bool         matchCase = false;
    bool         wholeWordOnly = false;
    bool         dotMatchesNewline = false;
    bool         wrapAround = true;
    std::wstring lastFilter = L"*.txt;*.cpp;*.h;*.hpp;*.md";
};

// 配置中心（设计文档 4.5 / 7.2）。
//
// 三条硬性要求，都是"别让配置把程序搞挂"：
//  1. 任何一项读取失败或非法都只记警告并回退默认值，绝不因此让启动失败；
//  2. 写盘是原子的（复用 io::WriteFileAtomic）；
//  3. 写盘请求合并——一秒内的多次改动只落一次盘。
class ConfigCenter {
public:
    // 解析配置根目录下的 config.xml。文件不存在时用一套默认值，不算失败。
    bool Load(std::wstring* error = nullptr);

    // force 为 true 时立即写盘；否则只在"距上次写盘超过 1 秒"时落盘，
    // 否则只是记下待写状态，等下一次请求或 Flush 时再写。
    bool Save(bool force, std::wstring* error = nullptr);

    // 请求保存（合并写盘）。真实写入可能延后到下一次请求或 Flush。
    void RequestSave();
    // 强制落盘。退出、系统关机前必须调用。
    void Flush();

    bool IsDirty() const noexcept { return dirty_; }
    static std::filesystem::path Path();

    GeneralConfig&  General() noexcept { return general_; }
    EditConfig&     Edit() noexcept { return edit_; }
    NewFileConfig&  NewFile() noexcept { return newFile_; }
    BackupConfig&   Backup() noexcept { return backup_; }
    LargeFileConfig& LargeFile() noexcept { return largeFile_; }
    SearchDefaults& Search() noexcept { return search_; }

    const GeneralConfig&  General() const noexcept { return general_; }
    const EditConfig&     Edit() const noexcept { return edit_; }
    const NewFileConfig&  NewFile() const noexcept { return newFile_; }
    const BackupConfig&   Backup() const noexcept { return backup_; }
    const LargeFileConfig& LargeFile() const noexcept { return largeFile_; }
    const SearchDefaults& Search() const noexcept { return search_; }

    // 加载过程中被回退的属性名，供"关于/诊断"展示
    const std::vector<std::wstring>& Warnings() const noexcept { return warnings_; }

private:
    GeneralConfig   general_;
    EditConfig      edit_;
    NewFileConfig   newFile_;
    BackupConfig    backup_;
    LargeFileConfig largeFile_;
    SearchDefaults  search_;
    std::vector<std::wstring> warnings_;
    long long       lastSaveTick_ = 0;
    bool            dirty_ = false;
};

}  // namespace npcc::core
