#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "common/value.h"
#include "execution/operator.h"

namespace liteolap {

/// Fully materializes its child, sorts all rows by one key column, then emits
/// them in batches. NULLs sort before all non-null values.
class OrderBy : public Operator {
   public:
    OrderBy(std::unique_ptr<Operator> child, std::size_t key_index, bool descending);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    std::unique_ptr<Operator> child_;
    std::size_t key_index_;
    bool descending_;
    std::vector<std::vector<Value>> rows_;
    std::size_t cursor_{0};
};

}  // namespace liteolap
