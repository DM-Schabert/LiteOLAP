#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/schema.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

namespace liteolap {

/// One row group of a table: a set of column streams (one first-page id per
/// column) covering `num_rows` rows. A table is an ordered list of row
/// groups — each bulk flush appends one. This mirrors the segment/row-group
/// model of Parquet and other column stores.
struct RowGroup {
    std::uint64_t num_rows{0};
    std::vector<PageId> column_roots;  ///< size == schema column count
};

struct TableMeta {
    std::string name;
    Schema schema;
    std::vector<RowGroup> row_groups;

    std::uint64_t TotalRows() const {
        std::uint64_t n = 0;
        for (const auto& rg : row_groups) n += rg.num_rows;
        return n;
    }
};

/**
 * Persisted registry of tables and their row groups. Lives in page 0.
 * Holds no in-memory write buffers — those are the DB's concern; the
 * catalog only records what is durably on disk.
 */
class Catalog {
   public:
    Catalog() = default;

    static Catalog Load(DiskManager& dm);
    void Persist(DiskManager& dm) const;

    void CreateTable(std::string name, Schema schema);
    bool DropTable(std::string_view name);

    /// Appends a freshly written row group to a table.
    void AddRowGroup(std::string_view table, RowGroup rg);

    /// Replaces all of a table's row groups (used by TRUNCATE).
    void ClearRowGroups(std::string_view table);

    TableMeta* GetTable(std::string_view name);
    const TableMeta* GetTable(std::string_view name) const;

    std::vector<std::string> ListTables() const;
    std::size_t NumTables() const noexcept { return tables_.size(); }

   private:
    std::map<std::string, TableMeta, std::less<>> tables_;
};

}  // namespace liteolap
