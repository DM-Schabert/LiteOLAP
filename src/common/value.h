#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace liteolap {

/// The four scalar types LiteOLAP supports. Disk byte values are stable;
/// do not renumber once data has been persisted.
enum class ColumnType : std::uint8_t {
    kInt = 1,      ///< int32_t
    kBigInt = 2,   ///< int64_t
    kFloat = 3,    ///< IEEE-754 double
    kVarchar = 4,  ///< UTF-8 bytes
};

/// Byte width of a fixed-width type. Undefined for kVarchar.
inline std::size_t FixedWidth(ColumnType t) {
    switch (t) {
        case ColumnType::kInt:
            return sizeof(std::int32_t);
        case ColumnType::kBigInt:
            return sizeof(std::int64_t);
        case ColumnType::kFloat:
            return sizeof(double);
        case ColumnType::kVarchar:
            return 0;
    }
    return 0;
}

inline const char* TypeName(ColumnType t) {
    switch (t) {
        case ColumnType::kInt:
            return "INT";
        case ColumnType::kBigInt:
            return "BIGINT";
        case ColumnType::kFloat:
            return "FLOAT";
        case ColumnType::kVarchar:
            return "VARCHAR";
    }
    return "?";
}

/// Sentinel for SQL NULL within a `Value` variant.
struct Null {
    bool operator==(const Null&) const noexcept { return true; }
    bool operator!=(const Null&) const noexcept { return false; }
};

/// Runtime representation of a single scalar value.
using Value = std::variant<Null, std::int32_t, std::int64_t, double, std::string>;

inline bool IsNull(const Value& v) { return std::holds_alternative<Null>(v); }

/// Three-way comparison result.
enum class Ordering : int { kLess = -1, kEqual = 0, kGreater = 1 };

/// Three-way comparison of two non-null values. NULL handling is the
/// caller's responsibility. Mixed numeric types compare numerically;
/// VARCHAR compares lexicographically and must not be mixed with numerics.
Ordering CompareValues(const Value& a, const Value& b);

/// Renders a value as text (for ResultSet printing / debugging).
std::string ValueToString(const Value& v);

}  // namespace liteolap
