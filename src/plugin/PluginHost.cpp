#include "plugin/PluginHost.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "core/CommandRegistry.h"
#include "core/JsonFlat.h"
#include "infra/Log.h"
#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/FileReader.h"
#include "io/XmlFile.h"
#include "notepadcc_plugin.h"

namespace npcc::plugin {
namespace {

constexpr int            kSchemaVersion = 1;
constexpr const wchar_t* kStateRoot = L"NotepadccPlugins";
constexpr uint32_t       kHostAbi = NPCC_PLUGIN_ABI_VERSION;

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                          need);
    return out;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          need, nullptr, nullptr);
    return out;
}

// plugin.json（设计文档 7.8）：扁平对象，认得的字段就这些
struct PluginManifest {
    std::wstring              name;
    std::wstring              version = L"0.0.0";
    std::wstring              author;
    std::wstring              description;
    std::wstring              dll;
    int                       loadOrder = 100;
    std::wstring              minHostVersion;
    std::vector<std::wstring> dependencies;
    bool                      disabled = false;
};

std::vector<std::wstring> SplitList(const std::string& utf8List) {
    std::vector<std::wstring> items;
    std::string current;
    const auto flush = [&items, &current]() {
        if (!current.empty()) {
            items.push_back(Widen(current));
            current.clear();
        }
    };
    for (const char ch : utf8List) {
        if (ch == ',' || ch == ';' || ch == ' ') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return items;
}

bool ReadManifest(const std::filesystem::path& directory, PluginManifest& manifest,
                  std::wstring& reason) {
    manifest.name = directory.filename().wstring();   // 缺省用目录名
    const std::filesystem::path path = directory / L"plugin.json";
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;   // 没有清单也认：一切用缺省值（裸 DLL 插件）
    }
    const io::ReadResult read = io::ReadWholeFile(path.wstring(), 256ull * 1024);
    if (!read.ok) {
        reason = L"读不了 plugin.json：" + read.message;
        return false;
    }
    core::JsonObject object;
    std::wstring error;
    if (!core::ParseJsonObject(read.bytes, object, &error)) {
        reason = L"plugin.json 解析失败：" + error;
        return false;
    }
    manifest.name = Widen(object.StringOr("name", Narrow(directory.filename().wstring())));
    manifest.version = Widen(object.StringOr("version", "0.0.0"));
    manifest.author = Widen(object.StringOr("author", ""));
    manifest.description = Widen(object.StringOr("description", ""));
    manifest.dll = Widen(object.StringOr("dll", ""));
    manifest.loadOrder = static_cast<int>(object.NumberOr("loadOrder", 100));
    manifest.minHostVersion = Widen(object.StringOr("minHostVersion", ""));
    manifest.disabled = object.BoolOr("disabled", false);
    if (object.Has("dependencies")) {
        manifest.dependencies = SplitList(object.StringOr("dependencies", ""));
    }
    return true;
}

// 宿主自己的"哪个插件被禁用了"记录。**不改第三方插件的 plugin.json**（那是人家的文件），
// 状态写在我们自己的 plugins.xml 里。
class DisabledSet {
public:
    static DisabledSet Load() {
        DisabledSet state;
        const std::filesystem::path path = PluginHost::StatePath();
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return state;
        }
        infra::XmlDocument document;
        std::wstring error;
        if (!io::LoadXmlFile(path.wstring(), document, &error)) {
            infra::LogWarn(L"plugin", L"插件状态文件读不了，按全部启用处理: " + error);
            return state;
        }
        const infra::XmlElement* root = document.Root();
        if (root == nullptr || root->name != kStateRoot) {
            return state;
        }
        for (const infra::XmlElement* node : root->Children(L"Plugin")) {
            if (node == nullptr) {
                continue;
            }
            const std::wstring name = node->AttributeOr(L"name", L"");
            if (!name.empty() && node->AttributeBool(L"disabled", false)) {
                state.disabled_.insert(name);
            }
        }
        return state;
    }

    bool Contains(const std::wstring& name) const {
        return disabled_.find(name) != disabled_.end();
    }

    bool Save(std::wstring* error) const {
        infra::XmlDocument document;
        infra::XmlElementPtr root = document.CreateRoot(kStateRoot);
        root->SetAttributeInt(L"schemaVersion", kSchemaVersion);
        for (const std::wstring& name : disabled_) {
            infra::XmlElementPtr node = root->AddChild(L"Plugin");
            node->SetAttribute(L"name", name);
            node->SetAttributeBool(L"disabled", true);
        }
        return io::SaveXmlFile(PluginHost::StatePath().wstring(), document, error);
    }

    std::set<std::wstring>& Mutable() noexcept { return disabled_; }

private:
    std::set<std::wstring> disabled_;
};

// 解释一次就够：读文件在启动路径上，每个插件都去读一遍没必要。
// Reset() 给单元测试用（测试之间要互不影响）
DisabledSet& DisabledCache() {
    static DisabledSet cache = DisabledSet::Load();
    return cache;
}

bool IsNewerVersion(std::wstring_view required, std::wstring_view actual) {
    // 只比形如 1.2.3 的数字段；缺段当 0。比不下去就不拦（宁可放行也不因为版本号格式
    // 奇怪而把插件全挡住）
    const auto parts = [](std::wstring_view text) {
        std::vector<int> numbers;
        int current = 0;
        bool any = false;
        for (const wchar_t ch : text) {
            if (ch >= L'0' && ch <= L'9') {
                current = current * 10 + (ch - L'0');
                any = true;
                continue;
            }
            if (any) {
                numbers.push_back(current);
                current = 0;
                any = false;
            }
        }
        if (any) {
            numbers.push_back(current);
        }
        return numbers;
    };
    const std::vector<int> want = parts(required);
    const std::vector<int> have = parts(actual);
    for (std::size_t index = 0; index < want.size(); ++index) {
        const int left = index < have.size() ? have[index] : 0;
        if (left != want[index]) {
            return left > want[index];
        }
    }
    return true;
}

}  // namespace

// 宿主上下文：函数表里的所有回调都拿它当 hostCookie。
// 它由 PluginHost 持有——插件会把 hostCookie 留到 Shutdown 之后，所以不能提前释放
struct HostContext {
    HostActions*       actions = nullptr;
    NpccHostServices   services{};
    std::wstring       currentPlugin;   // 正在加载谁（注册命令时要加前缀）
    // 插件注册的命令：函数指针与它自己的 cookie 都存这儿，注册表里的回调只拿指针
    struct CommandThunk {
        void (*execute)(void*) = nullptr;
        void* cookie = nullptr;
    };
    std::vector<std::unique_ptr<CommandThunk>> commands;
};

namespace {

HostContext* ContextOf(void* cookie) {
    return static_cast<HostContext*>(cookie);
}

void PluginCommandTrampoline(void* cookie);

void HostRegisterCommand(void* cookie, const NpccCommandDesc* desc) {
    HostContext* context = ContextOf(cookie);
    if (context == nullptr || context->actions == nullptr || desc == nullptr ||
        desc->key == nullptr || desc->execute == nullptr) {
        return;
    }
    auto thunk = std::make_unique<HostContext::CommandThunk>();
    thunk->execute = desc->execute;
    thunk->cookie = desc->pluginCookie;
    HostContext::CommandThunk* raw = thunk.get();
    context->commands.push_back(std::move(thunk));
    // 命令的实际执行体是插件里的函数；注册表里存的是这个跳板 + 插件自己的 cookie
    context->actions->RegisterPluginCommand(context->currentPlugin, desc->key,
                                           desc->name != nullptr ? desc->name : desc->key,
                                           &PluginCommandTrampoline, raw);
}

void PluginCommandTrampoline(void* cookie) {
    auto* thunk = static_cast<HostContext::CommandThunk*>(cookie);
    if (thunk != nullptr && thunk->execute != nullptr) {
        thunk->execute(thunk->cookie);
    }
}

void* HostAlloc(void*, size_t bytes) {
    return ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
}

void HostFree(void*, void* pointer) {
    if (pointer != nullptr) {
        ::HeapFree(::GetProcessHeap(), 0, pointer);
    }
}

size_t HostGetConfigPath(void* cookie, wchar_t* buf, size_t cch) {
    HostContext* context = ContextOf(cookie);
    if (context == nullptr || context->actions == nullptr) {
        return 0;
    }
    const std::wstring path = context->actions->ConfigPath();
    if (buf == nullptr) {
        return path.size() + 1;
    }
    if (cch == 0) {
        return 0;
    }
    const size_t copied = std::min(cch - 1, path.size());
    std::wmemcpy(buf, path.c_str(), copied);
    buf[copied] = L'\0';
    return copied;
}

size_t HostGetEditorText(void* cookie, char* buf, size_t bytes) {
    HostContext* context = ContextOf(cookie);
    if (context == nullptr || context->actions == nullptr) {
        return 0;
    }
    const std::string text = context->actions->EditorText();
    if (buf == nullptr) {
        return text.size();
    }
    const size_t copied = std::min(bytes, text.size());
    std::memcpy(buf, text.data(), copied);
    return copied;
}

int HostSetEditorText(void* cookie, const char* utf8, size_t bytes) {
    HostContext* context = ContextOf(cookie);
    if (context == nullptr || context->actions == nullptr || utf8 == nullptr) {
        return 0;
    }
    return context->actions->ReplaceSelection(std::string_view(utf8, bytes)) ? 1 : 0;
}

void HostShowMessage(void* cookie, const wchar_t* text) {
    HostContext* context = ContextOf(cookie);
    if (context != nullptr && context->actions != nullptr && text != nullptr) {
        context->actions->ShowMessage(text);
    }
}

void HostLog(void* cookie, int level, const wchar_t* text) {
    HostContext* context = ContextOf(cookie);
    if (context != nullptr && context->actions != nullptr && text != nullptr) {
        context->actions->Log(level, text);
    }
}

intptr_t HostScintillaSend(void* cookie, intptr_t, uint32_t message, uintptr_t, intptr_t) {
    // 见 SDK 头里的说明：这一版不给插件直通 Scintilla 消息（分层约定），
    // 但字段留着、调用有明确回应，免得插件以为"调了没反应"
    HostLog(cookie, 1, L"插件调用了 ScintillaSend，但本版不支持（只有 EditView 允许碰 Scintilla）");
    (void)message;
    return -1;
}

NpccHostServices BuildServices(HostContext* context) {
    NpccHostServices services{};
    services.size = sizeof(NpccHostServices);
    services.abiVersion = kHostAbi;
    services.hostCookie = context;
    services.RegisterCommand = &HostRegisterCommand;
    services.Alloc = &HostAlloc;
    services.Free = &HostFree;
    services.GetConfigPath = &HostGetConfigPath;
    services.GetEditorText = &HostGetEditorText;
    services.SetEditorText = &HostSetEditorText;
    services.ShowMessage = &HostShowMessage;
    services.Log = &HostLog;
    services.ScintillaSend = &HostScintillaSend;
    return services;
}

std::wstring FileName(const std::filesystem::path& path) {
    return path.filename().wstring();
}

}  // namespace

std::filesystem::path PluginHost::PluginsRoot() {
    return infra::ConfigRoot() / L"plugins";
}

std::filesystem::path PluginHost::StatePath() {
    return infra::ConfigRoot() / L"plugins.xml";
}

PluginHost::~PluginHost() {
    ShutdownAll();
}

PluginHost::PluginHost() = default;

std::size_t PluginHost::LoadedCount() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(plugins_.begin(), plugins_.end(),
                      [](const PluginStatus& status) { return status.loaded; }));
}

bool PluginHost::IsDisabled(const std::wstring& name) {
    return DisabledCache().Contains(name);
}

bool PluginHost::SetDisabled(const std::wstring& name, bool disabled, std::wstring* error) {
    DisabledSet& cache = DisabledCache();
    if (disabled) {
        cache.Mutable().insert(name);
    } else {
        cache.Mutable().erase(name);
    }
    return cache.Save(error);
}

void PluginHost::LoadAll(HostActions& actions, core::CommandRegistry*) {
    ShutdownAll();
    plugins_.clear();
    context_ = std::make_unique<HostContext>();
    context_->actions = &actions;
    context_->services = BuildServices(context_.get());

    // 发现：<配置目录>\plugins\* 每个子目录算一个插件（也允许直接放 .dll 的裸插件）
    struct Candidate {
        PluginManifest manifest;
        std::filesystem::path directory;
        std::filesystem::path dll;
    };
    std::vector<Candidate> candidates;
    std::error_code ec;
    const std::filesystem::path root = PluginsRoot();
    if (std::filesystem::is_directory(root, ec)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root, ec)) {
            Candidate candidate;
            candidate.directory = entry.path();
            std::wstring reason;
            if (entry.is_directory(ec)) {
                if (!ReadManifest(candidate.directory, candidate.manifest, reason)) {
                    PluginStatus status;
                    status.name = FileName(candidate.directory);
                    status.directory = candidate.directory;
                    status.reason = reason;
                    plugins_.push_back(std::move(status));
                    continue;
                }
                candidate.dll = candidate.manifest.dll.empty()
                                    ? candidate.directory / (candidate.manifest.name + L".dll")
                                    : candidate.directory / candidate.manifest.dll;
            } else if (entry.is_regular_file(ec) &&
                       entry.path().extension() == L".dll") {
                candidate.manifest.name = entry.path().stem().wstring();
                candidate.dll = entry.path();
            } else {
                continue;
            }
            candidates.push_back(std::move(candidate));
        }
    }
    // 装载顺序：loadOrder 小的先来，其次按名字（稳定，便于复现问题）
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.manifest.loadOrder != b.manifest.loadOrder) {
                      return a.manifest.loadOrder < b.manifest.loadOrder;
                  }
                  return a.manifest.name < b.manifest.name;
              });

    std::set<std::wstring> loadedNames;
    for (Candidate& candidate : candidates) {
        PluginStatus status;
        status.name = candidate.manifest.name;
        status.version = candidate.manifest.version;
        status.author = candidate.manifest.author;
        status.description = candidate.manifest.description;
        status.directory = candidate.directory;
        status.dllPath = candidate.dll;

        const auto reject = [&](const std::wstring& why) {
            status.loaded = false;
            status.reason = why;
            plugins_.push_back(std::move(status));
        };
        if (candidate.manifest.disabled || DisabledCache().Contains(status.name)) {
            status.disabled = true;
            status.reason = L"已被禁用";
            plugins_.push_back(std::move(status));
            continue;
        }
        if (::GetFileAttributesW(candidate.dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            reject(L"找不到 DLL：" + candidate.dll.filename().wstring());
            continue;
        }
        // 依赖：依赖的插件必须**已经**加载成功（排在前面的先加载）
        bool missingDependency = false;
        for (const std::wstring& dependency : candidate.manifest.dependencies) {
            if (loadedNames.find(dependency) == loadedNames.end()) {
                reject(L"缺少依赖插件：" + dependency);
                missingDependency = true;
                break;
            }
        }
        if (missingDependency) {
            continue;
        }
        if (!candidate.manifest.minHostVersion.empty() &&
            !IsNewerVersion(candidate.manifest.minHostVersion, L"1.0.0")) {
            reject(L"要求宿主版本 " + candidate.manifest.minHostVersion + L"，当前是 1.0.0");
            continue;
        }

        // 位数：32 位 DLL 在 x64 进程里 LoadLibrary 会以 216（ERROR_BAD_EXE_FORMAT）失败，
        // 这句话直接写给用户看
        HMODULE module = ::LoadLibraryW(candidate.dll.c_str());
        if (module == nullptr) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_BAD_EXE_FORMAT) {
                reject(L"不是 x64 的 DLL（插件必须是 64 位）");
            } else {
                reject(L"加载 DLL 失败（错误码 " + std::to_wstring(error) + L"）");
            }
            continue;
        }
        const auto getInfo = reinterpret_cast<NpccPluginGetInfoFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, NPCC_PLUGIN_GET_INFO_NAME)));
        const auto init = reinterpret_cast<NpccPluginInitFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, NPCC_PLUGIN_INIT_NAME)));
        const auto shutdown = reinterpret_cast<NpccPluginShutdownFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, NPCC_PLUGIN_SHUTDOWN_NAME)));
        if (getInfo == nullptr || init == nullptr || shutdown == nullptr) {
            const wchar_t* missing = getInfo == nullptr ? L"NpccPlugin_GetInfo"
                                     : init == nullptr ? L"NpccPlugin_Init"
                                                       : L"NpccPlugin_Shutdown";
            ::FreeLibrary(module);
            reject(std::wstring(L"缺少导出函数 ") + missing);
            continue;
        }
        const NpccPluginInfo* info = nullptr;
        try {
            info = getInfo();
        } catch (...) {
            ::FreeLibrary(module);
            reject(L"GetInfo 抛出了异常");
            continue;
        }
        if (info == nullptr) {
            ::FreeLibrary(module);
            reject(L"GetInfo 返回了空指针");
            continue;
        }
        // **先把要用的值取出来**：下面可能 FreeLibrary，卸库之后 info 指向的就是已经
        // 不属于我们的内存了（踩过：在 reject 的实参里读 info->abiVersion，直接访问违例）
        const uint32_t pluginAbi = info->abiVersion;
        const std::wstring pluginName = info->name != nullptr ? info->name : L"";
        if (pluginAbi != kHostAbi) {
            ::FreeLibrary(module);
            reject(L"ABI 版本不匹配：插件要 " + std::to_wstring(pluginAbi) +
                   L"，宿主是 " + std::to_wstring(kHostAbi));
            continue;
        }
        if (!pluginName.empty()) {
            status.name = pluginName;   // 以插件自己报的名字为准（目录名可能被改过）
        }
        bool ok = false;
        std::wstring failure;
        context_->currentPlugin = status.name;   // 这条命令属于谁（注册时要加前缀）
        try {
            // 双向校验：插件也要检查我们这张表的大小与版本
            ok = init(&context_->services) == 0;
            if (!ok) {
                failure = L"Init 返回了失败";
            }
        } catch (const std::exception& error) {
            failure = L"Init 抛出异常：" + Widen(error.what());
        } catch (...) {
            failure = L"Init 抛出了未知异常";
        }
        if (!ok) {
            ::FreeLibrary(module);
            reject(failure.empty() ? L"Init 失败" : failure);
            continue;
        }
        loadedNames.insert(status.name);
        status.loaded = true;
        modules_.push_back(Loaded{module, shutdown, status.name});
        infra::LogInfo(L"plugin", L"已加载插件：" + status.name + L" " + status.version);
        plugins_.push_back(std::move(status));
    }

    for (const PluginStatus& status : plugins_) {
        if (!status.Ok()) {
            infra::LogWarn(L"plugin", L"插件未加载：" + status.name + L" / " + status.reason);
        }
    }
}

void PluginHost::ShutdownAll() {
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (it->shutdown == nullptr) {
            continue;
        }
        try {
            it->shutdown();   // 逆序关闭：先加载的后关
        } catch (...) {
            infra::LogWarn(L"plugin", L"插件 Shutdown 抛了异常：" + it->name);
        }
    }
    modules_.clear();
    context_.reset();
}

}  // namespace npcc::plugin
