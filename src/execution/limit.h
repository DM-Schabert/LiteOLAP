#pragma once

#include <cstddef>
#include <memory>

#include "execution/operator.h"

namespace liteolap {

/// Passes through at most `n` rows from its child.
class Limit : public Operator {
   public:
    Limit(std::unique_ptr<Operator> child, std::size_t n);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    std::unique_ptr<Operator> child_;
    std::size_t remaining_;
    DataChunk input_;
};

}  // namespace liteolap
