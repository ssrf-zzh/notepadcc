#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace npcc::core {
class CommandRegistry;
}

namespace npcc::plugin {

struct HostContext;   // 定义在 .cpp：它装的是函数表与回调，不该出现在公共头里

// 插件加载结果（每一个发现到的插件目录都有一条，无论成功还是失败——
// "为什么没加载"必须说得出来，这是插件宿主最容易让用户困惑的地方）
struct PluginStatus {
    std::wstring name;          // 稳定标识（plugin.json 的 name，缺省用目录名）
    std::wstring version;
    std::wstring author;
    std::wstring description;
    std::filesystem::path directory;
    std::filesystem::path dllPath;
    bool         loaded = false;
    bool         disabled = false;   // 被用户禁用（配置里写着），不是错误
    std::wstring reason;             // 没加载的原因（loaded=false 且 !disabled 时非空）

    bool Ok() const noexcept { return loaded || disabled; }
};

// 宿主给插件的那张函数表要能落到真实动作上，而这些动作在外壳手里（文档、命令、日志）。
// 所以宿主通过这个接口回调外壳——插件宿主本身不认识 MainWindow。
class HostActions {
public:
    virtual ~HostActions() = default;

    // 注册一条命令；返回 false 表示被拒（例如 key 重复）
    virtual bool RegisterPluginCommand(const std::wstring& pluginName, const std::wstring& key,
                                       const std::wstring& name, void (*execute)(void*),
                                       void* pluginCookie) = 0;
    virtual std::wstring ConfigPath() const = 0;
    virtual std::string  EditorText() const = 0;
    virtual bool         ReplaceSelection(std::string_view utf8) = 0;
    virtual void         ShowMessage(const std::wstring& text) = 0;
    virtual void         Log(int level, const std::wstring& text) = 0;
};

// 插件宿主（设计文档 2.10）。
//
// 生命周期：LoadAll →（运行期由插件回调宿主）→ ShutdownAll。
class PluginHost {
public:
    // 这两个都要在 .cpp 里定义：HostContext 是不完整类型，unique_ptr 的构造/析构
    // 需要它的完整定义（pimpl 的老规矩）
    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // 扫描目录并逐个加载（按 plugin.json 的 loadOrder 与依赖顺序）。被禁用的只登记不加载。
    // actions 由外壳提供，必须活到 ShutdownAll 之后。
    void LoadAll(HostActions& actions, core::CommandRegistry* registry);
    // 调用每个已加载插件的 Shutdown。
    // **不 FreeLibrary**：插件可能把回调留在了宿主里，卸库等于给悬空指针开门；
    // 要真正卸载就重启程序（这一点在设计文档里如实记着）
    void ShutdownAll();

    const std::vector<PluginStatus>& Plugins() const noexcept { return plugins_; }
    // 命令总开关（给外壳重建菜单用）
    std::size_t LoadedCount() const noexcept;

    // 用户禁用/启用：写进 <配置目录>\plugins.xml。**下次启动才生效**——因为不能安全卸库
    static bool SetDisabled(const std::wstring& name, bool disabled, std::wstring* error);
    static bool IsDisabled(const std::wstring& name);

    static std::filesystem::path PluginsRoot();   // <配置目录>\plugins
    static std::filesystem::path StatePath();     // <配置目录>\plugins.xml

private:
    struct Loaded {
        HMODULE      module = nullptr;
        void (*shutdown)() = nullptr;
        std::wstring name;
    };

    std::vector<PluginStatus> plugins_;
    std::vector<Loaded>       modules_;
    std::unique_ptr<HostContext> context_;
};

}  // namespace npcc::plugin
