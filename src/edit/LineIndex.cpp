#include "edit/LineIndex.h"

#include <algorithm>
#include <cstring>

namespace npcc::edit {
namespace {

// 在一段字节里找最近的行尾起点。返回 false = 这段里没有行尾。
// \r\n 要一起吃掉；\r 后面不是 \n 就是孤立 \r（老 Mac 行尾）。
// limit 传块的终点：块尾正好落在 \r 上时**先不定**（"defer"），因为
// \r\n 与孤立 \r 要靠下一个字节分辨，而下一个字节还在下一块里。
struct Terminator {
    std::uint64_t start = 0;     // 行尾第一个字节
    std::uint64_t next = 0;      // 下一行起点（= 行尾之后）
    bool          defer = false; // 块尾的 \r：留到下一块
    bool          found = false;
};

Terminator FindTerminator(const char* data, std::uint64_t size, std::uint64_t from,
                          std::uint64_t limit) {
    Terminator result;
    if (from >= limit) {
        return result;
    }
    const char* base = data + from;
    const std::size_t span = static_cast<std::size_t>(limit - from);
    const char* lf = static_cast<const char*>(std::memchr(base, '\n', span));
    const char* cr = static_cast<const char*>(std::memchr(base, '\r', span));
    if (lf == nullptr && cr == nullptr) {
        return result;
    }
    result.found = true;
    if (cr != nullptr && (lf == nullptr || cr < lf)) {
        const std::uint64_t at = static_cast<std::uint64_t>(cr - data);
        if (at + 1 >= size) {
            // 文件最后一个字节是孤立 \r：它就是一个行尾
            result.start = at;
            result.next = at + 1;
            return result;
        }
        if (at + 1 >= limit) {
            result.defer = true;
            result.start = at;
            return result;
        }
        if (data[at + 1] == '\n') {
            result.start = at;
            result.next = at + 2;
        } else {
            result.start = at;
            result.next = at + 1;
        }
        return result;
    }
    const std::uint64_t at = static_cast<std::uint64_t>(lf - data);
    result.start = at;
    result.next = at + 1;
    return result;
}

}  // namespace

void LineIndex::Attach(const char* data, std::uint64_t size) {
    Attach(data, size, Options{});
}

void LineIndex::Attach(const char* data, std::uint64_t size, const Options& options) {
    Reset();
    data_ = data;
    size_ = data != nullptr ? size : 0;
    options_ = options;
    if (options_.anchorInterval == 0) {
        options_.anchorInterval = 4096;
    }
    if (options_.chunkBytes == 0) {
        options_.chunkBytes = 8ull * 1024 * 1024;
    }
    // 第 0 行从偏移 0 开始，空文件也算一行（与编辑器的口径一致）
    starts_.push_back(0);
}

void LineIndex::Reset() {
    data_ = nullptr;
    size_ = 0;
    options_ = Options{};
    sparse_ = false;
    scanPos_ = 0;
    lineCount_ = 1;
    starts_.clear();
    anchors_.clear();
}

double LineIndex::Progress() const noexcept {
    if (size_ == 0) {
        return 1.0;
    }
    const double done = static_cast<double>(scanPos_) / static_cast<double>(size_);
    return done >= 1.0 ? 1.0 : done;
}

bool LineIndex::ScanNextChunk() {
    if (scanPos_ >= size_) {
        return true;
    }
    std::uint64_t end = std::min<std::uint64_t>(scanPos_ + options_.chunkBytes, size_);
    // 上一块把 \r 留在了块尾：这一块至少要含入它后面的一个字节，否则
    // 1 字节一块的极端配置会在那个 \r 上原地打转（\r\n 与孤立 \r 分不出来）
    if (scanPos_ < size_ && data_[scanPos_] == '\r' && scanPos_ + 1 < size_) {
        end = std::min(std::max(end, scanPos_ + 2), size_);
    }
    ScanRange(scanPos_, end);
    return scanPos_ >= size_;
}

void LineIndex::ScanRange(std::uint64_t start, std::uint64_t end) {
    std::uint64_t pos = start;
    while (pos < end) {
        const Terminator term = FindTerminator(data_, size_, pos, end);
        if (term.defer) {
            // 把那个 \r 留到下一块：scanPos_ 停在它身上，Complete() 仍为假
            pos = term.start;
            break;
        }
        if (!term.found) {
            pos = end;
            break;
        }
        ++lineCount_;
        if (!sparse_) {
            starts_.push_back(term.next);
            if (starts_.size() > options_.sparseAboveLines) {
                ConvertToSparse();
            }
        } else if ((lineCount_ - 1) % options_.anchorInterval == 0) {
            anchors_.push_back(Anchor{lineCount_ - 1, term.next});
        }
        pos = term.next;
    }
    if (pos > scanPos_) {
        scanPos_ = pos;
    }
}

void LineIndex::ConvertToSparse() {
    // 稠密表里一行一个偏移，转稀疏就变成"每 interval 行留一个锚点"。
    // 这一步之后 8 字节/行 变成 16 字节/4096 行，1 亿行的索引从 800 MB 落到 200 KB。
    anchors_.clear();
    anchors_.push_back(Anchor{0, 0});
    for (std::uint64_t line = options_.anchorInterval; line < lineCount_;
         line += options_.anchorInterval) {
        anchors_.push_back(Anchor{line, starts_[static_cast<std::size_t>(line)]});
    }
    sparse_ = true;
    starts_.clear();
    starts_.shrink_to_fit();
}

std::uint64_t LineIndex::ScanForwardFrom(std::uint64_t anchorOffset,
                                         std::uint64_t skipLines) const {
    std::uint64_t pos = anchorOffset;
    for (std::uint64_t left = skipLines; left > 0 && pos < size_; --left) {
        const Terminator term = FindTerminator(data_, size_, pos, size_);
        if (!term.found || term.defer) {
            // 扫到头了（defer 只可能发生在块扫描里，这里 limit = size_，不会 defer）
            pos = size_;
            break;
        }
        pos = term.next;
    }
    return pos;
}

bool LineIndex::LineStart(std::uint64_t line, std::uint64_t* offset) const {
    if (offset == nullptr || line >= lineCount_) {
        return false;
    }
    if (!sparse_) {
        *offset = starts_[static_cast<std::size_t>(line)];
        return true;
    }
    // 找不超过 line 的最后一个锚点，然后局部扫（最多 interval 行）
    std::size_t lo = 0;
    std::size_t hi = anchors_.size();
    while (lo + 1 < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (anchors_[mid].line <= line) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    *offset = ScanForwardFrom(anchors_[lo].offset, line - anchors_[lo].line);
    return true;
}

bool LineIndex::LineOfOffset(std::uint64_t offset, std::uint64_t* line) const {
    if (line == nullptr || offset >= size_) {
        return false;
    }
    if (scanPos_ < size_ && offset >= scanPos_) {
        return false;   // 这一段还没扫到，行号还定不下来
    }
    if (!sparse_) {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
        *line = static_cast<std::uint64_t>(it - starts_.begin()) - 1;
        return true;
    }
    // 锚点按偏移也单调：找最后一个 offset ≤ 目标偏移 的锚点，从那行开始数
    std::size_t lo = 0;
    std::size_t hi = anchors_.size();
    while (lo + 1 < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (anchors_[mid].offset <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    std::uint64_t pos = anchors_[lo].offset;
    std::uint64_t current = anchors_[lo].line;
    while (pos <= offset) {
        std::uint64_t contentEnd = 0;
        std::uint64_t nextStart = 0;
        SpanAt(pos, &contentEnd, &nextStart);
        if (offset < nextStart) {
            *line = current;
            return true;
        }
        if (nextStart <= pos) {
            break;   // 防御：不允许出现不前进的循环
        }
        pos = nextStart;
        ++current;
    }
    *line = current;
    return true;
}

void LineIndex::SpanAt(std::uint64_t offset, std::uint64_t* contentEnd,
                       std::uint64_t* nextStart) const {
    const Terminator term = FindTerminator(data_, size_, offset, size_);
    if (!term.found || term.defer) {
        *contentEnd = size_;
        *nextStart = size_;
        return;
    }
    *contentEnd = term.start;
    *nextStart = term.next;
}

bool LineIndex::LineRange(std::uint64_t line, std::uint64_t* contentStart,
                          std::uint64_t* contentEnd, std::uint64_t* nextStart) const {
    std::uint64_t start = 0;
    if (!LineStart(line, &start)) {
        return false;
    }
    SpanAt(start, contentEnd, nextStart);
    *contentStart = start;
    return true;
}

}  // namespace npcc::edit
