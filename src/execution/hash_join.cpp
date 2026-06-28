#include "execution/hash_join.h"

namespace liteolap {

namespace {

/// Serializes a single join-key value into a hashable string. NULL yields an
/// empty sentinel that is never inserted into the build index (NULLs never
/// join).
std::string KeyOf(const Value& v) {
    std::string s;
    if (IsNull(v)) return s;  // caller skips empty keys
    if (std::holds_alternative<std::int32_t>(v)) {
        s.push_back('i');
        auto x = std::get<std::int32_t>(v);
        s.append(reinterpret_cast<const char*>(&x), sizeof(x));
    } else if (std::holds_alternative<std::int64_t>(v)) {
        s.push_back('I');
        auto x = std::get<std::int64_t>(v);
        s.append(reinterpret_cast<const char*>(&x), sizeof(x));
    } else if (std::holds_alternative<double>(v)) {
        s.push_back('d');
        auto x = std::get<double>(v);
        s.append(reinterpret_cast<const char*>(&x), sizeof(x));
    } else {
        s.push_back('s');
        s.append(std::get<std::string>(v));
    }
    return s;
}

}  // namespace

HashJoin::HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                   std::size_t left_key_index, std::size_t right_key_index)
    : left_(std::move(left)),
      right_(std::move(right)),
      left_key_(left_key_index),
      right_key_(right_key_index) {
    output_names_ = left_->output_names();
    for (const auto& n : right_->output_names()) output_names_.push_back(n);
    output_types_ = left_->output_types();
    for (auto t : right_->output_types()) output_types_.push_back(t);
}

void HashJoin::Open() {
    // Build phase: materialize the right side into a hash table.
    right_->Open();
    DataChunk rb;
    rb.Initialize(right_->output_types());
    while (true) {
        right_->Next(rb);
        if (rb.cardinality() == 0) break;
        for (std::size_t i = 0; i < rb.cardinality(); ++i) {
            std::vector<Value> row;
            row.reserve(rb.num_columns());
            for (std::size_t c = 0; c < rb.num_columns(); ++c) {
                row.push_back(rb.GetVector(c).GetValue(i));
            }
            const Value& key = row[right_key_];
            if (!IsNull(key)) {
                const std::string ks = KeyOf(key);
                build_index_[ks].push_back(static_cast<std::uint32_t>(build_rows_.size()));
            }
            build_rows_.push_back(std::move(row));
        }
    }
    right_->Close();

    // Probe phase setup.
    left_->Open();
    lchunk_.Initialize(left_->output_types());
    lpos_ = 0;
    left_done_ = false;
    matches_ = nullptr;
    mpos_ = 0;
}

bool HashJoin::AdvanceProbeRow() {
    while (true) {
        if (lpos_ >= lchunk_.cardinality()) {
            if (left_done_) return false;
            left_->Next(lchunk_);
            lpos_ = 0;
            if (lchunk_.cardinality() == 0) {
                left_done_ = true;
                return false;
            }
        }
        const Value key = lchunk_.GetVector(left_key_).GetValue(lpos_);
        if (!IsNull(key)) {
            auto it = build_index_.find(KeyOf(key));
            if (it != build_index_.end()) {
                matches_ = &it->second;
                mpos_ = 0;
                return true;
            }
        }
        ++lpos_;  // no match for this left row; advance
    }
}

void HashJoin::Next(DataChunk& out) {
    out.Reset();
    const std::size_t left_cols = left_->output_types().size();

    while (out.cardinality() < kVectorSize) {
        if (matches_ == nullptr || mpos_ >= matches_->size()) {
            // Done with current left row's matches; move to the next.
            if (matches_ != nullptr) ++lpos_;
            matches_ = nullptr;
            if (!AdvanceProbeRow()) break;
        }
        // Emit one joined row: left[lpos_] ++ build_rows_[match].
        const std::uint32_t build_idx = (*matches_)[mpos_++];
        const std::vector<Value>& rrow = build_rows_[build_idx];
        for (std::size_t c = 0; c < left_cols; ++c) {
            out.GetVector(c).AppendFrom(lchunk_.GetVector(c), lpos_);
        }
        for (std::size_t c = 0; c < rrow.size(); ++c) {
            out.GetVector(left_cols + c).AppendValue(rrow[c]);
        }
        out.set_cardinality(out.cardinality() + 1);
    }
}

void HashJoin::Close() {
    left_->Close();
    build_rows_.clear();
    build_index_.clear();
}

}  // namespace liteolap
