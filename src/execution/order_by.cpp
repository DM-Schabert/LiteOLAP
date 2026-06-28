#include "execution/order_by.h"

#include <algorithm>

namespace liteolap {

OrderBy::OrderBy(std::unique_ptr<Operator> child, std::size_t key_index, bool descending)
    : child_(std::move(child)), key_index_(key_index), descending_(descending) {
    output_names_ = child_->output_names();
    output_types_ = child_->output_types();
}

void OrderBy::Open() {
    child_->Open();
    DataChunk in;
    in.Initialize(child_->output_types());
    while (true) {
        child_->Next(in);
        if (in.cardinality() == 0) break;
        for (std::size_t i = 0; i < in.cardinality(); ++i) {
            std::vector<Value> row;
            row.reserve(in.num_columns());
            for (std::size_t c = 0; c < in.num_columns(); ++c) {
                row.push_back(in.GetVector(c).GetValue(i));
            }
            rows_.push_back(std::move(row));
        }
    }

    const std::size_t key = key_index_;
    const bool desc = descending_;
    std::stable_sort(rows_.begin(), rows_.end(),
                     [key, desc](const std::vector<Value>& a, const std::vector<Value>& b) {
                         const Value& va = a[key];
                         const Value& vb = b[key];
                         const bool na = IsNull(va);
                         const bool nb = IsNull(vb);
                         if (na || nb) {
                             if (na && nb) return false;
                             // NULLs first in ascending; last in descending.
                             return desc ? nb : na;
                         }
                         const Ordering o = CompareValues(va, vb);
                         if (o == Ordering::kEqual) return false;
                         const bool less = o == Ordering::kLess;
                         return desc ? !less : less;
                     });
    cursor_ = 0;
}

void OrderBy::Next(DataChunk& out) {
    out.Reset();
    if (cursor_ >= rows_.size()) {
        out.set_cardinality(0);
        return;
    }
    const std::size_t take = std::min(kVectorSize, rows_.size() - cursor_);
    for (std::size_t r = 0; r < take; ++r) {
        const std::vector<Value>& row = rows_[cursor_ + r];
        for (std::size_t c = 0; c < out.num_columns(); ++c) {
            out.GetVector(c).AppendValue(row[c]);
        }
    }
    cursor_ += take;
    out.set_cardinality(take);
}

void OrderBy::Close() {
    child_->Close();
    rows_.clear();
}

}  // namespace liteolap
