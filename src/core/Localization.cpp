#include "core/Localization.h"

#include <algorithm>

#include "infra/Log.h"
#include "infra/Xml.h"
#include "io/XmlFile.h"

namespace npcc::core {
namespace {

// 简体中文这一档不是"一套包"，所以给它一个不与其他 locale 冲突的名字
constexpr const wchar_t* kChineseLocale = L"zh-CN";

// 把语言包文本拼成菜单项该有的样子：值 + "(&助记键)"。
//
// 助记符要插在结尾的省略号**前面**（"Open(&O)..." 而不是 "Open...(&O)"）：
// 省略号在菜单里是有含义的（"点了会弹对话框"），跟着助记符后面会被当成文字的一部分。
// 值里如果自己已经带了 "(&X)" 就不再补——手工写的语言包常常直接把助记符写进文本。
std::wstring WithAccel(const LangEntry& entry) {
    if (entry.accel.empty()) {
        return entry.value;
    }
    std::wstring body = entry.value;
    std::wstring tail;
    if (body.size() >= 3 && body.compare(body.size() - 3, 3, L"...") == 0) {
        tail = L"...";
        body.erase(body.size() - 3);
    }
    if (!body.empty() && body.back() == L')' && body.find(L"(&") != std::wstring::npos) {
        return entry.value;   // 已经带了助记符
    }
    return body + L"(&" + entry.accel + L")" + tail;
}

}  // namespace

const LangEntry* LangPack::Find(std::wstring_view key) const {
    for (const LangEntry& entry : entries) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

bool ParseLangPack(const infra::XmlElement& root, LangPack& out,
                   std::vector<std::wstring>* warnings) {
    if (root.name != L"LangPack") {
        if (warnings != nullptr) {
            warnings->push_back(L"语言包的根元素不是 LangPack");
        }
        return false;
    }
    out.entries.clear();
    if (const std::wstring* locale = root.Attribute(L"locale"); locale != nullptr) {
        out.locale = *locale;
    }
    if (const std::wstring* name = root.Attribute(L"name"); name != nullptr) {
        out.name = *name;
    }
    if (out.name.empty()) {
        out.name = out.locale;
    }
    for (const infra::XmlElement* node : root.Children(L"Entry")) {
        if (node == nullptr) {
            continue;
        }
        LangEntry entry;
        if (const std::wstring* key = node->Attribute(L"key"); key != nullptr) {
            entry.key = *key;
        }
        if (const std::wstring* value = node->Attribute(L"value"); value != nullptr) {
            entry.value = *value;
        }
        if (const std::wstring* accel = node->Attribute(L"accel"); accel != nullptr) {
            entry.accel = *accel;
        }
        if (entry.key.empty() || entry.value.empty()) {
            if (warnings != nullptr) {
                warnings->push_back(L"语言包里有条目缺 key 或 value，已跳过");
            }
            continue;
        }
        // 同一个键出现多次：以最后一条为准，但要说一声——手工合包时最常见的错
        bool duplicate = false;
        for (LangEntry& existing : out.entries) {
            if (existing.key == entry.key) {
                existing = entry;
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            if (warnings != nullptr) {
                warnings->push_back(L"语言包里键 " + entry.key + L" 出现了多次，以最后一条为准");
            }
            continue;
        }
        out.entries.push_back(std::move(entry));
    }
    return true;
}

std::string LangPackToXml(const LangPack& pack) {
    infra::XmlDocument document;
    infra::XmlElementPtr root = document.CreateRoot(L"LangPack");
    root->SetAttribute(L"locale", pack.locale);
    root->SetAttribute(L"name", pack.name);
    for (const LangEntry& entry : pack.entries) {
        infra::XmlElementPtr node = root->AddChild(L"Entry");
        node->SetAttribute(L"key", entry.key);
        node->SetAttribute(L"value", entry.value);
        if (!entry.accel.empty()) {
            node->SetAttribute(L"accel", entry.accel);
        }
    }
    return document.ToUtf8();
}

Localization& Localization::Ui() {
    static Localization instance;
    return instance;
}

bool Localization::ChineseDefault() const noexcept {
    return locale_ == kChineseLocale;
}

const LangPack* Localization::ActivePack() const {
    for (const LangPack& pack : packs_) {
        if (pack.locale == locale_) {
            return &pack;
        }
    }
    return nullptr;
}

std::wstring Localization::Tr(std::wstring_view key, std::wstring_view fallback) const {
    if (const LangPack* pack = ActivePack(); pack != nullptr) {
        if (const LangEntry* entry = pack->Find(key); entry != nullptr) {
            return WithAccel(*entry);
        }
        // 装了包却没有这一条：记一笔。中文那一档没有包（默认文本就是中文），
        // 所以只在"真的在讲别的语言"时才记账
        RecordMissedKey(key);
    }
    if (!fallback.empty()) {
        return std::wstring(fallback);
    }
    return std::wstring(key);   // 回退链的最后一档：把键本身显示出来
}

void Localization::RecordMissedKey(std::wstring_view key) const {
    const std::wstring text(key);
    for (const std::wstring& seen : missedSet_) {
        if (seen == text) {
            return;
        }
    }
    missedSet_.push_back(text);
    missedKeys_.push_back(text);
}

std::wstring Localization::Tr(std::wstring_view key, std::wstring_view fallback,
                              const std::vector<std::wstring>& args) const {
    std::wstring text = Tr(key, fallback);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::wstring placeholder = L"{" + std::to_wstring(index) + L"}";
        for (std::size_t at = text.find(placeholder); at != std::wstring::npos;
             at = text.find(placeholder, at + args[index].size())) {
            text.replace(at, placeholder.size(), args[index]);
        }
    }
    return text;
}

bool Localization::Covers(std::wstring_view key) const {
    const LangPack* pack = ActivePack();
    return pack != nullptr && pack->Find(key) != nullptr;
}

void Localization::Load(const std::wstring& locale, const std::filesystem::path& directory,
                        std::vector<std::wstring>* warnings) {
    std::vector<std::wstring> local;
    std::vector<std::wstring>& sink = warnings != nullptr ? *warnings : local;

    packs_ = BuiltinLangPacks();
    directory_ = directory;

    std::error_code ec;
    if (!directory.empty() && std::filesystem::is_directory(directory, ec)) {
        std::vector<std::filesystem::path> files;
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(directory, ec)) {
            if (!item.is_regular_file(ec)) {
                continue;
            }
            // 认 *.lang.xml（设计文档 6.6 第 1 条的老写法 *.xml 也收）
            const std::wstring name = item.path().filename().wstring();
            if (name.size() > 9 && name.compare(name.size() - 9, 9, L".lang.xml") == 0) {
                files.push_back(item.path());
                continue;
            }
            if (name.size() > 4 && name.compare(name.size() - 4, 4, L".xml") == 0) {
                files.push_back(item.path());
            }
        }
        // 目录枚举顺序在各机器上不一定一样，排一下，保证"同名时谁覆盖谁"是确定的
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files) {
            infra::XmlDocument document;
            std::wstring parseError;
            if (!io::LoadXmlFile(file.wstring(), document, &parseError)) {
                sink.push_back(L"语言包 " + file.filename().wstring() + L" 读不了：" + parseError);
                continue;
            }
            const infra::XmlElement* root = document.Root();
            if (root == nullptr) {
                sink.push_back(L"语言包 " + file.filename().wstring() + L" 是空的，已跳过");
                continue;
            }
            LangPack pack;
            if (!ParseLangPack(*root, pack, &sink)) {
                sink.push_back(L"语言包 " + file.filename().wstring() + L" 不是 LangPack，已跳过");
                continue;
            }
            if (pack.locale.empty()) {
                // 没写 locale 就按文件名认（手写包最常漏这个属性）
                std::wstring stem = file.filename().wstring();
                const std::size_t dot = stem.find(L'.');
                pack.locale = dot == std::wstring::npos ? stem : stem.substr(0, dot);
            }
            // 外置包**逐条**覆盖同 locale 的内置包（不是整体替换）。
            //
            // 整体替换看起来更"干净"，但它有个很难受的后果：用户一旦"导出模板"留了一份，
            // 之后每个版本**新增**的键都会被那份旧文件挡住——表现是"新功能一片中文"，
            // 而用户手里那份文件里根本没有那些键。逐条覆盖之后，用户改过的行以他的为准，
            // 没动过的行跟着程序升级走，两全。
            LangPack* existing = nullptr;
            for (LangPack& candidate : packs_) {
                if (candidate.locale == pack.locale) {
                    existing = &candidate;
                    break;
                }
            }
            if (existing == nullptr) {
                packs_.push_back(std::move(pack));
                continue;
            }
            if (!pack.name.empty() && pack.name != existing->locale) {
                existing->name = pack.name;   // 显示名以用户文件为准
            }
            for (const LangEntry& entry : pack.entries) {
                bool overwritten = false;
                for (LangEntry& slot : existing->entries) {
                    if (slot.key == entry.key) {
                        slot = entry;
                        overwritten = true;
                        break;
                    }
                }
                if (!overwritten) {
                    existing->entries.push_back(entry);
                }
            }
        }
    }

    if (!SetLocale(locale)) {
        sink.push_back(L"配置里的界面语言 \"" + locale + L"\" 没有对应的语言包，已用简体中文");
        SetLocale(kChineseLocale);
    }
    for (const std::wstring& warning : sink) {
        infra::LogWarn(L"lang", L"界面语言: " + warning);
    }
}

bool Localization::SetLocale(const std::wstring& locale) {
    if (locale == kChineseLocale) {
        locale_ = kChineseLocale;   // 中文这一档总在：代码里的默认文本就是它
        return true;
    }
    for (const LangPack& pack : packs_) {
        if (pack.locale == locale) {
            locale_ = locale;
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::wstring, std::wstring>> Localization::Available() const {
    std::vector<std::pair<std::wstring, std::wstring>> result;
    result.emplace_back(kChineseLocale, L"简体中文");
    std::vector<LangPack> sorted = packs_;
    std::sort(sorted.begin(), sorted.end(),
              [](const LangPack& left, const LangPack& right) {
                  return left.locale < right.locale;
              });
    for (const LangPack& pack : sorted) {
        if (pack.locale.empty() || pack.locale == kChineseLocale) {
            continue;   // 中文那一档已经放过了
        }
        result.emplace_back(pack.locale, pack.name.empty() ? pack.locale : pack.name);
    }
    return result;
}

std::wstring Tr(std::wstring_view key, std::wstring_view fallback) {
    return Localization::Ui().Tr(key, fallback);
}

std::wstring Tr(std::wstring_view key, std::wstring_view fallback,
                const std::vector<std::wstring>& args) {
    return Localization::Ui().Tr(key, fallback, args);
}

}  // namespace npcc::core
