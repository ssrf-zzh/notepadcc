#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::infra {

struct XmlAttribute {
    std::wstring name;
    std::wstring value;
};

struct XmlElement;
using XmlElementPtr = std::shared_ptr<XmlElement>;

// 极简 XML DOM。够 ConfigCenter / SessionManager / 宏持久化使用，
// 不追求通用性（设计文档 1.2 选型：避免引入重型依赖）。
//
// 已知边界：不支持混合内容（同一元素里既有子元素又有文本时，
// 文本会被保留但序列化时排在子元素之前不再保持原顺序）。配置文件不会出现这种结构。
struct XmlElement {
    std::wstring               name;
    std::vector<XmlAttribute>  attributes;
    std::vector<XmlElementPtr> children;
    std::wstring               text;      // 仅叶子元素使用

    // 形参统一用 key 而非 name，避免与成员 name 同名（MSVC /W4 的 C4458）
    const std::wstring* Attribute(std::wstring_view key) const;
    std::wstring AttributeOr(std::wstring_view key, std::wstring_view fallback) const;
    int  AttributeInt(std::wstring_view key, int fallback) const;
    bool AttributeBool(std::wstring_view key, bool fallback) const;

    // 只重载字符串版本，其余类型用名字区分的函数。
    // 曾经写成同名重载，结果是 SetAttribute(L"path", L"") 里的 const wchar_t*
    // 会被优先转成 bool（标准转换优于转 wstring_view 的用户定义转换），
    // 空字符串被静默写成了 "no"。这种坑不值得为少打几个字符去踩。
    void SetAttribute(std::wstring_view key, std::wstring_view value);
    void SetAttributeInt(std::wstring_view key, int value);
    void SetAttributeUInt(std::wstring_view key, unsigned int value);
    void SetAttributeBool(std::wstring_view key, bool value);

    const XmlElement* Child(std::wstring_view childName) const;
    std::vector<const XmlElement*> Children(std::wstring_view childName) const;
    XmlElementPtr AddChild(std::wstring_view childName);
};

class XmlDocument {
public:
    // 输入为 UTF-8 字节。失败时填写 error（含出错位置），不改动 out。
    static bool Parse(std::string_view utf8, XmlDocument& out, std::wstring* error);

    XmlElementPtr CreateRoot(std::wstring_view rootName);
    void          SetRoot(XmlElementPtr root) { root_ = std::move(root); }
    XmlElement*   Root() const noexcept { return root_.get(); }

    // 带 XML 声明与两空格缩进的 UTF-8 文本。落盘请走 io::SaveXmlFile。
    std::string ToUtf8() const;

private:
    XmlElementPtr root_;
};

}  // namespace npcc::infra
