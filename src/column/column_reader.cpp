#include "column/column_reader.h"

namespace liteolap {

ColumnReader::ColumnReader(BufferPool& bp, PageId first_page, ColumnType type)
    : in_(bp, first_page), type_(type) {}

bool ColumnReader::NextChunk() {
    if (!payload_consumed_) {
        SkipPayload();
    }
    if (in_.AtEnd()) return false;

    meta_.num_rows = in_.ReadPod<std::uint32_t>();
    meta_.encoding = static_cast<Encoding>(in_.ReadPod<std::uint8_t>());
    meta_.has_zone_map = in_.ReadPod<std::uint8_t>();
    meta_.reserved = in_.ReadPod<std::uint16_t>();
    meta_.null_count = in_.ReadPod<std::uint32_t>();
    meta_.byte_length = in_.ReadPod<std::uint32_t>();
    meta_.min_value = in_.ReadPod<std::int64_t>();
    meta_.max_value = in_.ReadPod<std::int64_t>();
    payload_consumed_ = false;
    return true;
}

void ColumnReader::Decode(Vector& out) {
    scratch_.resize(meta_.byte_length);
    in_.Read(scratch_.data(), scratch_.size());
    DecodeChunk(type_, meta_.encoding, meta_.num_rows, scratch_.data(), scratch_.size(), out);
    payload_consumed_ = true;
}

void ColumnReader::SkipPayload() {
    if (payload_consumed_) return;
    in_.Skip(meta_.byte_length);
    payload_consumed_ = true;
}

}  // namespace liteolap
