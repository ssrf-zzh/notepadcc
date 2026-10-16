#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Ids.h"
#include "core/ConfigCenter.h"
#include "edit/Document.h"
#include "edit/FileTier.h"
#include "edit/LanguageRegistry.h"
#include "edit/StyleScheme.h"

namespace npcc::core {

enum class OpenStatus {
    Ok,
    NotFound,
    AccessDenied,
    TooLarge,
    EncodingFailed,
    AlreadyOpen,
    InternalError,
};

struct OpenOutcome {
    edit::Document* document = nullptr;
    OpenStatus      status   = OpenStatus::Ok;
    std::wstring    message;
};

struct SaveOutcome {
    bool         ok = false;
    std::wstring message;
};

// 文档生命周期的唯一权威。外壳只负责问"要不要保存"和呈现结果，
// 文件读写、编码判定、缓冲区身份都在这里（设计文档 4.2）。
class DocumentManager {
public:
    explicit DocumentManager(HWND viewParent);
    ~DocumentManager();

    // 大文件分档阈值（设计文档 6.1）。可配置，这里先给默认值。
    edit::TierPolicy& TierPolicyRef() noexcept { return tierPolicy_; }

    // 新建文档时使用的编码与行尾（来自配置中心的"新建"分页）
    void SetNewFileDefaults(edit::UniMode mode, edit::EolMode eol, std::uint32_t codePage);

    // 语言定义（可被用户的 langs.xml 覆盖）。为空时用内置表。
    // 只持有指针：语言表的生命周期由外壳负责。
    void SetLanguages(const edit::LanguageRegistry* languages) noexcept { languages_ = languages; }
    // 新文档的"控件皮肤"用哪套（浅色/深色）——由外壳在启动与切换主题时告知。
    // 不这么做的话，深色模式里每新开一个标签都会先闪一下浅色的边距。
    void SetPalette(const edit::EditorPalette& palette) noexcept { palette_ = palette; }
    const edit::EditorPalette& Palette() const noexcept { return palette_; }
    // 样式方案（用户可调的配色）。为空时用内置默认配色。
    void SetStyles(const edit::StyleScheme* styles) noexcept { styles_ = styles; }

    DocumentManager(const DocumentManager&) = delete;
    DocumentManager& operator=(const DocumentManager&) = delete;

    // 新建空白文档并激活
    edit::Document* CreateUntitled();

    // 打开文件；已在标签中打开时直接激活并返回 AlreadyOpen
    OpenOutcome Open(const std::wstring& path);

    // 保存到原路径 / 另存为
    SaveOutcome Save(edit::Document& document);
    SaveOutcome SaveAs(edit::Document& document, const std::wstring& path);
    // T3 只读文档的"编辑此文件"：把内容复制进一份新的可编辑文档（原文件不动，
    // 副本标成"已修改"，第一次保存会问存到哪）。失败返回 nullptr，原因在 error。
    edit::Document* OpenAsEditableCopy(edit::Document& source, std::wstring& error);

    // ---- 外部改动检测（设计文档 2.3 第 3 条） ----
    // 把磁盘上的身份与最近一次记录的比较。文件被删除也算"变了"。
    bool HasExternallyChanged(const edit::Document& document) const;
    // 用当前磁盘状态刷新记录。用户选择"保留当前内容"时调用。
    void RefreshIdentity(edit::Document& document);
    // 从磁盘重新装载，保留光标与滚动位置。
    bool Reload(edit::Document& document, std::wstring& error);

    // 关闭并从标签顺序中移除（不询问"是否保存"，由外壳决定）
    bool Close(edit::Document* document);

    edit::Document* Find(BufferId id) const;
    edit::Document* Active() const { return active_; }
    void Activate(BufferId id);
    void Activate(edit::Document* document);

    const std::vector<BufferId>& TabOrder() const noexcept { return tabOrder_; }
    // 把 moving 移到 before 之前；before 传 kInvalidId 表示移到末尾。
    // 注意：缓冲区 ID 从 1 开始编，0 永远不是合法 ID，这样 0 可以安全地
    // 用作"无活动标签"之类的哨兵值（见 Ids.h 的 kInvalidId）。
    void MoveBefore(BufferId moving, BufferId before);
    void SetTabOrder(const std::vector<BufferId>& order);

    bool AnyModified() const;
    std::vector<edit::Document*> All() const;
    size_t Count() const noexcept { return documents_.size(); }

    // 视图可见性：同一时刻只显示活动文档的控件
    void ShowOnly(edit::Document* document) const;
    void ResizeActive(const RECT& area) const;

    // ---- 分屏 / 克隆视图（设计文档 6.3，ADR-07：一文档多视图）----
    // 第二个视图与主视图共用同一份 Scintilla Document：文本、标记、折叠状态是同一份，
    // 各自只保留自己的光标与滚动位置。它不持有文档，只"指向"当前活动文档。
    bool            SplitEnableView();
    void            SplitDisableView();
    bool            SplitEnabled() const noexcept { return splitView_ != nullptr; }
    edit::EditView* SplitView() const noexcept { return splitView_.get(); }
    // 让克隆视图跟着某个文档走（活动文档变化、文档关闭后都要调）
    void SplitAttach(edit::Document& document) const;
    void SplitResize(const RECT& area) const;

    static std::wstring FileNameOf(const std::wstring& path);
    static std::wstring DirectoryOf(const std::wstring& path);

private:
    edit::Document* AddDocument(std::unique_ptr<edit::Document> document);
    // 把文件内容装载进文档（Open 与 Reload 共用）
    // forcedTier 非空时强制档位（"以副本方式编辑"用它把 T3 大文件按 T2 装进 Scintilla）
    bool LoadInto(edit::Document& document, std::wstring& error, OpenStatus& status,
                  const std::optional<edit::FileTier>& forcedTier = std::nullopt);
    void RemoveFromTabOrder(BufferId id);
    int  NextControlId();

    HWND                                     viewParent_ = nullptr;
    std::vector<std::unique_ptr<edit::Document>> documents_;
    std::vector<BufferId>                    tabOrder_;
    edit::Document*                          active_ = nullptr;
    int                                      nextControlId_ = 100;
    // 克隆视图用的控件 ID 固定：同一时刻只会有一个
    static constexpr int                     kSplitControlId = 900;
    std::unique_ptr<edit::EditView>          splitView_;
    int                                      nextBufferId_ = 1;   // 0 保留给"无效"
    int                                      untitledCounter_ = 0;
    edit::TierPolicy                         tierPolicy_;
    edit::UniMode                            newFileUniMode_ = edit::UniMode::Utf8;
    edit::EolMode                            newFileEol_ = edit::EolMode::Crlf;
    std::uint32_t                            newFileCodePage_ = 936;
    const edit::LanguageRegistry*            languages_ = nullptr;
    const edit::StyleScheme*                 styles_ = nullptr;
    edit::EditorPalette                      palette_ = edit::EditorPalette::Light();
};

}  // namespace npcc::core
