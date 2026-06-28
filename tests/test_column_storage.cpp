#include <catch2/catch_test_macros.hpp>
#include <random>
#include <string>
#include <vector>

#include "column/column_reader.h"
#include "column/column_writer.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "test_helpers.h"

using namespace liteolap;
using liteolap::test::TempDbPath;

namespace {

/// Reads every chunk of a column into one flat Vector.
void ReadAll(BufferPool& bp, PageId first, ColumnType type, Vector& out) {
    ColumnReader r(bp, first, type);
    while (r.NextChunk()) r.Decode(out);
}

}  // namespace

TEST_CASE("Column storage round-trips 100k ints across chunks", "[column]") {
    auto path = TempDbPath("col_int");
    DiskManager dm(path);
    BufferPool bp(dm, 64);

    constexpr int kN = 100'000;
    PageId first;
    {
        ColumnWriter w(bp, ColumnType::kInt);
        for (int i = 0; i < kN; ++i) w.Append(Value{static_cast<std::int32_t>(i)});
        first = w.Finish();
    }
    Vector out(ColumnType::kInt);
    ReadAll(bp, first, ColumnType::kInt, out);
    REQUIRE(out.size() == kN);
    for (int i = 0; i < kN; ++i) REQUIRE(out.GetData<std::int32_t>()[i] == i);

    std::filesystem::remove(path);
}

TEST_CASE("Column storage round-trips varchar with mixed lengths + nulls", "[column]") {
    auto path = TempDbPath("col_str");
    DiskManager dm(path);
    BufferPool bp(dm, 64);

    std::vector<Value> values;
    std::mt19937_64 rng(11);
    for (int i = 0; i < 20'000; ++i) {
        if (i % 13 == 0) {
            values.push_back(Value{Null{}});
        } else {
            values.push_back(Value{std::string(rng() % 20, 'a' + (i % 26))});
        }
    }
    PageId first;
    {
        ColumnWriter w(bp, ColumnType::kVarchar);
        for (auto& v : values) w.Append(v);
        first = w.Finish();
    }
    Vector out(ColumnType::kVarchar);
    ReadAll(bp, first, ColumnType::kVarchar, out);
    REQUIRE(out.size() == values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (IsNull(values[i])) {
            REQUIRE_FALSE(out.IsValid(i));
        } else {
            REQUIRE(out.GetValue(i) == values[i]);
        }
    }
    std::filesystem::remove(path);
}

TEST_CASE("Column storage handles all types with nulls", "[column]") {
    auto path = TempDbPath("col_all");
    DiskManager dm(path);
    BufferPool bp(dm, 64);

    auto run = [&](ColumnType type, const std::vector<Value>& vals) {
        PageId first;
        {
            ColumnWriter w(bp, type);
            for (auto& v : vals) w.Append(v);
            first = w.Finish();
        }
        Vector out(type);
        ReadAll(bp, first, type, out);
        REQUIRE(out.size() == vals.size());
        for (std::size_t i = 0; i < vals.size(); ++i) {
            if (IsNull(vals[i]))
                REQUIRE_FALSE(out.IsValid(i));
            else
                REQUIRE(out.GetValue(i) == vals[i]);
        }
    };

    std::vector<Value> bigs, dbls;
    for (int i = 0; i < 5000; ++i) {
        bigs.push_back(i % 5 == 0 ? Value{Null{}} : Value{static_cast<std::int64_t>(i) * 1000});
        dbls.push_back(i % 9 == 0 ? Value{Null{}} : Value{i * 0.25});
    }
    run(ColumnType::kBigInt, bigs);
    run(ColumnType::kFloat, dbls);

    std::filesystem::remove(path);
}

TEST_CASE("Column zone-map metadata is exposed per chunk", "[column]") {
    auto path = TempDbPath("col_zone");
    DiskManager dm(path);
    BufferPool bp(dm, 64);

    // 3 chunks: [0..2047], [2048..4095], [4096..6143] (kChunkRows = 2048).
    PageId first;
    {
        ColumnWriter w(bp, ColumnType::kInt);
        for (int i = 0; i < 3 * 2048; ++i) w.Append(Value{static_cast<std::int32_t>(i)});
        first = w.Finish();
    }
    ColumnReader r(bp, first, ColumnType::kInt);
    int chunk = 0;
    while (r.NextChunk()) {
        REQUIRE(r.meta().has_zone_map == 1);
        REQUIRE(r.meta().min_value == chunk * 2048);
        REQUIRE(r.meta().max_value == chunk * 2048 + 2047);
        r.SkipPayload();
        ++chunk;
    }
    REQUIRE(chunk == 3);
    std::filesystem::remove(path);
}
