#include "edit/ColumnEdit.h"

#include "edit/EditView.h"
#include "search/TextBytes.h"

namespace npcc::edit {

int ApplyColumnInsert(EditView& view, const std::vector<std::wstring>& texts, bool utfMode,
                      std::uint32_t codePage) {
    int32_t firstLine = 0;
    int32_t lastLine = 0;
    view.ColumnRange(firstLine, lastLine);
    const int32_t lineCount = lastLine - firstLine + 1;
    if (lineCount <= 0 || static_cast<int32_t>(texts.size()) < lineCount) {
        return 0;
    }

    // 必须先把所有行的插入位置取出来。改一次文档就会清掉选区，
    // 之后再问"第 N 行的选区从哪开始"就只能得到 -1（踩过）。
    std::vector<int32_t> positions(static_cast<std::size_t>(lineCount), -1);
    for (int32_t line = firstLine; line <= lastLine; ++line) {
        positions[static_cast<std::size_t>(line - firstLine)] = view.LineSelectionStart(line);
    }

    view.BeginUndoAction();
    int applied = 0;
    // 倒着插：先改前面的行会让后面行的位置整体后移
    for (int32_t line = lastLine; line >= firstLine; --line) {
        const int32_t position = positions[static_cast<std::size_t>(line - firstLine)];
        if (position < 0) {
            continue;
        }
        std::string bytes;
        if (!search::ConvertToDocBytes(texts[static_cast<std::size_t>(line - firstLine)],
                                       utfMode, codePage, bytes)) {
            continue;
        }
        if (bytes.empty()) {
            continue;
        }
        view.InsertTextAt(position, bytes);
        ++applied;
    }
    view.EndUndoAction();
    return applied;
}

int DeleteColumnSelection(EditView& view) {
    int32_t firstLine = 0;
    int32_t lastLine = 0;
    view.ColumnRange(firstLine, lastLine);

    // 同理：先取全部区间再动手
    struct Span {
        int32_t start = -1;
        int32_t end = -1;
    };
    std::vector<Span> spans(static_cast<std::size_t>(lastLine - firstLine + 1));
    for (int32_t line = firstLine; line <= lastLine; ++line) {
        Span& span = spans[static_cast<std::size_t>(line - firstLine)];
        span.start = view.LineSelectionStart(line);
        span.end = view.LineSelectionEnd(line);
    }

    view.BeginUndoAction();
    int removed = 0;
    for (int32_t line = lastLine; line >= firstLine; --line) {
        const Span& span = spans[static_cast<std::size_t>(line - firstLine)];
        if (span.start < 0 || span.end <= span.start) {
            continue;
        }
        view.DeleteRangeAt(span.start, span.end);
        ++removed;
    }
    view.EndUndoAction();
    return removed;
}

}  // namespace npcc::edit
