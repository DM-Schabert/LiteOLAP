#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "catalog/catalog.h"
#include "column/column_writer.h"
#include "execution/column_scan.h"
#include "execution/expression.h"
#include "execution/filter.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "test_helpers.h"

using namespace liteolap;
using liteolap::test::TempDbPath;

namespace {

/// Builds a single-row-group table (id INT, v BIGINT) and returns its meta.
TableMeta BuildTable(BufferPool& bp, int n) {
    Schema schema({Column{"id", ColumnType::kInt, 0, 0}, Column{"v", ColumnType::kBigInt, 0, 0}});
    RowGroup rg;
    rg.num_rows = n;
    {
        ColumnWriter wid(bp, ColumnType::kInt);
        ColumnWriter wv(bp, ColumnType::kBigInt);
        for (int i = 0; i < n; ++i) {
            wid.Append(Value{static_cast<std::int32_t>(i)});
            wv.Append(Value{static_cast<std::int64_t>(i) * 10});
        }
        rg.column_roots = {wid.Finish(), wv.Finish()};
    }
    TableMeta m;
    m.name = "t";
    m.schema = schema;
    m.row_groups.push_back(std::move(rg));
    return m;
}

}  // namespace

TEST_CASE("ColumnScan emits all rows across chunks", "[operators]") {
    auto path = TempDbPath("op_scan");
    DiskManager dm(path);
    BufferPool bp(dm, 64);
    TableMeta meta = BuildTable(bp, 5000);

    ColumnScan scan(bp, meta, "t", {0, 1});
    scan.Open();
    DataChunk chunk;
    chunk.Initialize(scan.output_types());
    std::size_t total = 0;
    std::int64_t sum_v = 0;
    while (true) {
        scan.Next(chunk);
        if (chunk.cardinality() == 0) break;
        for (std::size_t i = 0; i < chunk.cardinality(); ++i) {
            REQUIRE(chunk.GetVector(0).GetData<std::int32_t>()[i] ==
                    static_cast<std::int32_t>(total));
            sum_v += chunk.GetVector(1).GetData<std::int64_t>()[i];
            ++total;
        }
    }
    scan.Close();
    REQUIRE(total == 5000);
    REQUIRE(sum_v == 10LL * (4999LL * 5000LL / 2));
    std::filesystem::remove(path);
}

TEST_CASE("Filter reduces rows by predicate", "[operators]") {
    auto path = TempDbPath("op_filter");
    DiskManager dm(path);
    BufferPool bp(dm, 64);
    TableMeta meta = BuildTable(bp, 5000);

    auto scan = std::make_unique<ColumnScan>(bp, meta, "t", std::vector<std::size_t>{0, 1});
    auto pred = BoundExpr::Binary(BoundOp::kGe, BoundExpr::Column(0, ColumnType::kInt),
                                  BoundExpr::Literal(Value{std::int32_t{4000}}));
    Filter filter(std::move(scan), std::move(pred));
    filter.Open();
    DataChunk chunk;
    chunk.Initialize(filter.output_types());
    std::size_t total = 0;
    while (true) {
        filter.Next(chunk);
        if (chunk.cardinality() == 0) break;
        for (std::size_t i = 0; i < chunk.cardinality(); ++i) {
            REQUIRE(chunk.GetVector(0).GetData<std::int32_t>()[i] >= 4000);
            ++total;
        }
    }
    filter.Close();
    REQUIRE(total == 1000);
    std::filesystem::remove(path);
}

TEST_CASE("Zone-map filter skips non-matching chunks", "[operators]") {
    auto path = TempDbPath("op_zone");
    DiskManager dm(path);
    BufferPool bp(dm, 64);
    TableMeta meta = BuildTable(bp, 3 * 2048);  // 3 chunks: ids 0..6143

    // Look for id == 5000 -> only the third chunk (4096..6143) can match.
    ColumnScan scan(bp, meta, "t", {0, 1});
    scan.SetZoneFilter(0, 5000, 5000);
    scan.Open();
    DataChunk chunk;
    chunk.Initialize(scan.output_types());
    std::size_t decoded_rows = 0;
    bool found = false;
    while (true) {
        scan.Next(chunk);
        if (chunk.cardinality() == 0) break;
        decoded_rows += chunk.cardinality();
        for (std::size_t i = 0; i < chunk.cardinality(); ++i) {
            if (chunk.GetVector(0).GetData<std::int32_t>()[i] == 5000) found = true;
        }
    }
    scan.Close();
    REQUIRE(found);
    // Only one of three chunks should have been decoded.
    REQUIRE(decoded_rows == 2048);
    std::filesystem::remove(path);
}
