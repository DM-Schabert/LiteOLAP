#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "common/value.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

namespace liteolap {

/// Query result: output column names plus materialized rows.
struct ResultSet {
    std::vector<std::string> column_names;
    std::vector<std::vector<Value>> rows;
};

/**
 * Top-level analytical database. Inserts accumulate in an in-memory,
 * column-wise write buffer and are flushed to an on-disk row group (encoded
 * column streams) lazily — before a query that reads the table, or at Close.
 * This matches the bulk-load nature of analytical workloads and keeps INSERT
 * cheap.
 */
class DB {
   public:
    static std::unique_ptr<DB> Open(const std::filesystem::path& path);
    ~DB();

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    DB(DB&&) = delete;
    DB& operator=(DB&&) = delete;

    /// Parses and executes one SQL statement.
    ResultSet Execute(std::string_view sql);

    /// Forces all pending writes to disk and persists the catalog.
    void Close();

    const Catalog& catalog() const noexcept { return catalog_; }

   private:
    explicit DB(const std::filesystem::path& path);

    void FlushTable(const std::string& name);
    void FlushAllPending();

    std::filesystem::path path_;
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPool> bp_;
    Catalog catalog_;
    /// table name -> per-column pending value buffers.
    std::map<std::string, std::vector<std::vector<Value>>> pending_;
    bool closed_{false};
};

}  // namespace liteolap
