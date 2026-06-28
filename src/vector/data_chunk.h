#pragma once

#include <memory>
#include <vector>

#include "vector/vector.h"

namespace liteolap {

/**
 * A horizontal slice of a result: a set of column Vectors that all carry the
 * same number of rows (`cardinality`). This is what `Operator::Next` fills.
 * A `cardinality` of 0 signals end-of-stream.
 */
class DataChunk {
   public:
    DataChunk() = default;

    /// (Re)initializes the chunk with one empty Vector per type.
    void Initialize(const std::vector<ColumnType>& types);

    std::size_t num_columns() const noexcept { return columns_.size(); }
    std::size_t cardinality() const noexcept { return cardinality_; }
    void set_cardinality(std::size_t n) noexcept { cardinality_ = n; }

    Vector& GetVector(std::size_t i) { return *columns_[i]; }
    const Vector& GetVector(std::size_t i) const { return *columns_[i]; }

    /// Clears all vectors and resets cardinality to 0.
    void Reset();

    const std::vector<ColumnType>& types() const noexcept { return types_; }

   private:
    std::vector<ColumnType> types_;
    std::vector<std::unique_ptr<Vector>> columns_;
    std::size_t cardinality_{0};
};

}  // namespace liteolap
