#include "catalog/schema.h"

#include <cstring>
#include <stdexcept>

namespace liteolap {

namespace {
template <typename T>
void Put(std::vector<std::byte>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
template <typename T>
T Get(const std::byte*& cur, const std::byte* end) {
    if (cur + sizeof(T) > end) throw std::runtime_error("Schema: truncated");
    T v{};
    std::memcpy(&v, cur, sizeof(T));
    cur += sizeof(T);
    return v;
}
}  // namespace

Schema::Schema(std::vector<Column> columns) : columns_(std::move(columns)) {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        columns_[i].column_index = static_cast<std::uint16_t>(i);
        if (columns_[i].name.size() > kMaxColumnNameLen) {
            throw std::invalid_argument("Schema: column name too long");
        }
    }
}

std::optional<std::size_t> Schema::FindColumn(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) return i;
    }
    return std::nullopt;
}

std::vector<ColumnType> Schema::Types() const {
    std::vector<ColumnType> t;
    t.reserve(columns_.size());
    for (const auto& c : columns_) t.push_back(c.type);
    return t;
}

std::vector<std::byte> Schema::Serialize() const {
    std::vector<std::byte> out;
    Put(out, static_cast<std::uint16_t>(columns_.size()));
    for (const auto& c : columns_) {
        Put(out, static_cast<std::uint8_t>(c.name.size()));
        const auto* p = reinterpret_cast<const std::byte*>(c.name.data());
        out.insert(out.end(), p, p + c.name.size());
        Put(out, static_cast<std::uint8_t>(c.type));
        Put(out, c.varchar_len);
    }
    return out;
}

Schema Schema::Deserialize(const std::byte* data, std::size_t size, std::size_t& consumed) {
    const std::byte* cur = data;
    const std::byte* end = data + size;
    const auto n = Get<std::uint16_t>(cur, end);
    std::vector<Column> cols;
    cols.reserve(n);
    for (std::uint16_t i = 0; i < n; ++i) {
        const auto len = Get<std::uint8_t>(cur, end);
        if (cur + len > end) throw std::runtime_error("Schema: truncated name");
        Column c;
        c.name.assign(reinterpret_cast<const char*>(cur), len);
        cur += len;
        c.type = static_cast<ColumnType>(Get<std::uint8_t>(cur, end));
        c.varchar_len = Get<std::uint16_t>(cur, end);
        c.column_index = i;
        cols.push_back(std::move(c));
    }
    consumed = static_cast<std::size_t>(cur - data);
    return Schema(std::move(cols));
}

bool Schema::operator==(const Schema& o) const noexcept {
    if (columns_.size() != o.columns_.size()) return false;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name != o.columns_[i].name || columns_[i].type != o.columns_[i].type ||
            columns_[i].varchar_len != o.columns_[i].varchar_len) {
            return false;
        }
    }
    return true;
}

}  // namespace liteolap
