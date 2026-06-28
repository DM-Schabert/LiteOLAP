#include "vector/data_chunk.h"

namespace liteolap {

void DataChunk::Initialize(const std::vector<ColumnType>& types) {
    types_ = types;
    columns_.clear();
    columns_.reserve(types.size());
    for (auto t : types) {
        columns_.push_back(std::make_unique<Vector>(t));
    }
    cardinality_ = 0;
}

void DataChunk::Reset() {
    for (auto& c : columns_) c->Clear();
    cardinality_ = 0;
}

}  // namespace liteolap
