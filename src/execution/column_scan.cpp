#include "execution/column_scan.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

#include "column/column_reader.h"

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

void ColumnScan::Open() {
    chunks_.clear();
    chunk_cursor_ = 0;

    const std::size_t num_rg = table_.row_groups.size();
    if (num_rg == 0) return;

    // One result slot per row group: workers may finish out of order, but the
    // final flatten below restores row-group order.
    std::vector<std::vector<DataChunk>> per_rg(num_rg);

    // Decode one row group's chunks into its slot.
    auto decode_rg = [this, &per_rg](std::size_t rg) {
        const RowGroup& g = table_.row_groups[rg];
        std::vector<std::unique_ptr<ColumnReader>> readers;
        for (std::size_t k = 0; k < col_idx_.size(); ++k) {
            const std::size_t table_col = col_idx_[k];
            readers.push_back(std::make_unique<ColumnReader>(
                bp_, g.column_roots[table_col], table_.schema.GetColumn(table_col).type));
        }

        std::vector<DataChunk>& out = per_rg[rg];
        while (readers[0]->NextChunk()) {
            for (std::size_t k = 1; k < readers.size(); ++k) readers[k]->NextChunk();

            if (has_zone_) {
                const ChunkMeta& m = readers[zone_pos_]->meta();
                if (m.has_zone_map && (m.max_value < zone_lo_ || m.min_value > zone_hi_)) {
                    for (auto& r : readers) r->SkipPayload();
                    continue;
                }
            }

            DataChunk chunk;
            chunk.Initialize(output_types_);
            const std::uint32_t rows = readers[0]->meta().num_rows;
            for (std::size_t k = 0; k < readers.size(); ++k) readers[k]->Decode(chunk.GetVector(k));
            chunk.set_cardinality(rows);
            out.push_back(std::move(chunk));
        }
    };

    // Bounded pool: a fixed set of workers pull row groups from a shared atomic
    // cursor, so the thread count stays constant no matter how many groups.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const std::size_t num_workers = std::min<std::size_t>(hw, num_rg);

    std::atomic<std::size_t> next_rg{0};
    auto worker = [&]() {
        for (;;) {
            const std::size_t rg = next_rg.fetch_add(1);
            if (rg >= num_rg) break;
            decode_rg(rg);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    // Flatten in row-group order.
    for (auto& rg_chunks : per_rg)
        for (auto& c : rg_chunks) chunks_.push_back(std::move(c));
}

void ColumnScan::Next(DataChunk& out) {
    if (chunk_cursor_ >= chunks_.size()) {
        out.set_cardinality(0);
    } else {
        out = std::move(chunks_[chunk_cursor_++]);
    }
}

void ColumnScan::Close() { chunks_.clear(); }

}  // namespace liteolap
