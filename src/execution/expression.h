#pragma once

#include <memory>
#include <vector>

#include "common/value.h"
#include "vector/data_chunk.h"

namespace liteolap {

/// Operator tag for a bound (column-index-resolved) expression node.
enum class BoundOp : std::uint8_t {
    kColumn,
    kLiteral,
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
    kAnd,
    kOr,
    kNot,
    kAdd,
    kSub,
    kMul,
    kDiv,
};

/**
 * An expression whose column references have been resolved to positional
 * indices into the operating DataChunk. Produced by the planner from the AST.
 * Evaluated per row; the columnar batch it runs over is the unit of
 * vectorization.
 */
struct BoundExpr {
    BoundOp op;
    std::size_t col_index{0};        ///< for kColumn
    ColumnType col_type{ColumnType::kInt};
    Value literal;                   ///< for kLiteral
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;  ///< also "child" for kNot

    static std::unique_ptr<BoundExpr> Column(std::size_t idx, ColumnType type);
    static std::unique_ptr<BoundExpr> Literal(Value v);
    static std::unique_ptr<BoundExpr> Binary(BoundOp op, std::unique_ptr<BoundExpr> l,
                                             std::unique_ptr<BoundExpr> r);
    static std::unique_ptr<BoundExpr> Not(std::unique_ptr<BoundExpr> c);
};

/// Evaluates the expression for one row of `chunk`, returning a scalar Value.
Value EvalExprRow(const BoundExpr& e, const DataChunk& chunk, std::size_t row);

/// Truthiness used by WHERE/HAVING. NULL is false.
bool ValueTruthy(const Value& v);

/// Writes a selection mask (1 = keep) for every row of `chunk` into `mask`.
/// Uses a typed fast path for `col CMP literal` on fixed-width columns and
/// falls back to per-row evaluation otherwise.
void EvalPredicate(const BoundExpr& predicate, const DataChunk& chunk,
                   std::vector<std::uint8_t>& mask);

}  // namespace liteolap
