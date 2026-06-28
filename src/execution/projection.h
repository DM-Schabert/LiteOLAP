#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/operator.h"

namespace liteolap {

/// Selects and reorders a subset of its child's columns. (Scalar expression
/// projection beyond column references is not needed by the supported SQL.)
class Projection : public Operator {
   public:
    Projection(std::unique_ptr<Operator> child, std::vector<std::size_t> source_indices,
               std::vector<std::string> output_names);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    std::unique_ptr<Operator> child_;
    std::vector<std::size_t> sources_;
    DataChunk input_;
};

}  // namespace liteolap
