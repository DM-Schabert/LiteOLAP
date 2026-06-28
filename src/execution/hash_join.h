#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/value.h"
#include "execution/operator.h"

namespace liteolap {

/**
 * Inner equi-join on a single key column from each side. The right (build)
 * side is consumed fully in Open() into a hash table; the left (probe) side
 * is streamed. Output columns are the left side's followed by the right
 * side's. O(n+m) versus the nested-loop O(n*m).
 */
class HashJoin : public Operator {
   public:
    HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
             std::size_t left_key_index, std::size_t right_key_index);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    bool AdvanceProbeRow();  ///< loads the next left row's match list; false at end

    std::unique_ptr<Operator> left_;
    std::unique_ptr<Operator> right_;
    std::size_t left_key_{0};
    std::size_t right_key_{0};

    // Build side, fully materialized.
    std::vector<std::vector<Value>> build_rows_;
    std::unordered_map<std::string, std::vector<std::uint32_t>> build_index_;

    // Probe state.
    DataChunk lchunk_;
    std::size_t lpos_{0};
    bool left_done_{false};
    const std::vector<std::uint32_t>* matches_{nullptr};
    std::size_t mpos_{0};
};

}  // namespace liteolap
