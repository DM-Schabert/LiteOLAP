#include "sql/lexer.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace liteolap::sql {

namespace {

const std::unordered_set<std::string>& Keywords() {
    static const std::unordered_set<std::string> k = {
        "SELECT",  "FROM",    "WHERE",  "INSERT", "INTO",     "VALUES",  "CREATE",
        "TABLE",   "DROP",    "TRUNCATE","AND",   "OR",       "NOT",     "ORDER",
        "BY",      "ASC",     "DESC",   "LIMIT",  "GROUP",    "HAVING",  "AS",
        "BETWEEN", "IN",      "INT",    "BIGINT", "FLOAT",    "VARCHAR", "NULL",
        "COUNT",   "SUM",     "AVG",    "MIN",    "MAX",      "TRUE",    "FALSE",
    };
    return k;
}

std::string ToUpper(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return std::toupper(c); });
    return o;
}

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentCont(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

bool IsKeyword(std::string_view upper) noexcept {
    return Keywords().count(std::string(upper)) != 0;
}

std::vector<Token> Tokenize(std::string_view sql) {
    std::vector<Token> out;
    std::size_t i = 0;
    const std::size_t n = sql.size();
    auto err = [](const std::string& m) { return std::runtime_error("lexer: " + m); };

    while (i < n) {
        const char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') ++i;
            continue;
        }
        if (c == '\'') {
            ++i;
            std::string s;
            bool closed = false;
            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        s.push_back('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    closed = true;
                    break;
                }
                s.push_back(sql[i++]);
            }
            if (!closed) throw err("unterminated string literal");
            out.push_back(Token{TokenKind::kString, std::move(s), 0, 0.0});
            continue;
        }
        // A leading '-' starts a negative literal only where a value is not
        // already on the left (e.g. after '=', '(', ','); otherwise it is the
        // binary subtraction operator handled by the punct branch below.
        auto prev_is_value = [&]() {
            if (out.empty()) return false;
            const Token& p = out.back();
            return p.kind == TokenKind::kIdentifier || p.kind == TokenKind::kInteger ||
                   p.kind == TokenKind::kFloat || p.kind == TokenKind::kString ||
                   (p.kind == TokenKind::kPunct && p.text == ")");
        };
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])) &&
             !prev_is_value())) {
            const std::size_t start = i;
            if (c == '-') ++i;
            while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
            bool is_float = false;
            if (i < n && sql[i] == '.') {
                is_float = true;
                ++i;
                while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
            }
            const std::string text(sql.substr(start, i - start));
            try {
                if (is_float) {
                    out.push_back(Token{TokenKind::kFloat, text, 0, std::stod(text)});
                } else {
                    out.push_back(Token{TokenKind::kInteger, text, std::stoll(text), 0.0});
                }
            } catch (const std::exception&) {
                throw err("invalid numeric literal: " + text);
            }
            continue;
        }
        if (IsIdentStart(c)) {
            const std::size_t start = i;
            while (i < n && IsIdentCont(sql[i])) ++i;
            std::string text(sql.substr(start, i - start));
            std::string upper = ToUpper(text);
            if (Keywords().count(upper)) {
                out.push_back(Token{TokenKind::kKeyword, std::move(upper), 0, 0.0});
            } else {
                out.push_back(Token{TokenKind::kIdentifier, std::move(text), 0, 0.0});
            }
            continue;
        }
        auto two = [&](const char* p) {
            return i + 1 < n && sql[i] == p[0] && sql[i + 1] == p[1];
        };
        if (two("!=")) {
            out.push_back(Token{TokenKind::kPunct, "!=", 0, 0.0});
            i += 2;
            continue;
        }
        if (two("<=")) {
            out.push_back(Token{TokenKind::kPunct, "<=", 0, 0.0});
            i += 2;
            continue;
        }
        if (two(">=")) {
            out.push_back(Token{TokenKind::kPunct, ">=", 0, 0.0});
            i += 2;
            continue;
        }
        if (two("<>")) {
            out.push_back(Token{TokenKind::kPunct, "!=", 0, 0.0});
            i += 2;
            continue;
        }
        if (std::string("(),;*=<>.+-/").find(c) != std::string::npos) {
            out.push_back(Token{TokenKind::kPunct, std::string(1, c), 0, 0.0});
            ++i;
            continue;
        }
        throw err(std::string("unexpected character '") + c + "'");
    }
    out.push_back(Token{TokenKind::kEof, "", 0, 0.0});
    return out;
}

}  // namespace liteolap::sql
