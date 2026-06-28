#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/value.h"

namespace liteolap {

inline constexpr std::size_t kMaxColumnNameLen = 64;

struct Column {
    std::string name;
    ColumnType type{ColumnType::kInt};
    std::uint16_t varchar_len{0};
    std::uint16_t column_index{0};
};

/// Ordered set of columns describing a table's layout.
class Schema {
   public:
    Schema() = default;
    explicit Schema(std::vector<Column> columns);

    const std::vector<Column>& Columns() const noexcept { return columns_; }
    std::size_t NumColumns() const noexcept { return columns_.size(); }
    const Column& GetColumn(std::size_t idx) const { return columns_.at(idx); }

    std::optional<std::size_t> FindColumn(std::string_view name) const noexcept;

    /// Convenience: the list of column types in order (for DataChunk init).
    std::vector<ColumnType> Types() const;

    std::vector<std::byte> Serialize() const;
    static Schema Deserialize(const std::byte* data, std::size_t size, std::size_t& consumed);

    bool operator==(const Schema& o) const noexcept;

   private:
    std::vector<Column> columns_;
};

}  // namespace liteolap
