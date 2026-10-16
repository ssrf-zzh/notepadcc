#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "edit/Encoding.h"
#include "core/ToolbarItems.h"

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
    // 函数列表面板是否显示（设计文档 2.6 第 5 条）。默认关：它占左侧一条，
    // 第一次用的用户不该莫名其妙少一块编辑区
    bool functionListVisible = false;
    // Markdown 预览面板的开关（与函数列表同一个待遇：面板的可见性记在配置里）
    bool markdownPreviewVisible = false;
    // JSON 树面板的开关（与 Markdown 预览互斥：两者都用右侧那一栏）
    bool jsonTreeVisible = false;
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

// 工具栏（设计文档 2.11 第 6 条）：可见性与按钮表都在配置里。
// items 为空 = "用代码里那份默认表"——**不是**"一个按钮都不放"：
// 一个按钮都不放是用户明确删空的结果，那种情况存的是一份只含分隔符被清空的表，
// 所以这里用"字段缺失"与"空表"两种状态区分（见 ConfigCenter::Save）。
struct ToolbarConfig {
    bool         visible = true;
    int          iconSize = 16;        // 逻辑像素，按 DPI 缩放后使用
    ToolbarItems items;                // 空 = 用默认表
    bool         custom = false;       // 用户动过按钮表（区分"没配过"与"配成了空表"）
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
    // 立刻落盘（无视 1 秒合并窗口）。用在"用户刚做的、应当马上生效"的改动上：
    // 合并窗口的本意是"把连着来的小改动攒一攒再写"，不是"把改动攒着不写"——
    // 窗口内的那次改动如果没人再触发一次保存，就会一直挂在内存里（进程被强杀就丢了）。
    void SaveNow();

    bool IsDirty() const noexcept { return dirty_; }
    static std::filesystem::path Path();

    GeneralConfig&  General() noexcept { return general_; }
    EditConfig&     Edit() noexcept { return edit_; }
    NewFileConfig&  NewFile() noexcept { return newFile_; }
    BackupConfig&   Backup() noexcept { return backup_; }
    LargeFileConfig& LargeFile() noexcept { return largeFile_; }
    SearchDefaults& Search() noexcept { return search_; }
    ToolbarConfig&  Toolbar() noexcept { return toolbar_; }

    const GeneralConfig&  General() const noexcept { return general_; }
    const EditConfig&     Edit() const noexcept { return edit_; }
    const NewFileConfig&  NewFile() const noexcept { return newFile_; }
    const BackupConfig&   Backup() const noexcept { return backup_; }
    const LargeFileConfig& LargeFile() const noexcept { return largeFile_; }
    const SearchDefaults& Search() const noexcept { return search_; }
    const ToolbarConfig&  Toolbar() const noexcept { return toolbar_; }

    // 加载过程中被回退的属性名，供"关于/诊断"展示
    const std::vector<std::wstring>& Warnings() const noexcept { return warnings_; }

private:
    GeneralConfig   general_;
    EditConfig      edit_;
    NewFileConfig   newFile_;
    BackupConfig    backup_;
    LargeFileConfig largeFile_;
    SearchDefaults  search_;
    ToolbarConfig   toolbar_;
    std::vector<std::wstring> warnings_;
    long long       lastSaveTick_ = 0;
    bool            dirty_ = false;
};

}  // namespace npcc::core
