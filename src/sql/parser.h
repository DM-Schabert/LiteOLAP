#pragma once

#include <memory>
#include <string_view>

#include "sql/ast.h"

namespace liteolap::sql {

/// Parses a single SQL statement. Throws std::runtime_error with a
/// descriptive message on any syntax error.
std::unique_ptr<Statement> Parse(std::string_view sql);

}  // namespace liteolap::sql
