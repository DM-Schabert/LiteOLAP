#include <catch2/catch_test_macros.hpp>
#include <string>

#include "db.h"
#include "test_helpers.h"

using namespace liteolap;
using liteolap::test::TempDbPath;

namespace {
std::int64_t AsInt(const Value& v) {
    if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v);
    return std::get<std::int32_t>(v);
}
}  // namespace

TEST_CASE("DB: create / insert / filtered scan", "[db]") {
    auto path = TempDbPath("db_basic");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE t (id INT, region VARCHAR(8), amount FLOAT)");
    for (int i = 0; i < 1000; ++i) {
        db->Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'EU', " +
                    std::to_string(i * 1.5) + ")");
    }
    auto rs = db->Execute("SELECT id FROM t WHERE amount > 750.0");
    // amount = i*1.5 > 750 -> i > 500 -> i in [501..999] = 499 rows
    REQUIRE(rs.rows.size() == 499);
    REQUIRE(rs.column_names.size() == 1);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("DB: ORDER BY and LIMIT", "[db]") {
    auto path = TempDbPath("db_order");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE t (id INT, score FLOAT)");
    db->Execute("INSERT INTO t VALUES (1, 1.0)");
    db->Execute("INSERT INTO t VALUES (2, 9.0)");
    db->Execute("INSERT INTO t VALUES (3, 4.0)");
    db->Execute("INSERT INTO t VALUES (4, 7.0)");
    auto rs = db->Execute("SELECT id FROM t ORDER BY score DESC LIMIT 2");
    REQUIRE(rs.rows.size() == 2);
    REQUIRE(AsInt(rs.rows[0][0]) == 2);
    REQUIRE(AsInt(rs.rows[1][0]) == 4);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("DB: BETWEEN and IN", "[db]") {
    auto path = TempDbPath("db_pred");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE t (id INT, region VARCHAR(8))");
    for (int i = 0; i < 100; ++i) {
        const char* r = (i % 3 == 0) ? "EU" : (i % 3 == 1 ? "US" : "APAC");
        db->Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + r + "')");
    }
    auto rs1 = db->Execute("SELECT id FROM t WHERE id BETWEEN 10 AND 19");
    REQUIRE(rs1.rows.size() == 10);
    auto rs2 = db->Execute("SELECT id FROM t WHERE region IN ('EU', 'US')");
    // EU when i%3==0 (34 values: 0,3,..99) + US when i%3==1 (33 values)
    REQUIRE(rs2.rows.size() == 67);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("DB: persistence across reopen", "[db]") {
    auto path = TempDbPath("db_reopen");
    {
        auto db = DB::Open(path);
        db->Execute("CREATE TABLE t (id INT, name VARCHAR(16))");
        db->Execute("INSERT INTO t VALUES (1, 'hello')");
        db->Execute("INSERT INTO t VALUES (2, 'world')");
        db->Close();
    }
    auto db = DB::Open(path);
    auto rs = db->Execute("SELECT name FROM t WHERE id = 2");
    REQUIRE(rs.rows.size() == 1);
    REQUIRE(std::get<std::string>(rs.rows[0][0]) == "world");
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("DB: TRUNCATE clears rows", "[db]") {
    auto path = TempDbPath("db_trunc");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE t (id INT)");
    for (int i = 0; i < 50; ++i) db->Execute("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    REQUIRE(db->Execute("SELECT id FROM t").rows.size() == 50);
    db->Execute("TRUNCATE TABLE t");
    REQUIRE(db->Execute("SELECT id FROM t").rows.empty());
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("DB: parse errors do not crash", "[db]") {
    auto path = TempDbPath("db_err");
    auto db = DB::Open(path);
    REQUIRE_THROWS_AS(db->Execute("CREATE TABLE"), std::runtime_error);
    REQUIRE_THROWS_AS(db->Execute("UPDATE t SET x = 1"), std::runtime_error);
    db->Close();
    std::filesystem::remove(path);
}
