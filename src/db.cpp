#include "db.h"

#include <stdexcept>

#include "column/column_writer.h"
#include "execution/operator.h"
#include "planner/planner.h"
#include "sql/parser.h"

namespace liteolap {

namespace {

constexpr std::size_t kRowGroupSize = 100'000;

/// Coerces a parsed literal to the declared column type (e.g. an integer
/// literal into a BIGINT or FLOAT column).
Value Coerce(const Value& v, ColumnType type) {
    if (IsNull(v)) return v;
    switch (type) {
        case ColumnType::kInt:
            if (std::holds_alternative<std::int32_t>(v)) return v;
            if (std::holds_alternative<std::int64_t>(v))
                return Value{static_cast<std::int32_t>(std::get<std::int64_t>(v))};
            break;
        case ColumnType::kBigInt:
            if (std::holds_alternative<std::int64_t>(v)) return v;
            if (std::holds_alternative<std::int32_t>(v))
                return Value{static_cast<std::int64_t>(std::get<std::int32_t>(v))};
            break;
        case ColumnType::kFloat:
            if (std::holds_alternative<double>(v)) return v;
            if (std::holds_alternative<std::int32_t>(v))
                return Value{static_cast<double>(std::get<std::int32_t>(v))};
            if (std::holds_alternative<std::int64_t>(v))
                return Value{static_cast<double>(std::get<std::int64_t>(v))};
            break;
        case ColumnType::kVarchar:
            if (std::holds_alternative<std::string>(v)) return v;
            break;
    }
    throw std::runtime_error("INSERT: value does not match column type");
}

}  // namespace

DB::DB(const std::filesystem::path& path) : path_(path) {
    disk_ = std::make_unique<DiskManager>(path_);
    bp_ = std::make_unique<BufferPool>(*disk_);
    catalog_ = Catalog::Load(*disk_);
}

std::unique_ptr<DB> DB::Open(const std::filesystem::path& path) {
    return std::unique_ptr<DB>(new DB(path));
}

DB::~DB() { Close(); }

void DB::FlushTable(const std::string& name) {
    auto it = pending_.find(name);
    if (it == pending_.end() || it->second.empty()) return;
    auto& cols = it->second;
    if (cols[0].empty()) return;

    TableMeta* meta = catalog_.GetTable(name);
    if (!meta) return;

    const std::size_t total = cols[0].size();
    for (std::size_t start = 0; start < total; start += kRowGroupSize) {
        const std::size_t end = std::min(start + kRowGroupSize, total);
        RowGroup rg;
        rg.num_rows = end - start;
        rg.column_roots.resize(meta->schema.NumColumns());
        for (std::size_t c = 0; c < meta->schema.NumColumns(); ++c) {
            ColumnWriter w(*bp_, meta->schema.GetColumn(c).type);
            for (std::size_t r = start; r < end; ++r) w.Append(cols[c][r]);
            rg.column_roots[c] = w.Finish();
        }
        catalog_.AddRowGroup(name, std::move(rg));
    }

    for (auto& cv : cols) cv.clear();
    bp_->FlushAll();
    catalog_.Persist(*disk_);
}

void DB::FlushAllPending() {
    for (const auto& [name, _] : pending_) FlushTable(name);
}

void DB::Close() {
    if (closed_) return;
    FlushAllPending();
    if (bp_) bp_->FlushAll();
    if (disk_) catalog_.Persist(*disk_);
    bp_.reset();
    disk_.reset();
    closed_ = true;
}

ResultSet DB::Execute(std::string_view sql) {
    auto stmt = sql::Parse(sql);

    if (const auto* ct = dynamic_cast<sql::CreateTableStmt*>(stmt.get())) {
        if (catalog_.GetTable(ct->table_name))
            throw std::runtime_error("table already exists: " + ct->table_name);
        std::vector<Column> cols;
        for (std::size_t i = 0; i < ct->columns.size(); ++i) {
            Column c;
            c.name = ct->columns[i].name;
            c.type = ct->columns[i].type;
            c.varchar_len = ct->columns[i].varchar_len;
            c.column_index = static_cast<std::uint16_t>(i);
            cols.push_back(std::move(c));
        }
        Schema schema(std::move(cols));
        pending_[ct->table_name].assign(schema.NumColumns(), {});
        catalog_.CreateTable(ct->table_name, std::move(schema));
        catalog_.Persist(*disk_);
        return {};
    }

    if (const auto* dt = dynamic_cast<sql::DropTableStmt*>(stmt.get())) {
        if (!catalog_.DropTable(dt->table_name))
            throw std::runtime_error("unknown table: " + dt->table_name);
        pending_.erase(dt->table_name);
        catalog_.Persist(*disk_);
        return {};
    }

    if (const auto* tr = dynamic_cast<sql::TruncateStmt*>(stmt.get())) {
        if (!catalog_.GetTable(tr->table_name))
            throw std::runtime_error("unknown table: " + tr->table_name);
        catalog_.ClearRowGroups(tr->table_name);
        auto it = pending_.find(tr->table_name);
        if (it != pending_.end())
            for (auto& cv : it->second) cv.clear();
        catalog_.Persist(*disk_);
        return {};
    }

    if (const auto* ins = dynamic_cast<sql::InsertStmt*>(stmt.get())) {
        TableMeta* meta = catalog_.GetTable(ins->table_name);
        if (!meta) throw std::runtime_error("unknown table: " + ins->table_name);
        auto& cols = pending_[ins->table_name];
        if (cols.size() != meta->schema.NumColumns())
            cols.assign(meta->schema.NumColumns(), {});
        for (const auto& row : ins->rows) {
            if (row.size() != meta->schema.NumColumns())
                throw std::runtime_error("INSERT: value count does not match column count");
            for (std::size_t i = 0; i < row.size(); ++i) {
                cols[i].push_back(Coerce(row[i], meta->schema.GetColumn(i).type));
            }
        }
        return {};
    }

    if (const auto* sel = dynamic_cast<sql::SelectStmt*>(stmt.get())) {
        // Make all referenced tables' pending writes visible to the scan.
        for (const auto& tref : sel->tables) FlushTable(tref.name);

        auto op = PlanSelect(*sel, catalog_, *bp_);
        op->Open();
        ResultSet rs;
        rs.column_names = op->output_names();
        DataChunk chunk;
        chunk.Initialize(op->output_types());
        while (true) {
            op->Next(chunk);
            if (chunk.cardinality() == 0) break;
            for (std::size_t i = 0; i < chunk.cardinality(); ++i) {
                std::vector<Value> row;
                row.reserve(chunk.num_columns());
                for (std::size_t c = 0; c < chunk.num_columns(); ++c) {
                    row.push_back(chunk.GetVector(c).GetValue(i));
                }
                rs.rows.push_back(std::move(row));
            }
        }
        op->Close();
        return rs;
    }

    throw std::runtime_error("DB::Execute: unsupported statement");
}

}  // namespace liteolap
