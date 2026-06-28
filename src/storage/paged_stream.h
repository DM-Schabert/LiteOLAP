#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace liteolap {

/**
 * Append-only byte stream layered over a chain of 4 KiB pages. Bytes are
 * written contiguously and spill onto freshly-allocated pages as the
 * current page fills. The chain is singly linked via `PageHeader::next_page_id`,
 * and each page records its used `payload_bytes`.
 *
 * This is the substrate for column storage: a column is one paged stream
 * holding a sequence of self-describing chunks.
 */
class PagedOutputStream {
   public:
    explicit PagedOutputStream(BufferPool& bp);

    /// Appends `n` bytes, allocating pages as needed.
    void Write(const std::byte* data, std::size_t n);

    template <typename T>
    void WritePod(const T& v) {
        Write(reinterpret_cast<const std::byte*>(&v), sizeof(T));
    }

    /// Finalizes the current page and returns the id of the first page in
    /// the chain (the stream's handle). Must be called exactly once.
    PageId Finish();

    /// Total bytes written so far.
    std::uint64_t BytesWritten() const noexcept { return bytes_written_; }

   private:
    void StartNewPage();
    void FlushCurrent();

    BufferPool& bp_;
    PageId first_page_id_{kInvalidPageId};
    PageId current_page_id_{kInvalidPageId};
    Page* current_page_{nullptr};
    std::size_t offset_{0};  ///< bytes used in current page payload
    std::uint64_t bytes_written_{0};
    bool finished_{false};
};

/**
 * Sequential reader over a paged byte stream produced by PagedOutputStream.
 * Supports forward Read and Skip (the latter powers zone-map chunk skipping
 * without decoding).
 */
class PagedInputStream {
   public:
    PagedInputStream(BufferPool& bp, PageId first_page_id);

    /// Reads exactly `n` bytes into `out`. Throws on underflow.
    void Read(std::byte* out, std::size_t n);

    template <typename T>
    T ReadPod() {
        T v{};
        Read(reinterpret_cast<std::byte*>(&v), sizeof(T));
        return v;
    }

    /// Advances `n` bytes without copying (still walks pages).
    void Skip(std::size_t n);

    /// True once all written bytes have been consumed.
    bool AtEnd() const noexcept { return at_end_; }

   private:
    void Advance(std::byte* out, std::size_t n, bool copy);
    void LoadPage(PageId pid);

    BufferPool& bp_;
    PageId current_page_id_{kInvalidPageId};
    std::size_t offset_{0};
    std::size_t page_payload_{0};
    PageId next_page_id_{kInvalidPageId};
    bool at_end_{false};
    std::vector<std::byte> page_buf_;  ///< snapshot of current page payload
};

}  // namespace liteolap
