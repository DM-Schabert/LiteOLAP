#include <catch2/catch_test_macros.hpp>

#include "sql/parser.h"

using namespace liteolap;
using namespace liteolap::sql;

TEST_CASE("Parser: CREATE TABLE all types", "[parser]") {
    auto s = Parse("CREATE TABLE t (a INT, b BIGINT, c FLOAT, d VARCHAR(32))");
    auto* ct = dynamic_cast<CreateTableStmt*>(s.get());
    REQUIRE(ct);
    REQUIRE(ct->columns.size() == 4);
    REQUIRE(ct->columns[3].type == ColumnType::kVarchar);
    REQUIRE(ct->columns[3].varchar_len == 32);
}

TEST_CASE("Parser: aggregates with GROUP BY and HAVING", "[parser]") {
    auto s = Parse(
        "SELECT region, COUNT(*), SUM(amount) AS total FROM sales "
        "GROUP BY region HAVING SUM(amount) > 1000 ORDER BY total DESC LIMIT 5");
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    REQUIRE(sel);
    REQUIRE(sel->HasAggregates());
    REQUIRE(sel->items.size() == 3);
    REQUIRE(sel->group_by.size() == 1);
    REQUIRE(sel->having != nullptr);
    REQUIRE(sel->order_by.has_value());
    REQUIRE(sel->order_by->descending);
    REQUIRE(sel->limit.has_value());
    REQUIRE(*sel->limit == 5);
    // SUM(amount) AS total
    REQUIRE(sel->items[2].output_alias == "total");
}

TEST_CASE("Parser: BETWEEN desugars to AND of comparisons", "[parser]") {
    auto s = Parse("SELECT * FROM t WHERE x BETWEEN 10 AND 20");
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    REQUIRE(sel);
    auto* both = dynamic_cast<BinaryExpr*>(sel->where.get());
    REQUIRE(both);
    REQUIRE(both->op == BinOp::kAnd);
    REQUIRE(dynamic_cast<BinaryExpr*>(both->left.get())->op == BinOp::kGe);
    REQUIRE(dynamic_cast<BinaryExpr*>(both->right.get())->op == BinOp::kLe);
}

TEST_CASE("Parser: IN list", "[parser]") {
    auto s = Parse("SELECT * FROM t WHERE region IN ('EU', 'US', 'APAC')");
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    auto* in = dynamic_cast<InExpr*>(sel->where.get());
    REQUIRE(in);
    REQUIRE(in->values.size() == 3);
}

TEST_CASE("Parser: two-table join", "[parser]") {
    auto s = Parse("SELECT s.region, p.category FROM sales s, parts p WHERE s.pid = p.id");
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    REQUIRE(sel->tables.size() == 2);
    REQUIRE(sel->tables[0].alias == "s");
    REQUIRE(sel->tables[1].alias == "p");
}

TEST_CASE("Parser: TRUNCATE and DROP", "[parser]") {
    REQUIRE(dynamic_cast<TruncateStmt*>(Parse("TRUNCATE TABLE t").get()));
    REQUIRE(dynamic_cast<DropTableStmt*>(Parse("DROP TABLE t").get()));
}

TEST_CASE("Parser: syntax errors throw", "[parser]") {
    REQUIRE_THROWS_AS(Parse("SELECT FROM t"), std::runtime_error);
    REQUIRE_THROWS_AS(Parse("CREATE TABLE t"), std::runtime_error);
    REQUIRE_THROWS_AS(Parse("SELECT * FROM"), std::runtime_error);
}
