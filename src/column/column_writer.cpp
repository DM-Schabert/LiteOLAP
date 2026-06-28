#include "column/column_writer.h"

#include <stdexcept>

namespace liteolap {

ColumnWriter::ColumnWriter(BufferPool& bp, ColumnType type) : type_(type), out_(bp) {
    buffer_.reserve(kChunkRows);
}

void ColumnWriter::Append(const Value& v) {
    if (finished_) throw std::logic_error("ColumnWriter::Append after Finish");
    buffer_.push_back(v);
    if (buffer_.size() >= kChunkRows) {
        FlushChunk();
    }
}

void ColumnWriter::FlushChunk() {
    if (buffer_.empty()) return;
    EncodedChunk c = EncodeChunk(type_, buffer_);

    ChunkMeta meta;
    meta.num_rows = c.num_rows;
    meta.encoding = c.encoding;
    meta.has_zone_map = c.has_zone_map ? 1 : 0;
    meta.null_count = c.null_count;
    meta.byte_length = static_cast<std::uint32_t>(c.payload.size());
    meta.min_value = c.min_value;
    meta.max_value = c.max_value;

    // Serialize the 32-byte header field-by-field (stable layout).
    out_.WritePod(meta.num_rows);
    out_.WritePod(static_cast<std::uint8_t>(meta.encoding));
    out_.WritePod(meta.has_zone_map);
    out_.WritePod(meta.reserved);
    out_.WritePod(meta.null_count);
    out_.WritePod(meta.byte_length);
    out_.WritePod(meta.min_value);
    out_.WritePod(meta.max_value);
    out_.Write(c.payload.data(), c.payload.size());

    buffer_.clear();
}

PageId ColumnWriter::Finish() {
    if (finished_) throw std::logic_error("ColumnWriter::Finish called twice");
    FlushChunk();
    finished_ = true;
    return out_.Finish();
}

}  // namespace liteolap
