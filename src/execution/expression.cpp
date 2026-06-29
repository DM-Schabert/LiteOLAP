#include "execution/expression.h"

#include <stdexcept>

namespace liteolap {

std::unique_ptr<BoundExpr> BoundExpr::Column(std::size_t idx, ColumnType type) {
    auto e = std::make_unique<BoundExpr>();
    e->op = BoundOp::kColumn;
    e->col_index = idx;
    e->col_type = type;
    return e;
}

std::unique_ptr<BoundExpr> BoundExpr::Literal(Value v) {
    auto e = std::make_unique<BoundExpr>();
    e->op = BoundOp::kLiteral;
    e->literal = std::move(v);
    return e;
}

std::unique_ptr<BoundExpr> BoundExpr::Binary(BoundOp op, std::unique_ptr<BoundExpr> l,
                                             std::unique_ptr<BoundExpr> r) {
    auto e = std::make_unique<BoundExpr>();
    e->op = op;
    e->left = std::move(l);
    e->right = std::move(r);
    return e;
}

std::unique_ptr<BoundExpr> BoundExpr::Not(std::unique_ptr<BoundExpr> c) {
    auto e = std::make_unique<BoundExpr>();
    e->op = BoundOp::kNot;
    e->right = std::move(c);
    return e;
}

bool ValueTruthy(const Value& v) {
    if (IsNull(v)) return false;
    if (std::holds_alternative<std::int32_t>(v)) return std::get<std::int32_t>(v) != 0;
    if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v) != 0;
    if (std::holds_alternative<double>(v)) return std::get<double>(v) != 0.0;
    if (std::holds_alternative<std::string>(v)) return !std::get<std::string>(v).empty();
    return false;
}

namespace {

bool IsFloat(const Value& v) { return std::holds_alternative<double>(v); }
double AsDouble(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<std::int64_t>(v)) return static_cast<double>(std::get<std::int64_t>(v));
    return static_cast<double>(std::get<std::int32_t>(v));
}
std::int64_t AsI64(const Value& v) {
    if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v);
    return std::get<std::int32_t>(v);
}

/// Numeric arithmetic with null propagation. Promotes to double if either
/// operand is floating; integer division/modulo by zero yields NULL.
Value ArithResult(BoundOp op, const Value& l, const Value& r) {
    if (IsNull(l) || IsNull(r)) return Value{Null{}};
    if (IsFloat(l) || IsFloat(r)) {
        const double a = AsDouble(l), b = AsDouble(r);
        switch (op) {
            case BoundOp::kAdd: return Value{a + b};
            case BoundOp::kSub: return Value{a - b};
            case BoundOp::kMul: return Value{a * b};
            case BoundOp::kDiv: return b == 0.0 ? Value{Null{}} : Value{a / b};
            default: throw std::logic_error("ArithResult: not arithmetic");
        }
    }
    const std::int64_t a = AsI64(l), b = AsI64(r);
    switch (op) {
        case BoundOp::kAdd: return Value{a + b};
        case BoundOp::kSub: return Value{a - b};
        case BoundOp::kMul: return Value{a * b};
        case BoundOp::kDiv: return b == 0 ? Value{Null{}} : Value{a / b};
        default: throw std::logic_error("ArithResult: not arithmetic");
    }
}

Value CompareResult(BoundOp op, const Value& l, const Value& r) {
    if (IsNull(l) || IsNull(r)) return Value{std::int32_t{0}};
    const Ordering c = CompareValues(l, r);
    bool ok = false;
    switch (op) {
        case BoundOp::kEq:
            ok = c == Ordering::kEqual;
            break;
        case BoundOp::kNe:
            ok = c != Ordering::kEqual;
            break;
        case BoundOp::kLt:
            ok = c == Ordering::kLess;
            break;
        case BoundOp::kLe:
            ok = c != Ordering::kGreater;
            break;
        case BoundOp::kGt:
            ok = c == Ordering::kGreater;
            break;
        case BoundOp::kGe:
            ok = c != Ordering::kLess;
            break;
        default:
            throw std::logic_error("CompareResult: not a comparison");
    }
    return Value{std::int32_t{ok ? 1 : 0}};
}

}  // namespace

Value EvalExprRow(const BoundExpr& e, const DataChunk& chunk, std::size_t row) {
    switch (e.op) {
        case BoundOp::kColumn:
            return chunk.GetVector(e.col_index).GetValue(row);
        case BoundOp::kLiteral:
            return e.literal;
        case BoundOp::kNot:
            return Value{std::int32_t{ValueTruthy(EvalExprRow(*e.right, chunk, row)) ? 0 : 1}};
        case BoundOp::kAnd: {
            if (!ValueTruthy(EvalExprRow(*e.left, chunk, row))) return Value{std::int32_t{0}};
            return Value{std::int32_t{ValueTruthy(EvalExprRow(*e.right, chunk, row)) ? 1 : 0}};
        }
        case BoundOp::kOr: {
            if (ValueTruthy(EvalExprRow(*e.left, chunk, row))) return Value{std::int32_t{1}};
            return Value{std::int32_t{ValueTruthy(EvalExprRow(*e.right, chunk, row)) ? 1 : 0}};
        }
        case BoundOp::kAdd:
        case BoundOp::kSub:
        case BoundOp::kMul:
        case BoundOp::kDiv:
            return ArithResult(e.op, EvalExprRow(*e.left, chunk, row),
                               EvalExprRow(*e.right, chunk, row));
        default:
            return CompareResult(e.op, EvalExprRow(*e.left, chunk, row),
                                 EvalExprRow(*e.right, chunk, row));
    }
}

namespace {

/// Returns true if `e` is `column CMP literal` on a fixed-width column.
bool IsSimpleNumericCompare(const BoundExpr& e) {
    if (e.op != BoundOp::kEq && e.op != BoundOp::kNe && e.op != BoundOp::kLt &&
        e.op != BoundOp::kLe && e.op != BoundOp::kGt && e.op != BoundOp::kGe) {
        return false;
    }
    return e.left && e.left->op == BoundOp::kColumn && e.right &&
           e.right->op == BoundOp::kLiteral && e.left->col_type != ColumnType::kVarchar;
}

template <typename T>
void FastCompare(BoundOp op, const T* data, const std::vector<std::uint8_t>& validity, T lit,
                 std::size_t n, std::vector<std::uint8_t>& mask) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!validity[i]) {
            mask[i] = 0;
            continue;
        }
        const T v = data[i];
        bool ok = false;
        switch (op) {
            case BoundOp::kEq:
                ok = v == lit;
                break;
            case BoundOp::kNe:
                ok = v != lit;
                break;
            case BoundOp::kLt:
                ok = v < lit;
                break;
            case BoundOp::kLe:
                ok = v <= lit;
                break;
            case BoundOp::kGt:
                ok = v > lit;
                break;
            case BoundOp::kGe:
                ok = v >= lit;
                break;
            default:
                break;
        }
        mask[i] = ok ? 1 : 0;
    }
}

}  // namespace

void EvalPredicate(const BoundExpr& predicate, const DataChunk& chunk,
                   std::vector<std::uint8_t>& mask) {
    const std::size_t n = chunk.cardinality();
    mask.assign(n, 0);

    if (IsSimpleNumericCompare(predicate)) {
        const Vector& col = chunk.GetVector(predicate.left->col_index);
        const Value& lit = predicate.right->literal;
        switch (col.type()) {
            case ColumnType::kInt:
                FastCompare<std::int32_t>(predicate.op, col.GetData<std::int32_t>(),
                                          col.validity(),
                                          static_cast<std::int32_t>(std::get<std::int32_t>(lit)),
                                          n, mask);
                return;
            case ColumnType::kBigInt: {
                std::int64_t l = std::holds_alternative<std::int64_t>(lit)
                                     ? std::get<std::int64_t>(lit)
                                     : std::get<std::int32_t>(lit);
                FastCompare<std::int64_t>(predicate.op, col.GetData<std::int64_t>(),
                                          col.validity(), l, n, mask);
                return;
            }
            case ColumnType::kFloat: {
                double l = std::holds_alternative<double>(lit)
                               ? std::get<double>(lit)
                               : static_cast<double>(std::holds_alternative<std::int64_t>(lit)
                                                         ? std::get<std::int64_t>(lit)
                                                         : std::get<std::int32_t>(lit));
                FastCompare<double>(predicate.op, col.GetData<double>(), col.validity(), l, n,
                                    mask);
                return;
            }
            default:
                break;
        }
    }

    // Generic fallback.
    for (std::size_t i = 0; i < n; ++i) {
        mask[i] = ValueTruthy(EvalExprRow(predicate, chunk, i)) ? 1 : 0;
    }
}

}  // namespace liteolap
