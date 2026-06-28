#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/value.h"

namespace liteolap {

/// Number of rows processed per vectorized batch. Matches DuckDB's default;
/// big enough to amortize per-batch overhead, small enough to stay cache-warm.
inline constexpr std::size_t kVectorSize = 2048;

/**
 * A typed, column-oriented batch of up to a few thousand values plus a
 * validity (NULL) mask. This is the unit of data that flows between
 * operators in the vectorized engine.
 *
 * Fixed-width types (INT/BIGINT/FLOAT) are stored as a packed byte buffer
 * reinterpreted via `GetData<T>()`. VARCHAR uses an Arrow-style layout:
 * an offsets array of `size()+1` entries indexing into a contiguous byte
 * buffer. Validity is one byte per row (1 = valid, 0 = NULL) — simpler and
 * faster to branch on than a bitmap, at a small memory cost.
 */
class Vector {
   public:
    explicit Vector(ColumnType type);

    ColumnType type() const noexcept { return type_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    /// Resets to zero rows, keeping capacity.
    void Clear();

    /// Raw typed access to fixed-width data. Undefined for VARCHAR.
    template <typename T>
    T* GetData() {
        return reinterpret_cast<T*>(data_.data());
    }
    template <typename T>
    const T* GetData() const {
        return reinterpret_cast<const T*>(data_.data());
    }

    bool IsValid(std::size_t i) const { return validity_[i] != 0; }
    void SetValidity(std::size_t i, bool valid) {
        validity_[i] = valid ? std::uint8_t{1} : std::uint8_t{0};
    }
    const std::vector<std::uint8_t>& validity() const noexcept { return validity_; }

    std::string_view GetStringView(std::size_t i) const {
        const auto begin = offsets_[i];
        const auto end = offsets_[i + 1];
        return std::string_view(reinterpret_cast<const char*>(strbytes_.data()) + begin,
                                end - begin);
    }

    // --- Typed append (fast paths used by decoders and operators) -----------
    void AppendInt32(std::int32_t v);
    void AppendInt64(std::int64_t v);
    void AppendDouble(double v);
    void AppendString(std::string_view v);
    void AppendNull();

    // --- Generic value access ----------------------------------------------
    void AppendValue(const Value& v);
    Value GetValue(std::size_t i) const;

    /// Copies row `src_idx` of `src` onto the end of this vector. Types must
    /// match. Used by operators that gather selected rows.
    void AppendFrom(const Vector& src, std::size_t src_idx);

    /// Resizes to exactly `n` rows of fixed-width type, validity all-true.
    /// Intended for in-place fills by scan/filter on fixed types.
    void ResizeFixed(std::size_t n);

   private:
    void EnsureFixedSlot();

    ColumnType type_;
    std::size_t size_{0};
    std::vector<std::byte> data_;          ///< fixed-width packed values
    std::vector<std::uint8_t> validity_;   ///< 1 = valid, 0 = NULL
    std::vector<std::uint32_t> offsets_;   ///< VARCHAR offsets (size_+1 entries)
    std::vector<std::byte> strbytes_;      ///< VARCHAR payload
};

}  // namespace liteolap
