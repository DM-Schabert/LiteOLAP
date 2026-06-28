#include <catch2/catch_test_macros.hpp>
#include <random>
#include <vector>

#include "compression/encoding.h"
#include "vector/vector.h"

using namespace liteolap;

namespace {

/// Round-trips `values` through a specific encoding and asserts equality.
void CheckRoundTrip(ColumnType type, const std::vector<Value>& values, Encoding enc) {
    EncodedChunk c = EncodeChunkWith(type, values, enc);
    Vector out(type);
    DecodeChunk(type, enc, static_cast<std::uint32_t>(values.size()), c.payload.data(),
                c.payload.size(), out);
    REQUIRE(out.size() == values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (IsNull(values[i])) {
            REQUIRE_FALSE(out.IsValid(i));
        } else {
            REQUIRE(out.GetValue(i) == values[i]);
        }
    }
}

}  // namespace

TEST_CASE("BitsRequired and PackBits round-trip", "[compression]") {
    REQUIRE(BitsRequired(0) == 1);
    REQUIRE(BitsRequired(1) == 1);
    REQUIRE(BitsRequired(7) == 3);
    REQUIRE(BitsRequired(255) == 8);

    std::vector<std::uint64_t> vals;
    std::mt19937_64 rng(1);
    for (int i = 0; i < 1000; ++i) vals.push_back(rng() & 0x1FF);  // 9-bit
    std::vector<std::byte> packed;
    PackBits(vals.data(), vals.size(), 9, packed);
    std::vector<std::uint64_t> back(vals.size());
    UnpackBits(packed.data(), vals.size(), 9, back.data());
    REQUIRE(back == vals);
}

TEST_CASE("All encodings round-trip every type incl nulls", "[compression]") {
    std::mt19937_64 rng(42);

    std::vector<Value> ints;
    for (int i = 0; i < 5000; ++i) {
        ints.push_back(i % 11 == 0 ? Value{Null{}} : Value{static_cast<std::int32_t>(rng() % 1000)});
    }
    CheckRoundTrip(ColumnType::kInt, ints, Encoding::kPlain);
    CheckRoundTrip(ColumnType::kInt, ints, Encoding::kRle);
    CheckRoundTrip(ColumnType::kInt, ints, Encoding::kDictionary);
    CheckRoundTrip(ColumnType::kInt, ints, Encoding::kBitpack);

    std::vector<Value> bigs;
    for (int i = 0; i < 3000; ++i) {
        bigs.push_back(Value{static_cast<std::int64_t>(rng())});
    }
    CheckRoundTrip(ColumnType::kBigInt, bigs, Encoding::kPlain);
    CheckRoundTrip(ColumnType::kBigInt, bigs, Encoding::kBitpack);

    std::vector<Value> dbls;
    for (int i = 0; i < 3000; ++i) dbls.push_back(Value{static_cast<double>(rng() % 100) * 0.5});
    CheckRoundTrip(ColumnType::kFloat, dbls, Encoding::kPlain);
    CheckRoundTrip(ColumnType::kFloat, dbls, Encoding::kRle);
    CheckRoundTrip(ColumnType::kFloat, dbls, Encoding::kDictionary);

    std::vector<std::string> pool = {"alpha", "beta", "gamma", "", "delta-epsilon"};
    std::vector<Value> strs;
    for (int i = 0; i < 3000; ++i) {
        strs.push_back(i % 7 == 0 ? Value{Null{}} : Value{pool[rng() % pool.size()]});
    }
    CheckRoundTrip(ColumnType::kVarchar, strs, Encoding::kPlain);
    CheckRoundTrip(ColumnType::kVarchar, strs, Encoding::kDictionary);
}

TEST_CASE("RLE compresses runs", "[compression]") {
    std::vector<Value> v;
    for (int run = 0; run < 500; ++run) {
        for (int k = 0; k < 20; ++k) v.push_back(Value{static_cast<std::int32_t>(run)});
    }
    EncodedChunk plain = EncodeChunkWith(ColumnType::kInt, v, Encoding::kPlain);
    EncodedChunk rle = EncodeChunkWith(ColumnType::kInt, v, Encoding::kRle);
    REQUIRE(rle.payload.size() * 3 < plain.payload.size());
}

TEST_CASE("Dictionary compresses low-cardinality strings", "[compression]") {
    std::vector<std::string> pool = {"Berlin", "Munich", "Hamburg", "Cologne", "Frankfurt"};
    std::mt19937_64 rng(9);
    std::vector<Value> v;
    for (int i = 0; i < 2000; ++i) v.push_back(Value{pool[rng() % pool.size()]});
    EncodedChunk plain = EncodeChunkWith(ColumnType::kVarchar, v, Encoding::kPlain);
    EncodedChunk dict = EncodeChunkWith(ColumnType::kVarchar, v, Encoding::kDictionary);
    REQUIRE(dict.payload.size() * 10 < plain.payload.size());
}

TEST_CASE("Bit-packing compresses small-range integers", "[compression]") {
    std::mt19937_64 rng(5);
    std::vector<Value> v;
    for (int i = 0; i < 2000; ++i) v.push_back(Value{static_cast<std::int32_t>(rng() % 8)});
    EncodedChunk plain = EncodeChunkWith(ColumnType::kInt, v, Encoding::kPlain);
    EncodedChunk bp = EncodeChunkWith(ColumnType::kInt, v, Encoding::kBitpack);
    REQUIRE(bp.payload.size() * 5 < plain.payload.size());
}

TEST_CASE("Auto-selection picks a compressing encoding and round-trips", "[compression]") {
    // Low-cardinality strings -> dictionary.
    std::vector<std::string> pool = {"X", "Y", "Z"};
    std::mt19937_64 rng(3);
    std::vector<Value> v;
    for (int i = 0; i < 2000; ++i) v.push_back(Value{pool[rng() % pool.size()]});
    EncodedChunk best = EncodeChunk(ColumnType::kVarchar, v);
    REQUIRE(best.encoding == Encoding::kDictionary);
    REQUIRE(best.num_rows == 2000);

    Vector out(ColumnType::kVarchar);
    DecodeChunk(ColumnType::kVarchar, best.encoding, best.num_rows, best.payload.data(),
                best.payload.size(), out);
    for (std::size_t i = 0; i < v.size(); ++i) REQUIRE(out.GetValue(i) == v[i]);

    // Small-range ints -> bitpack, with a valid zone map.
    std::vector<Value> ints;
    for (int i = 0; i < 2000; ++i) ints.push_back(Value{static_cast<std::int32_t>(rng() % 16)});
    EncodedChunk bi = EncodeChunk(ColumnType::kInt, ints);
    REQUIRE(bi.encoding == Encoding::kBitpack);
    REQUIRE(bi.has_zone_map);
    REQUIRE(bi.min_value >= 0);
    REQUIRE(bi.max_value <= 15);
}
