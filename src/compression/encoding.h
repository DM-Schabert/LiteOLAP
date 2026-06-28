#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/value.h"
#include "vector/vector.h"

namespace liteolap {

/// On-disk encoding scheme of a column chunk. Stable byte values.
enum class Encoding : std::uint8_t {
    kPlain = 0,       ///< raw values
    kRle = 1,         ///< run-length encoded (fixed-width types)
    kDictionary = 2,  ///< dictionary + bit-packed indices
    kBitpack = 3,     ///< frame-of-reference bit-packing (integers)
};

const char* EncodingName(Encoding e);

/// Result of encoding one logical chunk of values.
struct EncodedChunk {
    Encoding encoding{Encoding::kPlain};
    std::uint32_t num_rows{0};
    std::uint32_t null_count{0};
    bool has_zone_map{false};
    std::int64_t min_value{0};  ///< zone map (INT/BIGINT only)
    std::int64_t max_value{0};
    std::vector<std::byte> payload;
};

/// Encodes `values` (length n, possibly containing Null) of the given type,
/// automatically choosing the encoding that yields the smallest payload.
EncodedChunk EncodeChunk(ColumnType type, const std::vector<Value>& values);

/// Encodes with a specific scheme (used by tests and the auto-chooser).
/// Returns std::nullopt if the scheme is not applicable to the type.
EncodedChunk EncodeChunkWith(ColumnType type, const std::vector<Value>& values, Encoding enc);

/// Decodes a payload back into `out` (appending `num_rows` rows).
void DecodeChunk(ColumnType type, Encoding enc, std::uint32_t num_rows,
                 const std::byte* payload, std::size_t len, Vector& out);

// --- Low-level bit-packing helpers (exposed for unit testing) --------------

/// Packs `n` values of `bit_width` bits each, LSB-first, appending to `out`.
/// `bit_width == 0` writes nothing (all values are implicitly zero).
void PackBits(const std::uint64_t* vals, std::size_t n, unsigned bit_width,
              std::vector<std::byte>& out);

/// Inverse of PackBits.
void UnpackBits(const std::byte* in, std::size_t n, unsigned bit_width, std::uint64_t* out);

/// Minimum bits needed to represent values in [0, max_val] (1 for max_val==0).
unsigned BitsRequired(std::uint64_t max_val);

}  // namespace liteolap
