#pragma once

#include <memory>

#include "catalog/catalog.h"
#include "execution/operator.h"
#include "sql/ast.h"
#include "storage/buffer_pool.h"

namespace liteolap {

/// Translates a parsed SELECT into a tree of vectorized operators. Performs
/// projection pushdown (scans touch only referenced columns), zone-map range
/// pushdown for single-table scans, and chooses a hash join for two-table
/// equi-joins. `catalog` and `bp` must outlive the returned operator.
std::unique_ptr<Operator> PlanSelect(const sql::SelectStmt& stmt, const Catalog& catalog,
                                     BufferPool& bp);

}  // namespace liteolap
