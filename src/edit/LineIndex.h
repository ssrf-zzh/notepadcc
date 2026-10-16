#pragma once

#include <cstdint>
#include <vector>

namespace npcc::edit {

// T3 超大文件的**延迟行索引**（设计说明书 6.1 T3 实现要点第 2 条）。
//
// 它只记"每一行从第几个字节开始"，不复制任何内容——数据是外部的只读映射，
// 本类只持有指针与偏移。分块扫描由调用方驱动（ScanNextChunk），一次扫一块，
// 界面在块与块之间能喘气画进度，不必为一个 GB 级文件卡住整个窗口。
//
// 两种形态：
//   · 稠密：一行一个 8 字节偏移，行数不超过 sparseAboveLines 时使用；
//   · 稀疏：每 anchorInterval 行记一个锚点，定位时从锚点局部扫描。
//     1 亿行在稠密形态下要 800 MB（设计里点名的那条），稀疏形态只要 200 KB。
//
// 行尾约定与编辑器一致：\n、\r\n、\r 都算换行；文件以换行结束时，末尾那个
// 空行也占一行（与 Scintilla / Notepad++ 的口径相同）。
class LineIndex {
public:
    struct Options {
        std::uint64_t chunkBytes       = 8ull * 1024 * 1024;   // 一次 ScanNextChunk 最多扫的字节
        std::uint64_t sparseAboveLines = 2ull * 1000 * 1000;   // 行数超过它就转稀疏
        std::uint64_t anchorInterval   = 4096;                 // 稀疏形态每多少行记一个锚点
    };

    LineIndex() = default;

    // 指向一段**只读**数据；调用方保证它的生命周期覆盖本对象，且扫描期间内容不变。
    void Attach(const char* data, std::uint64_t size);
    void Attach(const char* data, std::uint64_t size, const Options& options);
    void Reset();

    // 增量扫描一块。返回 true 表示整个索引已经建完；调用方循环
    //   while (!index.ScanNextChunk()) { 画一次进度 }
    // 即可在长文件上保持界面可动。
    bool ScanNextChunk();

    bool Complete() const noexcept { return scanPos_ >= size_; }
    std::uint64_t ScannedBytes() const noexcept { return scanPos_; }
    std::uint64_t TotalBytes() const noexcept { return size_; }
    double Progress() const noexcept;   // 0.0 ~ 1.0，扫完恒为 1
    bool Sparse() const noexcept { return sparse_; }

    // 行数。扫描中途是"已扫范围内的行数"，完成后才是总行数。
    std::uint64_t LineCount() const noexcept { return lineCount_; }

    // 定位（行号 0 起）。还没扫到的范围一律查不到，返回 false。
    bool LineStart(std::uint64_t line, std::uint64_t* offset) const;
    bool LineOfOffset(std::uint64_t offset, std::uint64_t* line) const;
    // 一行的三段：内容起点、内容终点（不含行尾）、下一行起点（含行尾）。
    bool LineRange(std::uint64_t line, std::uint64_t* contentStart, std::uint64_t* contentEnd,
                   std::uint64_t* nextStart) const;

    // 从行首往后找这一行的行尾（局部扫描，与形态无关）。渲染按行顺序推进时用它，
    // 每行只扫自己的字节，不回到锚点重扫。offset 必须 < Size()。
    void SpanAt(std::uint64_t offset, std::uint64_t* contentEnd, std::uint64_t* nextStart) const;

    const char* Data() const noexcept { return data_; }
    std::uint64_t Size() const noexcept { return size_; }

private:
    struct Anchor {
        std::uint64_t line = 0;     // 锚点所在行号（0 起）
        std::uint64_t offset = 0;   // 该行的起始字节
    };

    void ConvertToSparse();
    // 扫 [start, end)：把里面每个行尾记成"新的一行"。end 是块边界，块尾正好落在
    // \r 上时留到下一块再定（\r\n 与孤立 \r 要靠下一个字节分辨）。
    void ScanRange(std::uint64_t start, std::uint64_t end);
    // 从 anchorOffset 起向后跳过 skipLines 个行尾，返回目标行起点。
    std::uint64_t ScanForwardFrom(std::uint64_t anchorOffset, std::uint64_t skipLines) const;

    const char*   data_ = nullptr;
    std::uint64_t size_ = 0;
    Options       options_{};

    bool          sparse_    = false;
    std::uint64_t scanPos_   = 0;      // 已扫字节数
    std::uint64_t lineCount_ = 1;      // 已发现的行数（空文件也算 1 行）
    std::vector<std::uint64_t> starts_;   // 稠密：各行起点；starts_[0] = 0
    std::vector<Anchor>        anchors_;  // 稀疏：锚点，anchors_[0] = {0, 0}
};

}  // namespace npcc::edit
