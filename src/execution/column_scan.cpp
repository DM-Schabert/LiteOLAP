#include "execution/column_scan.h"

#include <future>
#include <memory>
#include <stdexcept>

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

    std::vector<std::future<std::vector<DataChunk>>> futures;

    for (std::size_t rg = 0; rg < table_.row_groups.size(); ++rg) {
        futures.push_back(std::async(std::launch::async,
            [this, rg]() -> std::vector<DataChunk> {
                std::vector<DataChunk> result;
                const RowGroup& g = table_.row_groups[rg];

                std::vector<std::unique_ptr<ColumnReader>> readers;
                for (std::size_t k = 0; k < col_idx_.size(); ++k) {
                    std::size_t table_col = col_idx_[k];
                    readers.push_back(std::make_unique<ColumnReader>(
                        bp_, g.column_roots[table_col],
                        table_.schema.GetColumn(table_col).type));
                }

                while (readers[0]->NextChunk()) {
                    for (std::size_t k = 1; k < readers.size(); ++k)
                        readers[k]->NextChunk();

                    if (has_zone_) {
                        const ChunkMeta& m = readers[zone_pos_]->meta();
                        if (m.has_zone_map &&
                            (m.max_value < zone_lo_ || m.min_value > zone_hi_)) {
                            for (auto& r : readers) r->SkipPayload();
                            continue;
                        }
                    }

                    DataChunk chunk;
                    chunk.Initialize(output_types_);
                    std::uint32_t rows = readers[0]->meta().num_rows;
                    for (std::size_t k = 0; k < readers.size(); ++k)
                        readers[k]->Decode(chunk.GetVector(k));
                    chunk.set_cardinality(rows);
                    result.push_back(std::move(chunk));
                }

                return result;
            }
        ));
    }

    for (auto& f : futures) {
        auto rg_chunks = f.get();
        for (auto& c : rg_chunks)
            chunks_.push_back(std::move(c));
    }
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
