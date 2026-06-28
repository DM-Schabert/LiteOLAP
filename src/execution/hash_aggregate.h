#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/value.h"
#include "execution/operator.h"
#include "sql/ast.h"

namespace liteolap {

/// One aggregate to compute.
struct AggSpec {
    sql::AggKind kind;
    std::size_t input_index{0};      ///< child output column (unused for COUNT(*))
    ColumnType input_type{ColumnType::kInt};
    ColumnType output_type{ColumnType::kBigInt};
    std::string output_name;
};

/**
 * Hash-based aggregation with optional GROUP BY. Consumes the child fully in
 * Open(), building one accumulator set per distinct group key, then streams
 * one output row per group. Output columns are the group-by columns followed
 * by the aggregate results. With no GROUP BY, exactly one row is produced
 * (even over empty input, per SQL semantics).
 */
class HashAggregate : public Operator {
   public:
    HashAggregate(std::unique_ptr<Operator> child, std::vector<std::size_t> group_indices,
                  std::vector<std::string> group_names, std::vector<ColumnType> group_types,
                  std::vector<AggSpec> aggregates);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    struct Acc {
        std::int64_t count{0};        ///< COUNT(*) / COUNT(col)
        std::int64_t nonnull{0};      ///< rows with a non-null arg
        std::int64_t isum{0};         ///< integer running sum
        double dsum{0.0};             ///< floating running sum
        Value minv{Null{}};
        Value maxv{Null{}};
    };
    struct GroupState {
        std::vector<Value> key;
        std::vector<Acc> accs;
    };

    void UpdateGroup(GroupState& g, const DataChunk& chunk, std::size_t row);
    Value Finalize(const AggSpec& spec, const Acc& a) const;

    std::unique_ptr<Operator> child_;
    std::vector<std::size_t> group_indices_;
    std::vector<ColumnType> group_types_;
    std::vector<AggSpec> aggs_;

    std::vector<GroupState> groups_;
    std::unordered_map<std::string, std::size_t> index_;
    std::size_t cursor_{0};
};

}  // namespace liteolap
