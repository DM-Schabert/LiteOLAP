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

TEST_CASE("Join: two-table equi-join returns matched rows", "[join]") {
    auto path = TempDbPath("join_basic");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE users (id INT, name VARCHAR(16))");
    db->Execute("CREATE TABLE orders (oid INT, uid INT, total FLOAT)");
    db->Execute("INSERT INTO users VALUES (1, 'Alice')");
    db->Execute("INSERT INTO users VALUES (2, 'Bob')");
    db->Execute("INSERT INTO orders VALUES (10, 1, 9.99)");
    db->Execute("INSERT INTO orders VALUES (11, 1, 19.99)");
    db->Execute("INSERT INTO orders VALUES (12, 2, 5.00)");

    auto rs = db->Execute(
        "SELECT u.name, o.total FROM users u, orders o WHERE u.id = o.uid");
    REQUIRE(rs.rows.size() == 3);
    int alice = 0, bob = 0;
    for (const auto& r : rs.rows) {
        const auto& n = std::get<std::string>(r[0]);
        if (n == "Alice") ++alice;
        if (n == "Bob") ++bob;
    }
    REQUIRE(alice == 2);
    REQUIRE(bob == 1);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("Join: join feeding an aggregate (GROUP BY)", "[join]") {
    auto path = TempDbPath("join_agg");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE sales (pid INT, amount BIGINT)");
    db->Execute("CREATE TABLE parts (id INT, brand VARCHAR(8))");
    db->Execute("INSERT INTO parts VALUES (1, 'ACME')");
    db->Execute("INSERT INTO parts VALUES (2, 'GLOBEX')");
    for (int i = 0; i < 100; ++i) {
        db->Execute("INSERT INTO sales VALUES (" + std::to_string((i % 2) + 1) + ", " +
                    std::to_string(i + 1) + ")");
    }
    auto rs = db->Execute(
        "SELECT p.brand, SUM(s.amount) AS total FROM sales s, parts p "
        "WHERE s.pid = p.id GROUP BY p.brand ORDER BY brand ASC");
    REQUIRE(rs.rows.size() == 2);
    // ACME = odd-indexed sums, GLOBEX = even; both should be > 0.
    REQUIRE(AsInt(rs.rows[0][1]) > 0);
    REQUIRE(AsInt(rs.rows[1][1]) > 0);
    // Sum of both totals == sum(1..100) = 5050.
    REQUIRE(AsInt(rs.rows[0][1]) + AsInt(rs.rows[1][1]) == 5050);
    db->Close();
    std::filesystem::remove(path);
}

TEST_CASE("Join: empty build side yields no rows", "[join]") {
    auto path = TempDbPath("join_empty");
    auto db = DB::Open(path);
    db->Execute("CREATE TABLE a (id INT)");
    db->Execute("CREATE TABLE b (id INT)");
    db->Execute("INSERT INTO a VALUES (1)");
    db->Execute("INSERT INTO a VALUES (2)");
    auto rs = db->Execute("SELECT a.id FROM a a, b b WHERE a.id = b.id");
    REQUIRE(rs.rows.empty());
    db->Close();
    std::filesystem::remove(path);
}
