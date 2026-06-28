#pragma once

#include <vector>

#include "column/column_chunk.h"
#include "storage/buffer_pool.h"
#include "storage/paged_stream.h"
#include "vector/vector.h"

namespace liteolap {

/**
 * Sequentially reads a column's chunks from its paged stream. Usage:
 *
 *   while (reader.NextChunk()) {
 *       if (CanSkip(reader.meta())) reader.SkipPayload();
 *       else reader.Decode(out_vector);
 *   }
 *
 * Reading the header before the payload is what enables zone-map pruning:
 * the caller inspects `meta().min_value/max_value` and may skip the
 * (potentially multi-page) payload without decoding it.
 */
class ColumnReader {
   public:
    ColumnReader(BufferPool& bp, PageId first_page, ColumnType type);

    /// Advances to the next chunk's header. Returns false at end of stream.
    /// If the previous chunk's payload was neither decoded nor skipped, it
    /// is skipped automatically.
    bool NextChunk();

    const ChunkMeta& meta() const noexcept { return meta_; }

    /// Decodes the current chunk's payload, appending rows to `out`.
    void Decode(Vector& out);

    /// Skips the current chunk's payload without decoding.
    void SkipPayload();

   private:
    PagedInputStream in_;
    ColumnType type_;
    ChunkMeta meta_;
    bool payload_consumed_{true};
    std::vector<std::byte> scratch_;
};

}  // namespace liteolap
