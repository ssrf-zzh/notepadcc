#pragma once

#include <cstdint>

namespace npcc::infra::crashguard {

// 崩溃时能抢救的文档槽位。
//
// 这里刻意全部用 POD 与定长缓冲：崩溃处理器运行在"堆可能已经坏了"的状态下，
// 任何 new/malloc/std::string 都可能二次崩溃，把本来能救的内容一起葬掉。
// 所以内容由一个预先登记好的数组描述，处理器只做只读遍历。
struct RescueDocument {
    void*         view = nullptr;      // 提供者用来定位文档（这里是 Scintilla 窗口）
    int           index = -1;          // 交给提供者解释
    wchar_t       name[96] = {};
    wchar_t       path[512] = {};
    std::uint32_t codePage = 65001;
    // 编码与行尾存的是 edit::UniModeToken / EolToken 的文本，不存枚举值：
    // 存数字就得在崩溃处理器里再抄一份"数字到名字"的映射，而那份映射抄错
    // 是安静的错误——恢复出来的文件会莫名其妙多出 BOM。
    char          uniMode[16] = {};
    char          eol[8] = {};
    bool          hasBom = false;
    bool          modified = false;    // 只抢救有未保存改动的
};

// 取文档内容。先问长度、再要字节，两步分开是为了在崩溃路径上只申请一次内存。
// 由外壳实现——只有 edit 层允许碰 Scintilla 消息（README 的硬约定）。
using LengthFn = int (*)(void* context, int index);
using CopyFn = int (*)(void* context, int index, char* buffer, int capacity);

struct RescueProvider {
    LengthFn lengthOf = nullptr;
    CopyFn   copyInto = nullptr;
    void*    context = nullptr;
};

inline constexpr int kMaxRescueDocuments = 64;

// 安装顶层未处理异常过滤器。backupRoot 形如 <config>\backup。
// 越早调用越好：窗口还没建起来时崩了，同样要留下痕迹。
void Install(const wchar_t* backupRoot);

// 登记可抢救的文档数组。数组必须活到进程结束——处理器只持有指针，不做拷贝。
void SetDocuments(RescueDocument* documents, int count, const RescueProvider& provider);

// 自检开关用：故意触发一次真正的访问违例。
// 崩溃处理这条路径只有真崩一次才验证得了（见 README 的自检开关说明）。
void RaiseTestCrash();

}  // namespace npcc::infra::crashguard
