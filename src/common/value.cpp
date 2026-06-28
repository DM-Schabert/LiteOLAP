#include "common/value.h"

#include <stdexcept>

namespace liteolap {

namespace {

template <typename A, typename B>
Ordering CompareArith(A a, B b) {
    if constexpr (std::is_same_v<A, B>) {
        if (a < b) return Ordering::kLess;
        if (a > b) return Ordering::kGreater;
        return Ordering::kEqual;
    } else {
        const auto da = static_cast<double>(a);
        const auto db = static_cast<double>(b);
        if (da < db) return Ordering::kLess;
        if (da > db) return Ordering::kGreater;
        return Ordering::kEqual;
    }
}

}  // namespace

Ordering CompareValues(const Value& a, const Value& b) {
    if (std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(b)) {
        if (!std::holds_alternative<std::string>(a) || !std::holds_alternative<std::string>(b)) {
            throw std::invalid_argument("CompareValues: cannot mix VARCHAR with numeric");
        }
        const auto& sa = std::get<std::string>(a);
        const auto& sb = std::get<std::string>(b);
        if (sa < sb) return Ordering::kLess;
        if (sa > sb) return Ordering::kGreater;
        return Ordering::kEqual;
    }
    return std::visit(
        [](auto av, auto bv) -> Ordering {
            using A = decltype(av);
            using B = decltype(bv);
            if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
                return CompareArith(av, bv);
            } else {
                throw std::invalid_argument("CompareValues: unsupported value kind");
            }
        },
        a, b);
}

std::string ValueToString(const Value& v) {
    return std::visit(
        [](auto&& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Null>) {
                return "NULL";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return x;
            } else {
                return std::to_string(x);
            }
        },
        v);
}

}  // namespace liteolap
