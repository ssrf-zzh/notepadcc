#include "core/PrintPaging.h"

namespace npcc::core {

std::vector<int32_t> CollectPageStarts(int32_t length,
                                       const std::function<intptr_t(int32_t)>& pageEnd) {
    std::vector<int32_t> starts;
    if (length <= 0 || !pageEnd) {
        return starts;
    }
    int32_t position = 0;
    for (;;) {
        const intptr_t next = pageEnd(position);
        if (next < 0) {
            break;   // 排不动了（控件没准备好、DC 失效之类）
        }
        starts.push_back(position);
        if (next >= length) {
            break;   // 排到文末：这就是最后一页
        }
        if (next <= position) {
            // 没有前进：再循环下去就是死循环，宁可少一页也要停
            break;
        }
        position = static_cast<int32_t>(next);
    }
    return starts;
}

}  // namespace npcc::core
