#include <catch2/catch_test_macros.hpp>
#include <map>
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

TEST_CASE("Aggregate: global COUNT and SUM", "[aggregate]") {
    auto path = TempDbPath("agg_global");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE t (id INT, amount FLOAT)");
    double expect_sum = 0;
    for (int i = 1; i <= 1000; ++i) {
        db->Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) +
                    ".0)");
        expect_sum += i;
    }
    auto rs = db->Execute("SELECT COUNT(*), SUM(amount) FROM t");
    REQUIRE(rs.rows.size() == 1);
    REQUIRE(AsInt(rs.rows[0][0]) == 1000);
    REQUIRE(std::get<double>(rs.rows[0][1]) == expect_sum);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("Aggregate: GROUP BY with COUNT/SUM/AVG/MIN/MAX", "[aggregate]") {
    auto path = TempDbPath("agg_group");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE sales (region VARCHAR(8), amount BIGINT)");
    std::map<std::string, std::int64_t> sum, cnt;
    const char* regions[] = {"EU", "US", "APAC"};
    for (int i = 0; i < 3000; ++i) {
        const std::string r = regions[i % 3];
        const std::int64_t amt = (i % 100) + 1;
        db->Execute("INSERT INTO sales VALUES ('" + r + "', " + std::to_string(amt) + ")");
        sum[r] += amt;
        cnt[r] += 1;
    }
    auto rs = db->Execute(
        "SELECT region, COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM sales GROUP BY region");
    REQUIRE(rs.rows.size() == 3);
    for (const auto& row : rs.rows) {
        const std::string r = std::get<std::string>(row[0]);
        REQUIRE(AsInt(row[1]) == cnt[r]);
        REQUIRE(AsInt(row[2]) == sum[r]);
        REQUIRE(AsInt(row[3]) == 1);
        REQUIRE(AsInt(row[4]) == 100);
    }
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("Aggregate: HAVING filters groups", "[aggregate]") {
    auto path = TempDbPath("agg_having");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE t (g INT, v BIGINT)");
    // group 1: sum 10, group 2: sum 200, group 3: sum 3000
    for (int k = 0; k < 10; ++k) db->Execute("INSERT INTO t VALUES (1, 1)");
    for (int k = 0; k < 10; ++k) db->Execute("INSERT INTO t VALUES (2, 20)");
    for (int k = 0; k < 10; ++k) db->Execute("INSERT INTO t VALUES (3, 300)");
    auto rs = db->Execute("SELECT g, SUM(v) AS s FROM t GROUP BY g HAVING SUM(v) > 100");
    REQUIRE(rs.rows.size() == 2);
    for (const auto& row : rs.rows) REQUIRE(AsInt(row[1]) > 100);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("Aggregate: WHERE before GROUP BY, ORDER BY on alias", "[aggregate]") {
    auto path = TempDbPath("agg_full");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE sales (region VARCHAR(8), amount BIGINT)");
    const char* regions[] = {"EU", "US", "APAC"};
    for (int i = 0; i < 3000; ++i) {
        db->Execute("INSERT INTO sales VALUES ('" + std::string(regions[i % 3]) + "', " +
                    std::to_string((i % 50) + 1) + ")");
    }
    auto rs = db->Execute(
        "SELECT region, SUM(amount) AS total FROM sales WHERE amount > 10 "
        "GROUP BY region ORDER BY total DESC");
    REQUIRE(rs.rows.size() == 3);
    // Sorted descending by total.
    REQUIRE(AsInt(rs.rows[0][1]) >= AsInt(rs.rows[1][1]));
    REQUIRE(AsInt(rs.rows[1][1]) >= AsInt(rs.rows[2][1]));
    db->Close();
    std::filesystem::remove(path);
}
