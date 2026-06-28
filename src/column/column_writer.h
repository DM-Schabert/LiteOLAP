#pragma once

#include <vector>

#include "column/column_chunk.h"
#include "common/value.h"
#include "storage/buffer_pool.h"
#include "storage/paged_stream.h"

namespace liteolap {

/**
 * Builds one column's on-disk representation. Values are buffered and flushed
 * as encoded chunks of `kChunkRows` rows each into a paged byte stream. The
 * best encoding is chosen per chunk by `EncodeChunk`.
 */
class ColumnWriter {
   public:
    ColumnWriter(BufferPool& bp, ColumnType type);

    void Append(const Value& v);

    /// Flushes the final partial chunk and returns the first page id of the
    /// column's stream (its handle, stored in the catalog).
    PageId Finish();

   private:
    void FlushChunk();

    ColumnType type_;
    PagedOutputStream out_;
    std::vector<Value> buffer_;
    bool finished_{false};
};

}  // namespace liteolap
