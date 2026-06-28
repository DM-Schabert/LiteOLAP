#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "catalog/catalog.h"
#include "column/column_reader.h"
#include "execution/operator.h"
#include "storage/buffer_pool.h"

namespace liteolap {

/**
 * Reads a subset of a table's columns, row group by row group, emitting one
 * DataChunk per stored chunk. Only the requested columns are touched — the
 * central advantage of column storage. An optional zone-map filter on one
 * scanned integer column lets whole chunks be skipped without decoding.
 */
class ColumnScan : public Operator {
   public:
    ColumnScan(BufferPool& bp, const TableMeta& table, std::string alias,
               std::vector<std::size_t> column_indices);

    /// Pushes down a range predicate `lo <= col <= hi` on the scan-output
    /// column at position `scan_pos`, enabling zone-map chunk skipping.
    void SetZoneFilter(std::size_t scan_pos, std::int64_t lo, std::int64_t hi);

    void Open() override;
    void Next(DataChunk& out) override;
    void Close() override;

   private:
    bool OpenRowGroup(std::size_t rg);  ///< false if rg index out of range

    BufferPool& bp_;
    const TableMeta& table_;
    std::string alias_;
    std::vector<std::size_t> col_idx_;

    std::size_t rg_index_{0};
    std::vector<std::unique_ptr<ColumnReader>> readers_;

    bool has_zone_{false};
    std::size_t zone_pos_{0};
    std::int64_t zone_lo_{0};
    std::int64_t zone_hi_{0};
};

}  // namespace liteolap
