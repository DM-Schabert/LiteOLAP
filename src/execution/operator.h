#pragma once

#include <string>
#include <vector>

#include "common/value.h"
#include "vector/data_chunk.h"

namespace liteolap {

/**
 * Vectorized pull-based operator. `Next` fills `out` with up to `kVectorSize`
 * rows; a returned cardinality of 0 means end-of-stream. Unlike the
 * tuple-at-a-time Volcano model, each call moves a whole batch, which is the
 * source of the analytical engine's throughput.
 */
class Operator {
   public:
    virtual ~Operator() = default;
    virtual void Open() = 0;
    virtual void Next(DataChunk& out) = 0;
    virtual void Close() = 0;

    /// Output column names (qualified as `alias.col` for scans).
    const std::vector<std::string>& output_names() const noexcept { return output_names_; }
    const std::vector<ColumnType>& output_types() const noexcept { return output_types_; }

   protected:
    std::vector<std::string> output_names_;
    std::vector<ColumnType> output_types_;
};

}  // namespace liteolap
