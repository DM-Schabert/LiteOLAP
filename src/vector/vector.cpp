#include "vector/vector.h"

#include <cstring>
#include <stdexcept>

namespace liteolap {

Vector::Vector(ColumnType type) : type_(type) {
    if (type_ == ColumnType::kVarchar) {
        offsets_.push_back(0);
    }
}

void Vector::Clear() {
    size_ = 0;
    data_.clear();
    validity_.clear();
    strbytes_.clear();
    offsets_.clear();
    if (type_ == ColumnType::kVarchar) {
        offsets_.push_back(0);
    }
}

void Vector::AppendInt32(std::int32_t v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    data_.insert(data_.end(), p, p + sizeof(v));
    validity_.push_back(1);
    ++size_;
}

void Vector::AppendInt64(std::int64_t v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    data_.insert(data_.end(), p, p + sizeof(v));
    validity_.push_back(1);
    ++size_;
}

void Vector::AppendDouble(double v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    data_.insert(data_.end(), p, p + sizeof(v));
    validity_.push_back(1);
    ++size_;
}

void Vector::AppendString(std::string_view v) {
    const auto* p = reinterpret_cast<const std::byte*>(v.data());
    strbytes_.insert(strbytes_.end(), p, p + v.size());
    offsets_.push_back(static_cast<std::uint32_t>(strbytes_.size()));
    validity_.push_back(1);
    ++size_;
}

void Vector::AppendNull() {
    // Reserve a slot so positional indexing stays valid.
    switch (type_) {
        case ColumnType::kInt:
            data_.insert(data_.end(), sizeof(std::int32_t), std::byte{0});
            break;
        case ColumnType::kBigInt:
            data_.insert(data_.end(), sizeof(std::int64_t), std::byte{0});
            break;
        case ColumnType::kFloat:
            data_.insert(data_.end(), sizeof(double), std::byte{0});
            break;
        case ColumnType::kVarchar:
            offsets_.push_back(static_cast<std::uint32_t>(strbytes_.size()));
            break;
    }
    validity_.push_back(0);
    ++size_;
}

void Vector::AppendValue(const Value& v) {
    if (IsNull(v)) {
        AppendNull();
        return;
    }
    switch (type_) {
        case ColumnType::kInt:
            AppendInt32(std::get<std::int32_t>(v));
            break;
        case ColumnType::kBigInt:
            AppendInt64(std::get<std::int64_t>(v));
            break;
        case ColumnType::kFloat:
            AppendDouble(std::get<double>(v));
            break;
        case ColumnType::kVarchar:
            AppendString(std::get<std::string>(v));
            break;
    }
}

Value Vector::GetValue(std::size_t i) const {
    if (!IsValid(i)) return Value{Null{}};
    switch (type_) {
        case ColumnType::kInt:
            return Value{GetData<std::int32_t>()[i]};
        case ColumnType::kBigInt:
            return Value{GetData<std::int64_t>()[i]};
        case ColumnType::kFloat:
            return Value{GetData<double>()[i]};
        case ColumnType::kVarchar:
            return Value{std::string(GetStringView(i))};
    }
    return Value{Null{}};
}

void Vector::AppendFrom(const Vector& src, std::size_t src_idx) {
    if (!src.IsValid(src_idx)) {
        AppendNull();
        return;
    }
    switch (type_) {
        case ColumnType::kInt:
            AppendInt32(src.GetData<std::int32_t>()[src_idx]);
            break;
        case ColumnType::kBigInt:
            AppendInt64(src.GetData<std::int64_t>()[src_idx]);
            break;
        case ColumnType::kFloat:
            AppendDouble(src.GetData<double>()[src_idx]);
            break;
        case ColumnType::kVarchar:
            AppendString(src.GetStringView(src_idx));
            break;
    }
}

void Vector::ResizeFixed(std::size_t n) {
    if (type_ == ColumnType::kVarchar) {
        throw std::logic_error("Vector::ResizeFixed on VARCHAR");
    }
    data_.assign(n * FixedWidth(type_), std::byte{0});
    validity_.assign(n, 1);
    size_ = n;
}

}  // namespace liteolap
