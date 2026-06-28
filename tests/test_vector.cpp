#include <catch2/catch_test_macros.hpp>

#include "vector/data_chunk.h"
#include "vector/vector.h"

using namespace liteolap;

TEST_CASE("Vector appends and reads back all types incl nulls", "[vector]") {
    Vector vi(ColumnType::kInt);
    vi.AppendInt32(7);
    vi.AppendNull();
    vi.AppendInt32(-3);
    REQUIRE(vi.size() == 3);
    REQUIRE(vi.GetData<std::int32_t>()[0] == 7);
    REQUIRE_FALSE(vi.IsValid(1));
    REQUIRE(vi.GetData<std::int32_t>()[2] == -3);
    REQUIRE(std::get<std::int32_t>(vi.GetValue(0)) == 7);
    REQUIRE(IsNull(vi.GetValue(1)));

    Vector vs(ColumnType::kVarchar);
    vs.AppendString("hello");
    vs.AppendNull();
    vs.AppendString("world!");
    REQUIRE(vs.size() == 3);
    REQUIRE(vs.GetStringView(0) == "hello");
    REQUIRE_FALSE(vs.IsValid(1));
    REQUIRE(vs.GetStringView(2) == "world!");
}

TEST_CASE("Vector AppendFrom copies a row across vectors", "[vector]") {
    Vector a(ColumnType::kBigInt);
    a.AppendInt64(100);
    a.AppendNull();
    Vector b(ColumnType::kBigInt);
    b.AppendFrom(a, 0);
    b.AppendFrom(a, 1);
    REQUIRE(b.GetData<std::int64_t>()[0] == 100);
    REQUIRE_FALSE(b.IsValid(1));
}

TEST_CASE("DataChunk initializes typed vectors", "[vector]") {
    DataChunk c;
    c.Initialize({ColumnType::kInt, ColumnType::kVarchar});
    REQUIRE(c.num_columns() == 2);
    c.GetVector(0).AppendInt32(1);
    c.GetVector(1).AppendString("x");
    c.set_cardinality(1);
    REQUIRE(c.cardinality() == 1);
    c.Reset();
    REQUIRE(c.cardinality() == 0);
    REQUIRE(c.GetVector(0).size() == 0);
}
