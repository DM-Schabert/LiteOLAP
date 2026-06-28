#pragma once

#include <cstdint>

#include "compression/encoding.h"

namespace liteolap {

/// Logical rows per column chunk. A chunk is the unit of encoding and of
/// zone-map pruning. All columns of a table use the same chunk boundaries
/// so that chunk *i* of every column covers the same row range — that
/// alignment is what lets a multi-column scan stay row-consistent.
inline constexpr std::uint32_t kChunkRows = 2048;

/// Per-chunk metadata, serialized ahead of each chunk's encoded payload in
/// the column's paged byte stream. Fixed 32-byte on-disk layout.
struct ChunkMeta {
    std::uint32_t num_rows{0};
    Encoding encoding{Encoding::kPlain};
    std::uint8_t has_zone_map{0};
    std::uint16_t reserved{0};
    std::uint32_t null_count{0};
    std::uint32_t byte_length{0};  ///< encoded payload bytes following this header
    std::int64_t min_value{0};
    std::int64_t max_value{0};
};

}  // namespace liteolap
