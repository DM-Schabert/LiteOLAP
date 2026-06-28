#include "execution/limit.h"

namespace liteolap {

Limit::Limit(std::unique_ptr<Operator> child, std::size_t n)
    : child_(std::move(child)), remaining_(n) {
    output_names_ = child_->output_names();
    output_types_ = child_->output_types();
}

void Limit::Open() {
    child_->Open();
    input_.Initialize(child_->output_types());
}

void Limit::Next(DataChunk& out) {
    out.Reset();
    if (remaining_ == 0) {
        out.set_cardinality(0);
        return;
    }
    child_->Next(input_);
    const std::size_t n = input_.cardinality();
    if (n == 0) {
        out.set_cardinality(0);
        return;
    }
    const std::size_t take = std::min(n, remaining_);
    for (std::size_t c = 0; c < out.num_columns(); ++c) {
        for (std::size_t i = 0; i < take; ++i) out.GetVector(c).AppendFrom(input_.GetVector(c), i);
    }
    remaining_ -= take;
    out.set_cardinality(take);
}

void Limit::Close() { child_->Close(); }

}  // namespace liteolap
