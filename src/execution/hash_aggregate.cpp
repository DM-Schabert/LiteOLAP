#include "execution/hash_aggregate.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>

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
        const Value v = EvalExprRow(*spec.input_expr, chunk, row);
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

void HashAggregate::ProcessRow(std::vector<GroupState>& groups,
                               std::unordered_map<std::string, std::size_t>& index,
                               const DataChunk& chunk, std::size_t row) {
    std::vector<Value> key;
    key.reserve(group_indices_.size());
    for (std::size_t gi : group_indices_) key.push_back(chunk.GetVector(gi).GetValue(row));
    const std::string ks = KeyString(key);
    auto it = index.find(ks);
    std::size_t gidx;
    if (it == index.end()) {
        gidx = groups.size();
        index.emplace(ks, gidx);
        GroupState gs;
        gs.key = std::move(key);
        gs.accs.resize(aggs_.size());
        groups.push_back(std::move(gs));
    } else {
        gidx = it->second;
    }
    UpdateGroup(groups[gidx], chunk, row);
}

void HashAggregate::MergeAcc(Acc& dst, const Acc& src) const {
    // Field-wise merge. Each accumulator only populates the fields relevant to
    // its aggregate kind; the rest stay 0 / null, so merging all of them is
    // safe regardless of kind.
    dst.count += src.count;
    dst.nonnull += src.nonnull;
    dst.isum += src.isum;
    dst.dsum += src.dsum;
    if (!IsNull(src.minv) &&
        (IsNull(dst.minv) || CompareValues(src.minv, dst.minv) == Ordering::kLess))
        dst.minv = src.minv;
    if (!IsNull(src.maxv) &&
        (IsNull(dst.maxv) || CompareValues(src.maxv, dst.maxv) == Ordering::kGreater))
        dst.maxv = src.maxv;
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

    // Drain the child fully so chunks can be aggregated in parallel.
    std::vector<DataChunk> inputs;
    while (true) {
        DataChunk in;
        in.Initialize(child_->output_types());
        child_->Next(in);
        if (in.cardinality() == 0) break;
        inputs.push_back(std::move(in));
    }

    if (!inputs.empty()) {
        // Each worker builds its own (groups, index) map — no locking on the
        // hot path. Workers pull chunks from a shared atomic cursor.
        struct Partial {
            std::vector<GroupState> groups;
            std::unordered_map<std::string, std::size_t> index;
        };

        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        const std::size_t num_workers = std::min<std::size_t>(hw, inputs.size());
        std::vector<Partial> partials(num_workers);

        std::atomic<std::size_t> next_chunk{0};
        auto worker = [&](Partial& pr) {
            for (;;) {
                const std::size_t ci = next_chunk.fetch_add(1);
                if (ci >= inputs.size()) break;
                const DataChunk& in = inputs[ci];
                for (std::size_t i = 0; i < in.cardinality(); ++i)
                    ProcessRow(pr.groups, pr.index, in, i);
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(num_workers);
        for (std::size_t w = 0; w < num_workers; ++w)
            pool.emplace_back(worker, std::ref(partials[w]));
        for (auto& t : pool) t.join();

        // Merge per-worker partials into the final groups_.
        for (auto& pr : partials) {
            for (auto& g : pr.groups) {
                const std::string ks = KeyString(g.key);
                auto it = index_.find(ks);
                if (it == index_.end()) {
                    index_.emplace(ks, groups_.size());
                    groups_.push_back(std::move(g));
                } else {
                    GroupState& dst = groups_[it->second];
                    for (std::size_t ai = 0; ai < aggs_.size(); ++ai)
                        MergeAcc(dst.accs[ai], g.accs[ai]);
                }
            }
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
