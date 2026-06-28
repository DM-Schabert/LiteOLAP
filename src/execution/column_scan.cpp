#include "execution/column_scan.h"

#include <stdexcept>

namespace liteolap {

ColumnScan::ColumnScan(BufferPool& bp, const TableMeta& table, std::string alias,
                       std::vector<std::size_t> column_indices)
    : bp_(bp), table_(table), alias_(std::move(alias)), col_idx_(std::move(column_indices)) {
    for (std::size_t idx : col_idx_) {
        const Column& c = table_.schema.GetColumn(idx);
        output_names_.push_back(alias_.empty() ? c.name : alias_ + "." + c.name);
        output_types_.push_back(c.type);
    }
}

void ColumnScan::SetZoneFilter(std::size_t scan_pos, std::int64_t lo, std::int64_t hi) {
    has_zone_ = true;
    zone_pos_ = scan_pos;
    zone_lo_ = lo;
    zone_hi_ = hi;
}

bool ColumnScan::OpenRowGroup(std::size_t rg) {
    readers_.clear();
    if (rg >= table_.row_groups.size()) return false;
    const RowGroup& g = table_.row_groups[rg];
    for (std::size_t k = 0; k < col_idx_.size(); ++k) {
        const std::size_t table_col = col_idx_[k];
        readers_.push_back(std::make_unique<ColumnReader>(
            bp_, g.column_roots[table_col], table_.schema.GetColumn(table_col).type));
    }
    return true;
}

void ColumnScan::Open() {
    rg_index_ = 0;
    readers_.clear();
    while (rg_index_ < table_.row_groups.size()) {
        if (OpenRowGroup(rg_index_) && !readers_.empty()) break;
        ++rg_index_;
    }
}

void ColumnScan::Next(DataChunk& out) {
    out.Reset();
    while (true) {
        if (readers_.empty()) {
            out.set_cardinality(0);
            return;
        }
        // Advance all readers to the next aligned chunk.
        bool have = readers_[0]->NextChunk();
        for (std::size_t k = 1; k < readers_.size(); ++k) {
            const bool h = readers_[k]->NextChunk();
            if (h != have) throw std::runtime_error("ColumnScan: misaligned column chunks");
        }
        if (!have) {
            // Row group exhausted; advance to the next non-empty one.
            ++rg_index_;
            readers_.clear();
            while (rg_index_ < table_.row_groups.size()) {
                if (OpenRowGroup(rg_index_) && !readers_.empty()) break;
                ++rg_index_;
            }
            continue;
        }

        // Zone-map pruning: if the filtered column's [min,max] cannot overlap
        // the predicate range, skip this chunk's payload in every column.
        if (has_zone_) {
            const ChunkMeta& m = readers_[zone_pos_]->meta();
            if (m.has_zone_map && (m.max_value < zone_lo_ || m.min_value > zone_hi_)) {
                for (auto& r : readers_) r->SkipPayload();
                continue;
            }
        }

        const std::uint32_t rows = readers_[0]->meta().num_rows;
        for (std::size_t k = 0; k < readers_.size(); ++k) {
            readers_[k]->Decode(out.GetVector(k));
        }
        out.set_cardinality(rows);
        return;
    }
}

void ColumnScan::Close() { readers_.clear(); }

}  // namespace liteolap
