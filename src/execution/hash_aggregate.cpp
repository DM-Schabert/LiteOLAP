#include "execution/hash_aggregate.h"

#include <cstring>
#include <stdexcept>

namespace liteolap {

namespace {

/// Serializes a group's key values into a byte string for hashing.
std::string KeyString(const std::vector<Value>& key) {
    std::string s;
    for (const auto& v : key) {
        if (IsNull(v)) {
            s.push_back('\0');
            continue;
        }
        s.push_back('\1');
        if (std::holds_alternative<std::int32_t>(v)) {
            auto x = std::get<std::int32_t>(v);
            s.append(reinterpret_cast<const char*>(&x), sizeof(x));
        } else if (std::holds_alternative<std::int64_t>(v)) {
            auto x = std::get<std::int64_t>(v);
            s.append(reinterpret_cast<const char*>(&x), sizeof(x));
        } else if (std::holds_alternative<double>(v)) {
            auto x = std::get<double>(v);
            s.append(reinterpret_cast<const char*>(&x), sizeof(x));
        } else {
            const auto& str = std::get<std::string>(v);
            s.push_back('\2');  // separator to avoid prefix collisions
            s.append(str);
            s.push_back('\3');
        }
    }
    return s;
}

double AsDoubleVal(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<std::int64_t>(v)) return static_cast<double>(std::get<std::int64_t>(v));
    return static_cast<double>(std::get<std::int32_t>(v));
}
std::int64_t AsI64Val(const Value& v) {
    if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v);
    return std::get<std::int32_t>(v);
}

}  // namespace

HashAggregate::HashAggregate(std::unique_ptr<Operator> child,
                             std::vector<std::size_t> group_indices,
                             std::vector<std::string> group_names,
                             std::vector<ColumnType> group_types, std::vector<AggSpec> aggregates)
    : child_(std::move(child)),
      group_indices_(std::move(group_indices)),
      group_types_(std::move(group_types)),
      aggs_(std::move(aggregates)) {
    for (std::size_t i = 0; i < group_indices_.size(); ++i) {
        output_names_.push_back(group_names[i]);
        output_types_.push_back(group_types_[i]);
    }
    for (const auto& a : aggs_) {
        output_names_.push_back(a.output_name);
        output_types_.push_back(a.output_type);
    }
}

void HashAggregate::UpdateGroup(GroupState& g, const DataChunk& chunk, std::size_t row) {
    for (std::size_t ai = 0; ai < aggs_.size(); ++ai) {
        const AggSpec& spec = aggs_[ai];
        Acc& acc = g.accs[ai];
        if (spec.kind == sql::AggKind::kCountStar) {
            ++acc.count;
            continue;
        }
        const Value v = chunk.GetVector(spec.input_index).GetValue(row);
        if (IsNull(v)) continue;
        ++acc.nonnull;
        switch (spec.kind) {
            case sql::AggKind::kCount:
                break;  // nonnull already counted
            case sql::AggKind::kSum:
                if (spec.input_type == ColumnType::kFloat)
                    acc.dsum += AsDoubleVal(v);
                else
                    acc.isum += AsI64Val(v);
                break;
            case sql::AggKind::kAvg:
                acc.dsum += AsDoubleVal(v);
                break;
            case sql::AggKind::kMin:
                if (IsNull(acc.minv) || CompareValues(v, acc.minv) == Ordering::kLess)
                    acc.minv = v;
                break;
            case sql::AggKind::kMax:
                if (IsNull(acc.maxv) || CompareValues(v, acc.maxv) == Ordering::kGreater)
                    acc.maxv = v;
                break;
            default:
                break;
        }
    }
}

Value HashAggregate::Finalize(const AggSpec& spec, const Acc& a) const {
    switch (spec.kind) {
        case sql::AggKind::kCountStar:
            return Value{a.count};
        case sql::AggKind::kCount:
            return Value{a.nonnull};
        case sql::AggKind::kSum:
            if (a.nonnull == 0) return Value{Null{}};
            if (spec.input_type == ColumnType::kFloat) return Value{a.dsum};
            return Value{a.isum};
        case sql::AggKind::kAvg:
            if (a.nonnull == 0) return Value{Null{}};
            return Value{a.dsum / static_cast<double>(a.nonnull)};
        case sql::AggKind::kMin:
            return a.minv;
        case sql::AggKind::kMax:
            return a.maxv;
    }
    return Value{Null{}};
}

void HashAggregate::Open() {
    child_->Open();
    DataChunk in;
    in.Initialize(child_->output_types());

    while (true) {
        child_->Next(in);
        if (in.cardinality() == 0) break;
        for (std::size_t i = 0; i < in.cardinality(); ++i) {
            std::vector<Value> key;
            key.reserve(group_indices_.size());
            for (std::size_t gi : group_indices_) key.push_back(in.GetVector(gi).GetValue(i));
            const std::string ks = KeyString(key);
            auto it = index_.find(ks);
            std::size_t gidx;
            if (it == index_.end()) {
                gidx = groups_.size();
                index_.emplace(ks, gidx);
                GroupState gs;
                gs.key = std::move(key);
                gs.accs.resize(aggs_.size());
                groups_.push_back(std::move(gs));
            } else {
                gidx = it->second;
            }
            UpdateGroup(groups_[gidx], in, i);
        }
    }

    // No GROUP BY over empty input still yields one row (e.g. COUNT(*) = 0).
    if (group_indices_.empty() && groups_.empty()) {
        GroupState gs;
        gs.accs.resize(aggs_.size());
        groups_.push_back(std::move(gs));
    }
    cursor_ = 0;
}

void HashAggregate::Next(DataChunk& out) {
    out.Reset();
    if (cursor_ >= groups_.size()) {
        out.set_cardinality(0);
        return;
    }
    const std::size_t take = std::min(kVectorSize, groups_.size() - cursor_);
    for (std::size_t r = 0; r < take; ++r) {
        const GroupState& g = groups_[cursor_ + r];
        std::size_t col = 0;
        for (std::size_t k = 0; k < group_indices_.size(); ++k, ++col) {
            out.GetVector(col).AppendValue(g.key[k]);
        }
        for (std::size_t ai = 0; ai < aggs_.size(); ++ai, ++col) {
            out.GetVector(col).AppendValue(Finalize(aggs_[ai], g.accs[ai]));
        }
    }
    cursor_ += take;
    out.set_cardinality(take);
}

void HashAggregate::Close() {
    child_->Close();
    groups_.clear();
    index_.clear();
}

}  // namespace liteolap
