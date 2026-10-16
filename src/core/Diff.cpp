#include "core/Diff.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "core/Localization.h"

namespace npcc::core {
namespace {

// UTF-8 序列长度（与预览面板那套同一个规则）
std::size_t Utf8Length(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

std::uint32_t DecodeUtf8(std::string_view text, std::size_t index, std::size_t* length) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    const std::size_t size = Utf8Length(lead);
    *length = size;
    if (size == 1) {
        return lead;
    }
    std::uint32_t value = lead & ((1u << (8 - size - 1)) - 1u);
    for (std::size_t offset = 1; offset < size; ++offset) {
        if (index + offset >= text.size()) {
            return lead;   // 截断的序列：按一个字节算，不越界
        }
        value = (value << 6) | (static_cast<unsigned char>(text[index + offset]) & 0x3Fu);
    }
    return value;
}

// 显示宽度：东亚宽字符（含全角标点）算两格，其余算一格。
// 这里用的是一张**粗粒度的区间表**——够把中文、日文、韩文与常见全角符号算对，
// 不追求 Unicode East Asian Width 的完整覆盖（那要一张大表，也不影响对齐的可用性）
bool IsWide(std::uint32_t code) {
    return (code >= 0x1100 && code <= 0x115F)      // 谚文字母
        || (code >= 0x2E80 && code <= 0x303E)      // 中日韩部首、标点
        || (code >= 0x3041 && code <= 0x33FF)      // 假名、注音、兼容字符
        || (code >= 0x3400 && code <= 0x4DBF)      // 扩展 A
        || (code >= 0x4E00 && code <= 0x9FFF)      // 基本汉字
        || (code >= 0xA000 && code <= 0xA4CF)      // 彝文
        || (code >= 0xAC00 && code <= 0xD7A3)      // 谚文音节
        || (code >= 0xF900 && code <= 0xFAFF)      // 兼容汉字
        || (code >= 0xFE30 && code <= 0xFE6F)      // 兼容形式
        || (code >= 0xFF00 && code <= 0xFF60)      // 全角形式
        || (code >= 0xFFE0 && code <= 0xFFE6)
        || (code >= 0x20000 && code <= 0x3FFFD);   // 扩展 B 以上
}

// 把一段文本补到 limit 个显示宽度；太长就截断并留一个记号
//（记号也要占位置，所以要先把它的宽度留出来再截）
std::string PadTo(std::string_view text, int limit, std::string_view marker) {
    const int markerWidth = DisplayWidth(marker);
    int width = DisplayWidth(text);
    std::string out;
    if (width <= limit) {
        out.assign(text);
    } else {
        const int budget = std::max(0, limit - markerWidth);
        int keep = 0;
        std::size_t cut = 0;
        while (cut < text.size()) {
            std::size_t length = 1;
            const std::uint32_t code = DecodeUtf8(text, cut, &length);
            const int cells = IsWide(code) ? 2 : 1;
            if (keep + cells > budget) {
                break;
            }
            keep += cells;
            cut += length;
        }
        out.assign(text.substr(0, cut));
        out += marker;
        width = keep + markerWidth;
    }
    out.append(static_cast<std::size_t>(std::max(0, limit - width)), ' ');
    return out;
}

}  // namespace

int DisplayWidth(std::string_view utf8) {
    int width = 0;
    std::size_t index = 0;
    while (index < utf8.size()) {
        std::size_t length = 1;
        const std::uint32_t code = DecodeUtf8(utf8, index, &length);
        width += IsWide(code) ? 2 : 1;
        index += length;
    }
    return width;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

std::vector<DiffRow> ComputeLineDiff(const std::vector<std::string>& left,
                                     const std::vector<std::string>& right,
                                     DiffStats* stats) {
    DiffStats local;
    local.leftLines = static_cast<int>(left.size());
    local.rightLines = static_cast<int>(right.size());

    std::vector<DiffRow> rows;
    const auto pushEqual = [&rows](int leftNumber, int rightNumber, const std::string& text) {
        DiffRow row;
        row.kind = DiffKind::Equal;
        row.leftNumber = leftNumber;
        row.rightNumber = rightNumber;
        row.leftText = text;
        row.rightText = text;
        rows.push_back(std::move(row));
    };

    // 1) 相同的前缀与后缀先切掉
    std::size_t head = 0;
    while (head < left.size() && head < right.size() && left[head] == right[head]) {
        pushEqual(static_cast<int>(head) + 1, static_cast<int>(head) + 1, left[head]);
        ++head;
    }
    std::size_t tail = 0;
    while (tail < left.size() - head && tail < right.size() - head &&
           left[left.size() - 1 - tail] == right[right.size() - 1 - tail]) {
        ++tail;
    }

    const std::size_t leftMiddle = left.size() - head - tail;
    const std::size_t rightMiddle = right.size() - head - tail;

    // 2) 中间那段：能算就精确算，太大就整块替换
    std::vector<DiffRow> middle;
    if (leftMiddle > 0 || rightMiddle > 0) {
        if (leftMiddle * rightMiddle > kMaxDiffCells) {
            local.coarse = true;
            for (std::size_t index = 0; index < std::max(leftMiddle, rightMiddle); ++index) {
                DiffRow row;
                const bool hasLeft = index < leftMiddle;
                const bool hasRight = index < rightMiddle;
                if (hasLeft && hasRight) {
                    row.kind = DiffKind::Changed;
                } else if (hasLeft) {
                    row.kind = DiffKind::Deleted;
                } else {
                    row.kind = DiffKind::Inserted;
                }
                row.leftNumber = hasLeft ? static_cast<int>(head + index + 1) : 0;
                row.leftText = hasLeft ? left[head + index] : std::string();
                row.rightNumber = hasRight ? static_cast<int>(head + index + 1) : 0;
                row.rightText = hasRight ? right[head + index] : std::string();
                middle.push_back(std::move(row));
            }
        } else {
            // LCS：dp[i][j] = 左边从 i 起、右边从 j 起的最长公共子序列长度
            const std::size_t rowsCount = leftMiddle + 1;
            const std::size_t columnsCount = rightMiddle + 1;
            std::vector<std::uint32_t> dp(rowsCount * columnsCount, 0);
            const auto at = [&dp, columnsCount](std::size_t i, std::size_t j) -> std::uint32_t& {
                return dp[i * columnsCount + j];
            };
            for (std::size_t i = leftMiddle; i-- > 0;) {
                for (std::size_t j = rightMiddle; j-- > 0;) {
                    at(i, j) = left[head + i] == right[head + j]
                                   ? at(i + 1, j + 1) + 1
                                   : std::max(at(i + 1, j), at(i, j + 1));
                }
            }
            std::size_t i = 0;
            std::size_t j = 0;
            // 中间这`段的每一行都先收进 middle（**包括相同的行**），最后统一按顺序
            // 并到 rows 里：边算边往 rows 里塞的话，相同的行会挤到改动行前面去
            const auto emitEqual = [&middle](int leftNumber, int rightNumber,
                                             const std::string& text) {
                DiffRow row;
                row.kind = DiffKind::Equal;
                row.leftNumber = leftNumber;
                row.rightNumber = rightNumber;
                row.leftText = text;
                row.rightText = text;
                middle.push_back(std::move(row));
            };
            while (i < leftMiddle && j < rightMiddle) {
                if (left[head + i] == right[head + j]) {
                    emitEqual(static_cast<int>(head + i) + 1, static_cast<int>(head + j) + 1,
                              left[head + i]);
                    ++i;
                    ++j;
                    continue;
                }
                if (at(i + 1, j) >= at(i, j + 1)) {
                    middle.push_back(DiffRow{DiffKind::Deleted, static_cast<int>(head + i) + 1, 0,
                                             left[head + i], std::string()});
                    ++i;
                } else {
                    middle.push_back(DiffRow{DiffKind::Inserted, 0, static_cast<int>(head + j) + 1,
                                             std::string(), right[head + j]});
                    ++j;
                }
            }
            while (i < leftMiddle) {
                middle.push_back(DiffRow{DiffKind::Deleted, static_cast<int>(head + i) + 1, 0,
                                         left[head + i], std::string()});
                ++i;
            }
            while (j < rightMiddle) {
                middle.push_back(DiffRow{DiffKind::Inserted, 0, static_cast<int>(head + j) + 1,
                                         std::string(), right[head + j]});
                ++j;
            }
        }
    }

    // 3) 相邻的"删一片 + 插一片"配成一对对的改动行：读起来是"这两行不一样"
    std::size_t cursor = 0;
    while (cursor < middle.size()) {
        if (middle[cursor].kind != DiffKind::Deleted) {
            rows.push_back(std::move(middle[cursor]));
            ++cursor;
            continue;
        }
        std::size_t deletedEnd = cursor;
        while (deletedEnd < middle.size() && middle[deletedEnd].kind == DiffKind::Deleted) {
            ++deletedEnd;
        }
        std::size_t insertedEnd = deletedEnd;
        while (insertedEnd < middle.size() && middle[insertedEnd].kind == DiffKind::Inserted) {
            ++insertedEnd;
        }
        const std::size_t deletedCount = deletedEnd - cursor;
        const std::size_t insertedCount = insertedEnd - deletedEnd;
        const std::size_t paired = std::min(deletedCount, insertedCount);
        for (std::size_t index = 0; index < paired; ++index) {
            DiffRow row;
            row.kind = DiffKind::Changed;
            row.leftNumber = middle[cursor + index].leftNumber;
            row.leftText = middle[cursor + index].leftText;
            row.rightNumber = middle[deletedEnd + index].rightNumber;
            row.rightText = middle[deletedEnd + index].rightText;
            rows.push_back(std::move(row));
        }
        for (std::size_t index = paired; index < deletedCount; ++index) {
            rows.push_back(std::move(middle[cursor + index]));
        }
        for (std::size_t index = paired; index < insertedCount; ++index) {
            rows.push_back(std::move(middle[deletedEnd + index]));
        }
        cursor = insertedEnd;
    }

    // 4) 后缀
    for (std::size_t index = 0; index < tail; ++index) {
        const std::size_t slot = left.size() - tail + index;
        const std::size_t rightSlot = right.size() - tail + index;
        pushEqual(static_cast<int>(slot) + 1, static_cast<int>(rightSlot) + 1, left[slot]);
    }

    // 5) 统计（"处数"按连续的差异段算）
    bool inDifference = false;
    for (const DiffRow& row : rows) {
        switch (row.kind) {
            case DiffKind::Equal:
                ++local.equal;
                inDifference = false;
                break;
            case DiffKind::Changed:
                ++local.changed;
                if (!inDifference) {
                    ++local.hunks;
                    inDifference = true;
                }
                break;
            case DiffKind::Deleted:
                ++local.deleted;
                if (!inDifference) {
                    ++local.hunks;
                    inDifference = true;
                }
                break;
            case DiffKind::Inserted:
                ++local.inserted;
                if (!inDifference) {
                    ++local.hunks;
                    inDifference = true;
                }
                break;
        }
    }
    if (stats != nullptr) {
        *stats = local;
    }
    return rows;
}

std::string RenderComparison(const std::vector<DiffRow>& rows, const DiffStats& stats,
                             const CompareRenderOptions& options) {
    const int width = std::max(options.columnWidth, 20);
    std::string out;

    const auto appendLine = [&out](const std::string& line) {
        out += line;
        out.push_back('\n');
    };

    // 头两行：谁跟谁比、比出了什么
    // 表头是"我们的字"，走语言包；下面的差异行是用户的正文，一个字都不动
    const auto utf8 = [](const std::wstring& wide) {
        if (wide.empty()) {
            return std::string();
        }
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                               static_cast<int>(wide.size()), nullptr, 0,
                                               nullptr, nullptr);
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                              need, nullptr, nullptr);
        return out;
    };
    const auto toWide = [](const std::string& narrow) {
        if (narrow.empty()) {
            return std::wstring();
        }
        const int need = ::MultiByteToWideChar(CP_UTF8, 0, narrow.data(),
                                               static_cast<int>(narrow.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(need), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()),
                              out.data(), need);
        return out;
    };
    appendLine(utf8(Tr(L"compare.title", L"比较：{0}  ↔  {1}",
                       {toWide(options.leftName.empty() ? std::string("(未命名)")
                                                        : options.leftName),
                        toWide(options.rightName.empty() ? std::string("(未命名)")
                                                         : options.rightName)})));
    appendLine(utf8(Tr(L"compare.stats",
                       L"左 {0} 行，右 {1} 行；不同 {2} 处（改动 {3}、删除 {4}、新增 {5}）",
                       {std::to_wstring(stats.leftLines), std::to_wstring(stats.rightLines),
                        std::to_wstring(stats.hunks), std::to_wstring(stats.changed),
                        std::to_wstring(stats.deleted), std::to_wstring(stats.inserted)})));
    if (stats.coarse) {
        appendLine(utf8(Tr(L"compare.coarse",
                           L"（内容太大，中间那段按「整块替换」粗略比较：行号仍然准确，"
                           L"配对的粗细不如精确比较）")));
    }
    appendLine(std::string(static_cast<std::size_t>(std::min(width * 2 + 12, 160)), '-'));

    // 行那一列宽 5（够 99999 行；再多也读得出来），其余留给正文
    constexpr int kNumberWidth = 5;
    const std::string marker = " \xE2\x80\xA6";   // " …"：被截断的记号
    for (const DiffRow& row : rows) {
        char mark = ' ';
        switch (row.kind) {
            case DiffKind::Changed:  mark = '~'; break;
            case DiffKind::Deleted:  mark = '-'; break;
            case DiffKind::Inserted: mark = '+'; break;
            default:                 mark = ' '; break;
        }
        std::string line;
        line.push_back(mark);
        if (row.leftNumber > 0) {
            char number[16] = {};
            std::snprintf(number, sizeof(number), "%5d ", row.leftNumber);
            line += number;
        } else {
            line.append(static_cast<std::size_t>(kNumberWidth) + 1, ' ');
        }
        line += PadTo(row.leftText, width, marker);
        line += " \xE2\x94\x82 ";   // " │ " 两栏之间的分隔
        if (row.rightNumber > 0) {
            char number[16] = {};
            std::snprintf(number, sizeof(number), "%5d ", row.rightNumber);
            line += number;
        } else {
            line.append(static_cast<std::size_t>(kNumberWidth) + 1, ' ');
        }
        line += PadTo(row.rightText, width, marker);
        appendLine(line);
    }
    return out;
}

}  // namespace npcc::core
