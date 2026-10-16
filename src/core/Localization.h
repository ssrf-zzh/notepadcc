#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace npcc::infra {
struct XmlElement;
}

namespace npcc::core {

// 一条语言包条目（设计文档 7.7）。
struct LangEntry {
    std::wstring key;
    std::wstring value;
    // 助记键：单独给一个字段，而不是把 "(&S)" 写在 value 里。不同语言下合适的
    // 加速字母本来就不同（英文 Save 用 S，中文"保存"还迁就 S 就很别扭），
    // 混在文本里就没法按语言换了（设计文档 6.6 第 4 条）。
    std::wstring accel;
};

// 一套语言包。纯数据 + 查询，不做 IO。
struct LangPack {
    std::wstring             locale;   // zh-CN / en-US …
    std::wstring             name;     // 显示名（"English"）
    std::vector<LangEntry>   entries;

    const LangEntry* Find(std::wstring_view key) const;
};

// 解析 <LangPack locale="…" name="…"><Entry key="…" value="…" accel="…"/></LangPack>。
// 缺 key 或缺 value 的条目跳过并记警告；同一个键出现多次以最后一条为准（也记一条警告）。
bool ParseLangPack(const infra::XmlElement& root, LangPack& out,
                   std::vector<std::wstring>* warnings);
// 反向写出来（"导出语言包模板"用）。与 ParseLangPack 对称，测试里做往返。
std::string LangPackToXml(const LangPack& pack);

// 编译进来的语言包。**简体中文不在这里**：代码里写下的默认文本就是简体中文，
// 再抄一份进语言包只会变成"改了代码忘了改语言包"的老问题。
const std::vector<LangPack>& BuiltinLangPacks();

// 界面语言（设计文档 6.6）。
//
// 回退链：**当前语言的包 → 代码里的默认文本（简体中文）→ 键本身**。
// 第三档只在调用方没给默认文本时才会走到（例如以后新加的对话框文案）。
class Localization {
public:
    // 进程内唯一一份：菜单、对话框、插件都问它
    static Localization& Ui();

    // 装包：先放内置的，再读 directory 下的 *.lang.xml（同 locale 覆盖内置），
    // 最后选中 locale；选不中就回简体中文（不装包，直接用代码里的默认文本）。
    // 文件读不了或格式不对只记警告，不算失败——界面必须能起来。
    void Load(const std::wstring& locale, const std::filesystem::path& directory,
              std::vector<std::wstring>* warnings);

    const std::wstring& Locale() const noexcept { return locale_; }
    // 当前用的是代码里的中文默认文本（没有装包）
    bool ChineseDefault() const noexcept;
    const LangPack* ActivePack() const;

    // 取一条文本。fallback 是代码里写下的默认文本（简体中文），可以为空。
    std::wstring Tr(std::wstring_view key, std::wstring_view fallback) const;
    // 取一条文本并替换 {0}、{1}……占位符。
    //
    // 占位符是"按语言给整句"的前提：像"「名字」必须是 1 到 64 之间的整数"这种句子，
    // 逐段拼接在英文里必然读不通（语序不同、冠词不同）。所以凡是带变量的提示都应当
    // 走这一条，把整句交给语言包。参数不够时占位符**原样保留**（看得见才好修）。
    std::wstring Tr(std::wstring_view key, std::wstring_view fallback,
                    const std::vector<std::wstring>& args) const;
    // 当前语言的包**覆没覆盖**这个键。外壳据此把缺键写进日志（设计文档 6.6 第 2 条）
    bool Covers(std::wstring_view key) const;
    // 用到过、但当前语言的包没盖住的键（去重，按第一次用到的顺序）。
    //
    // 这是"界面文案进语言包"这件事的自报机制：命令那边可以拿注册表逐条核对，
    // 而对话框里的文案没有一张静态表可查——只能靠"真用到的时候记一笔"。
    // 英文界面里冒出中文时，这条记录是唯一的线索（见 9.3 的界面语言那一段）。
    const std::vector<std::wstring>& MissedKeys() const noexcept { return missedKeys_; }
    void ClearMissedKeys() {
        missedKeys_.clear();
        missedSet_.clear();
    }

    // 可选的界面语言（locale, 显示名）。第 0 项固定是简体中文（内置默认文本），
    // 其后是各套语言包，按 locale 排序——顺序稳定，菜单里的命令号才稳定。
    std::vector<std::pair<std::wstring, std::wstring>> Available() const;
    // 只切"用哪套包"，不重新读文件
    bool SetLocale(const std::wstring& locale);

    // 当前装着的语言包目录（导出模板用）
    const std::filesystem::path& Directory() const noexcept { return directory_; }

private:
    void RecordMissedKey(std::wstring_view key) const;

    std::vector<LangPack>  packs_;
    std::wstring           locale_ = L"zh-CN";
    std::filesystem::path  directory_;
    // Tr 是 const 的（到处都在用），所以这两个是 mutable：它们只是"记账"，不改语言本身
    mutable std::vector<std::wstring>          missedKeys_;
    mutable std::vector<std::wstring>          missedSet_;
};

// 便捷入口：菜单与对话框里到处都要用
std::wstring Tr(std::wstring_view key, std::wstring_view fallback);
std::wstring Tr(std::wstring_view key, std::wstring_view fallback,
                const std::vector<std::wstring>& args);

}  // namespace npcc::core
