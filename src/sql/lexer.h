#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace liteolap::sql {

enum class TokenKind : std::uint8_t {
    kKeyword,
    kIdentifier,
    kInteger,
    kFloat,
    kString,
    kPunct,
    kEof,
};

struct Token {
    TokenKind kind;
    std::string text;  ///< upper-cased for keywords; original for identifiers
    std::int64_t int_value{0};
    double float_value{0.0};
};

/// Tokenizes a SQL string, terminated by an EOF token. Throws on malformed
/// input (unterminated string, bad number, unexpected char).
std::vector<Token> Tokenize(std::string_view sql);

bool IsKeyword(std::string_view upper) noexcept;

}  // namespace liteolap::sql
