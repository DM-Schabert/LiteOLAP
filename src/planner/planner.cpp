#include "planner/planner.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "execution/column_scan.h"
#include "execution/expression.h"
#include "execution/filter.h"
#include "execution/hash_aggregate.h"
#include "execution/hash_join.h"
#include "execution/limit.h"
#include "execution/order_by.h"
#include "execution/projection.h"

namespace liteolap {

namespace {

[[noreturn]] void Fail(const std::string& m) { throw std::runtime_error("planner: " + m); }

bool EndsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

/// Resolves a (possibly qualified) column reference against a list of output
/// names of the form "alias.col" (or bare names for aggregate output).
std::size_t Resolve(const std::vector<std::string>& names, const std::string& alias,
                    const std::string& col) {
    if (!alias.empty()) {
        const std::string target = alias + "." + col;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == target) return i;
        Fail("unknown column: " + target);
    }
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == col || EndsWith(names[i], "." + col)) matches.push_back(i);
    }
    if (matches.empty()) Fail("unknown column: " + col);
    if (matches.size() > 1) Fail("ambiguous column: " + col);
    return matches[0];
}

const char* OpSym(sql::BinOp op) {
    switch (op) {
        case sql::BinOp::kEq: return "=";
        case sql::BinOp::kNe: return "!=";
        case sql::BinOp::kLt: return "<";
        case sql::BinOp::kLe: return "<=";
        case sql::BinOp::kGt: return ">";
        case sql::BinOp::kGe: return ">=";
        case sql::BinOp::kAnd: return "AND";
        case sql::BinOp::kOr: return "OR";
        case sql::BinOp::kAdd: return "+";
        case sql::BinOp::kSub: return "-";
        case sql::BinOp::kMul: return "*";
        case sql::BinOp::kDiv: return "/";
    }
    return "?";
}

/// Canonical textual form of an aggregate argument, used to name and dedup
/// aggregates. A bare column keeps its short name (no alias) for backward
/// compatibility; arithmetic is rendered fully parenthesized.
std::string ExprToString(const sql::Expr* e) {
    if (const auto* c = dynamic_cast<const sql::ColumnRefExpr*>(e)) return c->column_name;
    if (const auto* l = dynamic_cast<const sql::LiteralExpr*>(e)) return ValueToString(l->value);
    if (const auto* b = dynamic_cast<const sql::BinaryExpr*>(e))
        return "(" + ExprToString(b->left.get()) + " " + OpSym(b->op) + " " +
               ExprToString(b->right.get()) + ")";
    return "?";
}

std::string AggName(const sql::AggregateExpr& a) {
    auto arg = [&]() -> std::string { return ExprToString(a.argument.get()); };
    switch (a.kind) {
        case sql::AggKind::kCountStar:
            return "COUNT(*)";
        case sql::AggKind::kCount:
            return "COUNT(" + arg() + ")";
        case sql::AggKind::kSum:
            return "SUM(" + arg() + ")";
        case sql::AggKind::kAvg:
            return "AVG(" + arg() + ")";
        case sql::AggKind::kMin:
            return "MIN(" + arg() + ")";
        case sql::AggKind::kMax:
            return "MAX(" + arg() + ")";
    }
    return "?";
}

/// Infers the scalar type a bound expression evaluates to (used to pick
/// integer vs floating accumulators for SUM/AVG over arithmetic arguments).
ColumnType InferBoundType(const BoundExpr& e) {
    switch (e.op) {
        case BoundOp::kColumn:
            return e.col_type;
        case BoundOp::kLiteral:
            if (std::holds_alternative<double>(e.literal)) return ColumnType::kFloat;
            if (std::holds_alternative<std::int64_t>(e.literal)) return ColumnType::kBigInt;
            if (std::holds_alternative<std::string>(e.literal)) return ColumnType::kVarchar;
            return ColumnType::kInt;
        case BoundOp::kAdd:
        case BoundOp::kSub:
        case BoundOp::kMul:
        case BoundOp::kDiv: {
            const ColumnType lt = InferBoundType(*e.left);
            const ColumnType rt = InferBoundType(*e.right);
            return (lt == ColumnType::kFloat || rt == ColumnType::kFloat) ? ColumnType::kFloat
                                                                          : ColumnType::kBigInt;
        }
        default:
            return ColumnType::kInt;  // comparisons / logic produce 0/1
    }
}

BoundOp ToBoundOp(sql::BinOp op) {
    switch (op) {
        case sql::BinOp::kEq:
            return BoundOp::kEq;
        case sql::BinOp::kNe:
            return BoundOp::kNe;
        case sql::BinOp::kLt:
            return BoundOp::kLt;
        case sql::BinOp::kLe:
            return BoundOp::kLe;
        case sql::BinOp::kGt:
            return BoundOp::kGt;
        case sql::BinOp::kGe:
            return BoundOp::kGe;
        case sql::BinOp::kAnd:
            return BoundOp::kAnd;
        case sql::BinOp::kOr:
            return BoundOp::kOr;
        case sql::BinOp::kAdd:
            return BoundOp::kAdd;
        case sql::BinOp::kSub:
            return BoundOp::kSub;
        case sql::BinOp::kMul:
            return BoundOp::kMul;
        case sql::BinOp::kDiv:
            return BoundOp::kDiv;
    }
    return BoundOp::kEq;
}

/// Binds an AST expression to a BoundExpr against a (names, types) schema.
/// Handles column refs, literals, AND/OR/NOT, comparisons, IN (desugared to
/// OR-of-equalities) and aggregate references (resolved by canonical name,
/// for HAVING).
std::unique_ptr<BoundExpr> Bind(const sql::Expr* e, const std::vector<std::string>& names,
                                const std::vector<ColumnType>& types) {
    if (const auto* c = dynamic_cast<const sql::ColumnRefExpr*>(e)) {
        const std::size_t idx = Resolve(names, c->table_alias, c->column_name);
        return BoundExpr::Column(idx, types[idx]);
    }
    if (const auto* l = dynamic_cast<const sql::LiteralExpr*>(e)) {
        return BoundExpr::Literal(l->value);
    }
    if (const auto* n = dynamic_cast<const sql::NotExpr*>(e)) {
        return BoundExpr::Not(Bind(n->child.get(), names, types));
    }
    if (const auto* a = dynamic_cast<const sql::AggregateExpr*>(e)) {
        const std::string nm = AggName(*a);
        const std::size_t idx = Resolve(names, "", nm);
        return BoundExpr::Column(idx, types[idx]);
    }
    if (const auto* in = dynamic_cast<const sql::InExpr*>(e)) {
        const auto* col = dynamic_cast<const sql::ColumnRefExpr*>(in->column.get());
        if (!col) Fail("IN requires a column on the left");
        std::unique_ptr<BoundExpr> chain;
        for (const auto& v : in->values) {
            const std::size_t idx = Resolve(names, col->table_alias, col->column_name);
            auto eq = BoundExpr::Binary(BoundOp::kEq, BoundExpr::Column(idx, types[idx]),
                                        BoundExpr::Literal(v));
            chain = chain ? BoundExpr::Binary(BoundOp::kOr, std::move(chain), std::move(eq))
                          : std::move(eq);
        }
        if (!chain) Fail("empty IN list");
        return chain;
    }
    if (const auto* b = dynamic_cast<const sql::BinaryExpr*>(e)) {
        return BoundExpr::Binary(ToBoundOp(b->op), Bind(b->left.get(), names, types),
                                 Bind(b->right.get(), names, types));
    }
    Fail("unsupported expression in predicate");
}

/// Splits a WHERE tree into top-level AND conjuncts.
void Flatten(const sql::Expr* e, std::vector<const sql::Expr*>& out) {
    const auto* b = dynamic_cast<const sql::BinaryExpr*>(e);
    if (b && b->op == sql::BinOp::kAnd) {
        Flatten(b->left.get(), out);
        Flatten(b->right.get(), out);
    } else {
        out.push_back(e);
    }
}

/// Collects every column reference, invoking cb(alias, col).
void WalkRefs(const sql::Expr* e, const std::function<void(const std::string&, const std::string&)>& cb) {
    if (!e) return;
    if (const auto* c = dynamic_cast<const sql::ColumnRefExpr*>(e)) {
        cb(c->table_alias, c->column_name);
    } else if (const auto* n = dynamic_cast<const sql::NotExpr*>(e)) {
        WalkRefs(n->child.get(), cb);
    } else if (const auto* a = dynamic_cast<const sql::AggregateExpr*>(e)) {
        WalkRefs(a->argument.get(), cb);
    } else if (const auto* in = dynamic_cast<const sql::InExpr*>(e)) {
        WalkRefs(in->column.get(), cb);
    } else if (const auto* b = dynamic_cast<const sql::BinaryExpr*>(e)) {
        WalkRefs(b->left.get(), cb);
        WalkRefs(b->right.get(), cb);
    }
}

struct ScanInfo {
    const TableMeta* table;
    std::string alias;
    std::vector<std::size_t> col_indices;  ///< table column indices, sorted
    std::map<std::string, std::size_t> name_to_scanpos;  ///< colname -> position in scan output
};

}  // namespace

std::unique_ptr<Operator> PlanSelect(const sql::SelectStmt& stmt, const Catalog& catalog,
                                     BufferPool& bp) {
    if (stmt.tables.empty()) Fail("SELECT without FROM is not supported");
    if (stmt.tables.size() > 2) Fail("more than two-table joins are not supported");

    // Resolve FROM tables.
    std::vector<ScanInfo> scans;
    for (const auto& tr : stmt.tables) {
        const TableMeta* meta = catalog.GetTable(tr.name);
        if (!meta) Fail("unknown table: " + tr.name);
        ScanInfo si;
        si.table = meta;
        si.alias = tr.alias.empty() ? tr.name : tr.alias;
        scans.push_back(std::move(si));
    }

    auto owning_alias = [&](const std::string& col) -> std::string {
        std::string found;
        for (const auto& s : scans) {
            if (s.table->schema.FindColumn(col)) {
                if (!found.empty()) Fail("ambiguous column: " + col);
                found = s.alias;
            }
        }
        if (found.empty()) Fail("unknown column: " + col);
        return found;
    };

    // Collect needed columns per alias.
    std::map<std::string, std::set<std::string>> needed;
    auto add_ref = [&](const std::string& alias, const std::string& col) {
        const std::string a = alias.empty() ? owning_alias(col) : alias;
        needed[a].insert(col);
    };

    if (stmt.select_star) {
        for (const auto& s : scans)
            for (const auto& c : s.table->schema.Columns()) needed[s.alias].insert(c.name);
    } else {
        for (const auto& item : stmt.items) WalkRefs(item.expr.get(), add_ref);
    }
    WalkRefs(stmt.where.get(), add_ref);
    WalkRefs(stmt.having.get(), add_ref);
    for (const auto& g : stmt.group_by) add_ref(g->table_alias, g->column_name);

    const bool aggregated = stmt.HasAggregates() || !stmt.group_by.empty();

    // In a non-aggregate query, ORDER BY may reference a base column that is
    // not in the SELECT list; ensure it is scanned. (If the name is a SELECT
    // alias instead, owning_alias throws and we resolve it post-projection.)
    if (stmt.order_by && !aggregated) {
        try {
            add_ref("", stmt.order_by->output_name);
        } catch (...) {
        }
    }

    // Extract a single equi-join predicate (two-table case) from WHERE.
    std::vector<const sql::Expr*> conjuncts;
    if (stmt.where) Flatten(stmt.where.get(), conjuncts);

    const sql::BinaryExpr* join_pred = nullptr;
    if (scans.size() == 2) {
        for (const auto* c : conjuncts) {
            const auto* b = dynamic_cast<const sql::BinaryExpr*>(c);
            if (!b || b->op != sql::BinOp::kEq) continue;
            const auto* lc = dynamic_cast<const sql::ColumnRefExpr*>(b->left.get());
            const auto* rc = dynamic_cast<const sql::ColumnRefExpr*>(b->right.get());
            if (lc && rc) {
                join_pred = b;
                break;
            }
        }
        if (!join_pred) Fail("two-table query requires an equi-join predicate");
        const auto* lc = dynamic_cast<const sql::ColumnRefExpr*>(join_pred->left.get());
        const auto* rc = dynamic_cast<const sql::ColumnRefExpr*>(join_pred->right.get());
        add_ref(lc->table_alias, lc->column_name);
        add_ref(rc->table_alias, rc->column_name);
    }

    // Finalize each scan's column list (sorted by table column index).
    for (auto& s : scans) {
        auto& set = needed[s.alias];
        if (set.empty()) {
            // A table referenced only via SELECT * with no columns is unusual;
            // fall back to its first column so the scan still drives rows.
            set.insert(s.table->schema.GetColumn(0).name);
        }
        std::vector<std::size_t> idxs;
        for (const auto& cn : set) idxs.push_back(*s.table->schema.FindColumn(cn));
        std::sort(idxs.begin(), idxs.end());
        s.col_indices = idxs;
        for (std::size_t pos = 0; pos < idxs.size(); ++pos) {
            s.name_to_scanpos[s.table->schema.GetColumn(idxs[pos]).name] = pos;
        }
    }

    // Build scan operators.
    std::vector<std::unique_ptr<ColumnScan>> scan_ops;
    for (auto& s : scans) {
        scan_ops.push_back(
            std::make_unique<ColumnScan>(bp, *s.table, s.alias, s.col_indices));
    }

    // Combined input schema (names/types) for binding the residual filter.
    std::vector<std::string> combined_names;
    std::vector<ColumnType> combined_types;
    std::vector<std::size_t> scan_offset(scans.size(), 0);
    for (std::size_t si = 0; si < scans.size(); ++si) {
        scan_offset[si] = combined_names.size();
        for (const auto& n : scan_ops[si]->output_names()) combined_names.push_back(n);
        for (auto t : scan_ops[si]->output_types()) combined_types.push_back(t);
    }

    // Zone-map pushdown (single-table only): scan an int column with a bound.
    if (scans.size() == 1) {
        for (const auto* c : conjuncts) {
            const auto* b = dynamic_cast<const sql::BinaryExpr*>(c);
            if (!b) continue;
            const auto* col = dynamic_cast<const sql::ColumnRefExpr*>(b->left.get());
            const auto* lit = dynamic_cast<const sql::LiteralExpr*>(b->right.get());
            if (!col || !lit) continue;
            auto it = scans[0].name_to_scanpos.find(col->column_name);
            if (it == scans[0].name_to_scanpos.end()) continue;
            const ColumnType ct = scans[0].table->schema.GetColumn(
                                       *scans[0].table->schema.FindColumn(col->column_name)).type;
            if (ct != ColumnType::kInt && ct != ColumnType::kBigInt) continue;
            if (!std::holds_alternative<std::int32_t>(lit->value) &&
                !std::holds_alternative<std::int64_t>(lit->value))
                continue;
            std::int64_t v = std::holds_alternative<std::int64_t>(lit->value)
                                 ? std::get<std::int64_t>(lit->value)
                                 : std::get<std::int32_t>(lit->value);
            std::int64_t lo = std::numeric_limits<std::int64_t>::min();
            std::int64_t hi = std::numeric_limits<std::int64_t>::max();
            switch (b->op) {
                case sql::BinOp::kEq:
                    lo = hi = v;
                    break;
                case sql::BinOp::kGe:
                    lo = v;
                    break;
                case sql::BinOp::kGt:
                    lo = v + 1;
                    break;
                case sql::BinOp::kLe:
                    hi = v;
                    break;
                case sql::BinOp::kLt:
                    hi = v - 1;
                    break;
                default:
                    continue;
            }
            scan_ops[0]->SetZoneFilter(it->second, lo, hi);
            break;  // one pushdown is enough for the demo
        }
    }

    // Assemble the bottom of the plan: scans + optional join.
    std::unique_ptr<Operator> root;
    if (scans.size() == 1) {
        root = std::move(scan_ops[0]);
    } else {
        const auto* lc = dynamic_cast<const sql::ColumnRefExpr*>(join_pred->left.get());
        const auto* rc = dynamic_cast<const sql::ColumnRefExpr*>(join_pred->right.get());
        // Figure out which column ref belongs to which scan.
        auto pos_in_scan = [&](std::size_t si, const sql::ColumnRefExpr* c) -> std::size_t {
            return scan_offset[si] + scans[si].name_to_scanpos.at(c->column_name);
        };
        // Match aliases to scan order.
        std::size_t left_scan = 0, right_scan = 1;
        if (lc->table_alias == scans[1].alias || rc->table_alias == scans[0].alias) {
            std::swap(left_scan, right_scan);
        }
        const sql::ColumnRefExpr* left_col = (lc->table_alias == scans[left_scan].alias) ? lc : rc;
        const sql::ColumnRefExpr* right_col = (left_col == lc) ? rc : lc;
        const std::size_t left_key = scans[left_scan].name_to_scanpos.at(left_col->column_name);
        const std::size_t right_key = scans[right_scan].name_to_scanpos.at(right_col->column_name);
        (void)pos_in_scan;
        root = std::make_unique<HashJoin>(std::move(scan_ops[left_scan]),
                                          std::move(scan_ops[right_scan]), left_key, right_key);
    }

    // Residual WHERE filter (everything except the join predicate).
    std::vector<const sql::Expr*> residual;
    for (const auto* c : conjuncts)
        if (c != static_cast<const sql::Expr*>(join_pred)) residual.push_back(c);
    if (!residual.empty()) {
        std::unique_ptr<BoundExpr> pred;
        for (const auto* c : residual) {
            auto bound = Bind(c, combined_names, combined_types);
            pred = pred ? BoundExpr::Binary(BoundOp::kAnd, std::move(pred), std::move(bound))
                        : std::move(bound);
        }
        root = std::make_unique<Filter>(std::move(root), std::move(pred));
    }

    bool order_applied = false;

    if (aggregated) {
        // Group columns.
        std::vector<std::size_t> group_idx;
        std::vector<std::string> group_names;
        std::vector<ColumnType> group_types;
        for (const auto& g : stmt.group_by) {
            const std::size_t idx = Resolve(combined_names, g->table_alias, g->column_name);
            group_idx.push_back(idx);
            group_names.push_back(g->column_name);
            group_types.push_back(combined_types[idx]);
        }

        // Collect aggregates from SELECT items and HAVING (dedup by name).
        std::vector<AggSpec> specs;
        std::map<std::string, std::size_t> agg_pos;  // canonical name -> position
        auto add_agg = [&](const sql::AggregateExpr* a) {
            const std::string nm = AggName(*a);
            if (agg_pos.count(nm)) return;
            AggSpec spec;
            spec.kind = a->kind;
            spec.output_name = nm;
            if (a->kind == sql::AggKind::kCountStar) {
                spec.output_type = ColumnType::kBigInt;
            } else {
                spec.input_expr = Bind(a->argument.get(), combined_names, combined_types);
                spec.input_type = InferBoundType(*spec.input_expr);
                switch (a->kind) {
                    case sql::AggKind::kCount:
                        spec.output_type = ColumnType::kBigInt;
                        break;
                    case sql::AggKind::kSum:
                        spec.output_type = spec.input_type == ColumnType::kFloat
                                               ? ColumnType::kFloat
                                               : ColumnType::kBigInt;
                        break;
                    case sql::AggKind::kAvg:
                        spec.output_type = ColumnType::kFloat;
                        break;
                    case sql::AggKind::kMin:
                    case sql::AggKind::kMax:
                        spec.output_type = spec.input_type;
                        break;
                    default:
                        break;
                }
            }
            agg_pos.emplace(nm, specs.size());
            specs.push_back(std::move(spec));
        };

        std::function<void(const sql::Expr*)> collect = [&](const sql::Expr* e) {
            if (!e) return;
            if (const auto* a = dynamic_cast<const sql::AggregateExpr*>(e)) {
                add_agg(a);
            } else if (const auto* n = dynamic_cast<const sql::NotExpr*>(e)) {
                collect(n->child.get());
            } else if (const auto* b = dynamic_cast<const sql::BinaryExpr*>(e)) {
                collect(b->left.get());
                collect(b->right.get());
            }
        };
        for (const auto& item : stmt.items) collect(item.expr.get());
        collect(stmt.having.get());

        // Aggregate output schema.
        std::vector<std::string> agg_out_names = group_names;
        std::vector<ColumnType> agg_out_types = group_types;
        for (const auto& s : specs) {
            agg_out_names.push_back(s.output_name);
            agg_out_types.push_back(s.output_type);
        }

        root = std::make_unique<HashAggregate>(std::move(root), group_idx, group_names,
                                               group_types, specs);

        if (stmt.having) {
            auto pred = Bind(stmt.having.get(), agg_out_names, agg_out_types);
            root = std::make_unique<Filter>(std::move(root), std::move(pred));
        }

        // Final projection: select items from the aggregate output.
        std::vector<std::size_t> proj_idx;
        std::vector<std::string> proj_names;
        for (const auto& item : stmt.items) {
            std::size_t idx;
            std::string default_name;
            if (const auto* a = dynamic_cast<const sql::AggregateExpr*>(item.expr.get())) {
                default_name = AggName(*a);
                idx = Resolve(agg_out_names, "", default_name);
            } else if (const auto* c = dynamic_cast<const sql::ColumnRefExpr*>(item.expr.get())) {
                default_name = c->column_name;
                idx = Resolve(agg_out_names, "", c->column_name);
            } else {
                Fail("unsupported SELECT item in aggregate query");
            }
            proj_idx.push_back(idx);
            proj_names.push_back(item.output_alias.empty() ? default_name : item.output_alias);
        }
        root = std::make_unique<Projection>(std::move(root), proj_idx, proj_names);
    } else {
        // Non-aggregate projection.
        std::vector<std::size_t> proj_idx;
        std::vector<std::string> proj_names;
        if (stmt.select_star) {
            for (std::size_t i = 0; i < combined_names.size(); ++i) {
                proj_idx.push_back(i);
                // Bare column name for single-table star; qualified for joins.
                if (scans.size() == 1) {
                    const std::string& full = combined_names[i];
                    auto dot = full.find('.');
                    proj_names.push_back(dot == std::string::npos ? full : full.substr(dot + 1));
                } else {
                    proj_names.push_back(combined_names[i]);
                }
            }
        } else {
            for (const auto& item : stmt.items) {
                const auto* c = dynamic_cast<const sql::ColumnRefExpr*>(item.expr.get());
                if (!c) Fail("non-aggregate query: SELECT item must be a column");
                const std::size_t idx = Resolve(combined_names, c->table_alias, c->column_name);
                proj_idx.push_back(idx);
                proj_names.push_back(item.output_alias.empty() ? c->column_name
                                                               : item.output_alias);
            }
        }

        // ORDER BY in a non-aggregate query may target a base column not in
        // the SELECT list, so sort before projection in that case; otherwise
        // sort the projected output (supports `ORDER BY <select-alias>`).
        if (stmt.order_by) {
            const std::string& on = stmt.order_by->output_name;
            std::size_t pidx = proj_names.size();
            for (std::size_t i = 0; i < proj_names.size(); ++i)
                if (proj_names[i] == on) pidx = i;
            if (pidx == proj_names.size()) {
                const std::size_t cidx = Resolve(combined_names, "", on);
                root = std::make_unique<OrderBy>(std::move(root), cidx, stmt.order_by->descending);
                root = std::make_unique<Projection>(std::move(root), proj_idx, proj_names);
            } else {
                root = std::make_unique<Projection>(std::move(root), proj_idx, proj_names);
                root = std::make_unique<OrderBy>(std::move(root), pidx, stmt.order_by->descending);
            }
            order_applied = true;
        } else {
            root = std::make_unique<Projection>(std::move(root), proj_idx, proj_names);
        }
    }

    // Aggregate path: ORDER BY references a projection output column.
    if (stmt.order_by && !order_applied) {
        const std::size_t idx = Resolve(root->output_names(), "", stmt.order_by->output_name);
        root = std::make_unique<OrderBy>(std::move(root), idx, stmt.order_by->descending);
    }
    if (stmt.limit) {
        root = std::make_unique<Limit>(std::move(root), *stmt.limit);
    }
    return root;
}

}  // namespace liteolap
