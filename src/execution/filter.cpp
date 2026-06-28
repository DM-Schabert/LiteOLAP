#include "execution/filter.h"

namespace liteolap {

Filter::Filter(std::unique_ptr<Operator> child, std::unique_ptr<BoundExpr> predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {
    output_names_ = child_->output_names();
    output_types_ = child_->output_types();
}

void Filter::Open() {
    child_->Open();
    input_.Initialize(child_->output_types());
}

void Filter::Next(DataChunk& out) {
    out.Reset();
    while (true) {
        child_->Next(input_);
        if (input_.cardinality() == 0) {
            out.set_cardinality(0);
            return;
        }
        EvalPredicate(*predicate_, input_, mask_);
        std::size_t kept = 0;
        for (std::size_t i = 0; i < input_.cardinality(); ++i) {
            if (!mask_[i]) continue;
            for (std::size_t c = 0; c < out.num_columns(); ++c) {
                out.GetVector(c).AppendFrom(input_.GetVector(c), i);
            }
            ++kept;
        }
        if (kept > 0) {
            out.set_cardinality(kept);
            return;
        }
        // Whole batch filtered out — pull the next one.
    }
}

void Filter::Close() { child_->Close(); }

}  // namespace liteolap
