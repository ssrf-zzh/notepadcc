#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "core/Ids.h"
#include "edit/Document.h"

namespace npcc::core {

enum class OpenStatus {
    Ok,
    NotFound,
    AccessDenied,
    TooLarge,
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

    DocumentManager(const DocumentManager&) = delete;
    DocumentManager& operator=(const DocumentManager&) = delete;

    // 新建空白文档并激活
    edit::Document* CreateUntitled();

    // 打开文件；已在标签中打开时直接激活并返回 AlreadyOpen
    OpenOutcome Open(const std::wstring& path);

    // 保存到原路径 / 另存为
    SaveOutcome Save(edit::Document& document);
    SaveOutcome SaveAs(edit::Document& document, const std::wstring& path);

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

    static std::wstring FileNameOf(const std::wstring& path);
    static std::wstring DirectoryOf(const std::wstring& path);

private:
    edit::Document* AddDocument(std::unique_ptr<edit::Document> document);
    void RemoveFromTabOrder(BufferId id);
    int  NextControlId();

    HWND                                     viewParent_ = nullptr;
    std::vector<std::unique_ptr<edit::Document>> documents_;
    std::vector<BufferId>                    tabOrder_;
    edit::Document*                          active_ = nullptr;
    int                                      nextControlId_ = 100;
    int                                      nextBufferId_ = 1;   // 0 保留给"无效"
    int                                      untitledCounter_ = 0;
};

}  // namespace npcc::core
