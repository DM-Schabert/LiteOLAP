#pragma once

#include <memory>
#include <vector>

#include "execution/expression.h"
#include "execution/operator.h"

namespace liteolap {

/// Applies a boolean predicate to its child's batches, gathering surviving
/// rows into the output. Output schema matches the child's.
class Filter : public Operator {
   public:
    Filter(std::unique_ptr<Operator> child, std::unique_ptr<BoundExpr> predicate);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    std::unique_ptr<Operator> child_;
    std::unique_ptr<BoundExpr> predicate_;
    DataChunk input_;
    std::vector<std::uint8_t> mask_;
};

}  // namespace liteolap
