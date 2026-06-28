#include "execution/projection.h"

namespace liteolap {

Projection::Projection(std::unique_ptr<Operator> child, std::vector<std::size_t> source_indices,
                       std::vector<std::string> output_names)
    : child_(std::move(child)), sources_(std::move(source_indices)) {
    output_names_ = std::move(output_names);
    for (std::size_t s : sources_) output_types_.push_back(child_->output_types()[s]);
}

void Projection::Open() {
    child_->Open();
    input_.Initialize(child_->output_types());
}

void Projection::Next(DataChunk& out) {
    out.Reset();
    child_->Next(input_);
    const std::size_t n = input_.cardinality();
    if (n == 0) {
        out.set_cardinality(0);
        return;
    }
    for (std::size_t c = 0; c < sources_.size(); ++c) {
        const Vector& src = input_.GetVector(sources_[c]);
        Vector& dst = out.GetVector(c);
        for (std::size_t i = 0; i < n; ++i) dst.AppendFrom(src, i);
    }
    out.set_cardinality(n);
}

void Projection::Close() { child_->Close(); }

}  // namespace liteolap
