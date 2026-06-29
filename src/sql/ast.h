#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/value.h"

namespace liteolap::sql {

enum class BinOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr };

enum class AggKind : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax };

struct Expr {
    virtual ~Expr() = default;
};

struct ColumnRefExpr : Expr {
    std::string table_alias;  ///< empty if unqualified
    std::string column_name;
};

struct LiteralExpr : Expr {
    Value value;
};

struct BinaryExpr : Expr {
    BinOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct NotExpr : Expr {
    std::unique_ptr<Expr> child;
};

/// `col IN (v1, v2, ...)` — desugared during planning into OR-of-equalities.
struct InExpr : Expr {
    std::unique_ptr<Expr> column;
    std::vector<Value> values;
};

/// An aggregate call in a SELECT list, e.g. SUM(amount).
struct AggregateExpr : Expr {
    AggKind kind;
    std::unique_ptr<Expr> argument;  ///< nullptr for COUNT(*)
};

struct ColumnDef {
    std::string name;
    ColumnType type;
    std::uint16_t varchar_len;
};

struct TableRef {
    std::string name;
    std::string alias;  ///< defaults to name if omitted
};

/// One projection item: either a scalar column or an aggregate, plus an
/// optional output alias (`... AS total`).
struct SelectItem {
    std::unique_ptr<Expr> expr;  ///< ColumnRefExpr or AggregateExpr
    std::string output_alias;    ///< empty -> derived name
};

struct OrderBy {
    std::string output_name;  ///< references a SELECT output column
    bool descending{false};
};

struct Statement {
    virtual ~Statement() = default;
};

struct CreateTableStmt : Statement {
    std::string table_name;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt : Statement {
    std::string table_name;
};

struct TruncateStmt : Statement {
    std::string table_name;
};

struct InsertStmt : Statement {
    std::string table_name;
    std::vector<std::vector<Value>> rows;
};

struct SelectStmt : Statement {
    bool select_star{false};
    std::vector<SelectItem> items;
    std::vector<TableRef> tables;
    std::unique_ptr<Expr> where;     ///< nullable
    std::vector<std::unique_ptr<ColumnRefExpr>> group_by;
    std::unique_ptr<Expr> having;    ///< nullable
    std::optional<OrderBy> order_by;
    std::optional<std::size_t> limit;

    bool HasAggregates() const;  ///< true if any item is an AggregateExpr
};

}  // namespace liteolap::sql
