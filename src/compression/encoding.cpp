#include "compression/encoding.h"

#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace liteolap {

namespace {

// --- byte helpers ----------------------------------------------------------

template <typename T>
void PutPod(std::vector<std::byte>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
T GetPod(const std::byte*& cur, const std::byte* end) {
    if (cur + sizeof(T) > end) throw std::runtime_error("encoding: truncated payload");
    T v{};
    std::memcpy(&v, cur, sizeof(T));
    cur += sizeof(T);
    return v;
}

// --- validity bitmap -------------------------------------------------------

std::size_t ValidityBytes(std::size_t n) { return (n + 7) / 8; }

void WriteValidity(std::vector<std::byte>& out, const std::vector<Value>& values) {
    const std::size_t vb = ValidityBytes(values.size());
    const std::size_t start = out.size();
    out.insert(out.end(), vb, std::byte{0});
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!IsNull(values[i])) {
            out[start + i / 8] |= static_cast<std::byte>(1u << (i % 8));
        }
    }
}

bool TestValidity(const std::byte* bitmap, std::size_t i) {
    return (static_cast<std::uint8_t>(bitmap[i / 8]) & (1u << (i % 8))) != 0;
}

// --- numeric extraction ----------------------------------------------------

std::int64_t AsI64(const Value& v) {
    if (std::holds_alternative<std::int32_t>(v)) return std::get<std::int32_t>(v);
    if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v);
    throw std::runtime_error("encoding: expected integer value");
}

double AsDouble(const Value& v) { return std::get<double>(v); }

}  // namespace

const char* EncodingName(Encoding e) {
    switch (e) {
        case Encoding::kPlain:
            return "plain";
        case Encoding::kRle:
            return "rle";
        case Encoding::kDictionary:
            return "dictionary";
        case Encoding::kBitpack:
            return "bitpack";
    }
    return "?";
}

unsigned BitsRequired(std::uint64_t max_val) {
    unsigned b = 0;
    while (max_val > 0) {
        ++b;
        max_val >>= 1;
    }
    return b == 0 ? 1 : b;
}

void PackBits(const std::uint64_t* vals, std::size_t n, unsigned bit_width,
              std::vector<std::byte>& out) {
    if (bit_width == 0) return;
    // Bit-granular writer: fill one output byte at a time, LSB-first. Works
    // for any bit width 0..64 without overflowing a 64-bit accumulator.
    std::uint8_t cur = 0;
    unsigned cur_bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t v = vals[i];
        unsigned remaining = bit_width;
        while (remaining > 0) {
            const unsigned take = std::min(8u - cur_bits, remaining);
            const std::uint8_t bits = static_cast<std::uint8_t>(v & ((1u << take) - 1u));
            cur |= static_cast<std::uint8_t>(bits << cur_bits);
            cur_bits += take;
            v >>= take;
            remaining -= take;
            if (cur_bits == 8) {
                out.push_back(static_cast<std::byte>(cur));
                cur = 0;
                cur_bits = 0;
            }
        }
    }
    if (cur_bits > 0) out.push_back(static_cast<std::byte>(cur));
}

void UnpackBits(const std::byte* in, std::size_t n, unsigned bit_width, std::uint64_t* out) {
    if (bit_width == 0) {
        for (std::size_t i = 0; i < n; ++i) out[i] = 0;
        return;
    }
    std::size_t byte_idx = 0;
    std::uint8_t cur = 0;
    unsigned cur_bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t val = 0;
        unsigned got = 0;
        while (got < bit_width) {
            if (cur_bits == 0) {
                cur = static_cast<std::uint8_t>(in[byte_idx++]);
                cur_bits = 8;
            }
            const unsigned take = std::min(cur_bits, bit_width - got);
            const std::uint64_t bits = cur & ((1u << take) - 1u);
            val |= bits << got;
            cur = static_cast<std::uint8_t>(cur >> take);
            cur_bits -= take;
            got += take;
        }
        out[i] = val;
    }
}

// === PLAIN ==================================================================

namespace {

EncodedChunk EncodePlain(ColumnType type, const std::vector<Value>& values) {
    EncodedChunk c;
    c.encoding = Encoding::kPlain;
    WriteValidity(c.payload, values);
    if (type == ColumnType::kVarchar) {
        // offsets (n+1) then bytes
        std::vector<std::uint32_t> offsets;
        offsets.reserve(values.size() + 1);
        std::vector<std::byte> bytes;
        std::uint32_t cur = 0;
        offsets.push_back(0);
        for (const auto& v : values) {
            if (!IsNull(v)) {
                const auto& s = std::get<std::string>(v);
                const auto* p = reinterpret_cast<const std::byte*>(s.data());
                bytes.insert(bytes.end(), p, p + s.size());
                cur += static_cast<std::uint32_t>(s.size());
            }
            offsets.push_back(cur);
        }
        for (auto o : offsets) PutPod(c.payload, o);
        c.payload.insert(c.payload.end(), bytes.begin(), bytes.end());
    } else {
        const std::size_t w = FixedWidth(type);
        for (const auto& v : values) {
            if (IsNull(v)) {
                c.payload.insert(c.payload.end(), w, std::byte{0});
            } else if (type == ColumnType::kFloat) {
                PutPod(c.payload, AsDouble(v));
            } else if (type == ColumnType::kInt) {
                PutPod(c.payload, static_cast<std::int32_t>(AsI64(v)));
            } else {
                PutPod(c.payload, AsI64(v));
            }
        }
    }
    return c;
}

void DecodePlain(ColumnType type, std::uint32_t n, const std::byte* p, std::size_t len,
                 Vector& out) {
    const std::byte* end = p + len;
    const std::byte* validity = p;
    const std::byte* cur = p + ValidityBytes(n);
    if (type == ColumnType::kVarchar) {
        const std::byte* offbase = cur;
        cur += static_cast<std::size_t>(n + 1) * sizeof(std::uint32_t);
        const char* strbase = reinterpret_cast<const char*>(cur);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!TestValidity(validity, i)) {
                out.AppendNull();
                continue;
            }
            std::uint32_t b, e;
            std::memcpy(&b, offbase + static_cast<std::size_t>(i) * 4, 4);
            std::memcpy(&e, offbase + static_cast<std::size_t>(i + 1) * 4, 4);
            out.AppendString(std::string_view(strbase + b, e - b));
        }
    } else {
        const std::size_t w = FixedWidth(type);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::byte* slot = cur + static_cast<std::size_t>(i) * w;
            if (slot + w > end) throw std::runtime_error("encoding: plain underflow");
            if (!TestValidity(validity, i)) {
                out.AppendNull();
                continue;
            }
            if (type == ColumnType::kInt) {
                std::int32_t v;
                std::memcpy(&v, slot, 4);
                out.AppendInt32(v);
            } else if (type == ColumnType::kBigInt) {
                std::int64_t v;
                std::memcpy(&v, slot, 8);
                out.AppendInt64(v);
            } else {
                double v;
                std::memcpy(&v, slot, 8);
                out.AppendDouble(v);
            }
        }
    }
}

// === RLE (fixed-width types) ===============================================
// Runs are over the raw value slots; null slots carry value 0 and are
// restored to NULL via the validity bitmap.

EncodedChunk EncodeRle(ColumnType type, const std::vector<Value>& values) {
    EncodedChunk c;
    c.encoding = Encoding::kRle;
    WriteValidity(c.payload, values);

    // Materialize raw 64-bit (or double-as-bits) slot values.
    const std::size_t n = values.size();
    auto slot_bits = [&](std::size_t i) -> std::uint64_t {
        if (IsNull(values[i])) return 0;
        if (type == ColumnType::kFloat) {
            double d = AsDouble(values[i]);
            std::uint64_t bits;
            std::memcpy(&bits, &d, 8);
            return bits;
        }
        return static_cast<std::uint64_t>(AsI64(values[i]));
    };

    std::vector<std::pair<std::uint64_t, std::uint32_t>> runs;
    for (std::size_t i = 0; i < n;) {
        std::uint64_t v = slot_bits(i);
        std::uint32_t len = 1;
        while (i + len < n && slot_bits(i + len) == v) ++len;
        runs.emplace_back(v, len);
        i += len;
    }
    PutPod(c.payload, static_cast<std::uint32_t>(runs.size()));
    const std::size_t w = FixedWidth(type);
    for (auto& [val, len] : runs) {
        if (w == 4) {
            PutPod(c.payload, static_cast<std::int32_t>(val));
        } else {
            PutPod(c.payload, val);
        }
        PutPod(c.payload, len);
    }
    return c;
}

void DecodeRle(ColumnType type, std::uint32_t n, const std::byte* p, std::size_t len,
               Vector& out) {
    const std::byte* end = p + len;
    const std::byte* validity = p;
    const std::byte* cur = p + ValidityBytes(n);
    const std::uint32_t num_runs = GetPod<std::uint32_t>(cur, end);
    const std::size_t w = FixedWidth(type);

    std::uint32_t row = 0;
    for (std::uint32_t r = 0; r < num_runs; ++r) {
        std::uint64_t bits;
        if (w == 4) {
            bits = static_cast<std::uint64_t>(GetPod<std::int32_t>(cur, end));
        } else {
            bits = GetPod<std::uint64_t>(cur, end);
        }
        const std::uint32_t run_len = GetPod<std::uint32_t>(cur, end);
        for (std::uint32_t k = 0; k < run_len; ++k, ++row) {
            if (!TestValidity(validity, row)) {
                out.AppendNull();
                continue;
            }
            if (type == ColumnType::kInt) {
                out.AppendInt32(static_cast<std::int32_t>(bits));
            } else if (type == ColumnType::kBigInt) {
                out.AppendInt64(static_cast<std::int64_t>(bits));
            } else {
                double d;
                std::memcpy(&d, &bits, 8);
                out.AppendDouble(d);
            }
        }
    }
}

// === DICTIONARY (varchar + fixed) ==========================================
// Layout: validity | u32 dict_count | dict entries | u8 idx_bit_width |
//         packed indices (one per row; null rows store index 0).

EncodedChunk EncodeDictionary(ColumnType type, const std::vector<Value>& values) {
    EncodedChunk c;
    c.encoding = Encoding::kDictionary;
    WriteValidity(c.payload, values);

    std::vector<Value> dict;
    std::vector<std::uint64_t> indices;
    indices.reserve(values.size());

    if (type == ColumnType::kVarchar) {
        std::unordered_map<std::string, std::uint32_t> lookup;
        for (const auto& v : values) {
            if (IsNull(v)) {
                indices.push_back(0);
                continue;
            }
            const auto& s = std::get<std::string>(v);
            auto it = lookup.find(s);
            if (it == lookup.end()) {
                std::uint32_t id = static_cast<std::uint32_t>(dict.size());
                lookup.emplace(s, id);
                dict.emplace_back(s);
                indices.push_back(id);
            } else {
                indices.push_back(it->second);
            }
        }
    } else {
        std::map<std::int64_t, std::uint32_t> lookup;  // works for int/bigint/float-bits
        for (const auto& v : values) {
            if (IsNull(v)) {
                indices.push_back(0);
                continue;
            }
            std::int64_t key;
            if (type == ColumnType::kFloat) {
                double d = AsDouble(v);
                std::memcpy(&key, &d, 8);
            } else {
                key = AsI64(v);
            }
            auto it = lookup.find(key);
            if (it == lookup.end()) {
                std::uint32_t id = static_cast<std::uint32_t>(dict.size());
                lookup.emplace(key, id);
                dict.push_back(v);
                indices.push_back(id);
            } else {
                indices.push_back(it->second);
            }
        }
    }

    PutPod(c.payload, static_cast<std::uint32_t>(dict.size()));
    if (type == ColumnType::kVarchar) {
        for (const auto& v : dict) {
            const auto& s = std::get<std::string>(v);
            PutPod(c.payload, static_cast<std::uint32_t>(s.size()));
            const auto* p = reinterpret_cast<const std::byte*>(s.data());
            c.payload.insert(c.payload.end(), p, p + s.size());
        }
    } else {
        const std::size_t w = FixedWidth(type);
        for (const auto& v : dict) {
            if (type == ColumnType::kFloat) {
                PutPod(c.payload, AsDouble(v));
            } else if (w == 4) {
                PutPod(c.payload, static_cast<std::int32_t>(AsI64(v)));
            } else {
                PutPod(c.payload, AsI64(v));
            }
        }
    }

    const unsigned bw = dict.empty() ? 0 : BitsRequired(dict.size() - 1);
    PutPod(c.payload, static_cast<std::uint8_t>(bw));
    PackBits(indices.data(), indices.size(), bw, c.payload);
    return c;
}

void DecodeDictionary(ColumnType type, std::uint32_t n, const std::byte* p, std::size_t len,
                      Vector& out) {
    const std::byte* end = p + len;
    const std::byte* validity = p;
    const std::byte* cur = p + ValidityBytes(n);
    const std::uint32_t dict_count = GetPod<std::uint32_t>(cur, end);

    std::vector<Value> dict;
    dict.reserve(dict_count);
    if (type == ColumnType::kVarchar) {
        for (std::uint32_t i = 0; i < dict_count; ++i) {
            const std::uint32_t slen = GetPod<std::uint32_t>(cur, end);
            if (cur + slen > end) throw std::runtime_error("encoding: dict str underflow");
            dict.emplace_back(std::string(reinterpret_cast<const char*>(cur), slen));
            cur += slen;
        }
    } else {
        for (std::uint32_t i = 0; i < dict_count; ++i) {
            if (type == ColumnType::kFloat) {
                dict.emplace_back(GetPod<double>(cur, end));
            } else if (type == ColumnType::kInt) {
                dict.emplace_back(GetPod<std::int32_t>(cur, end));
            } else {
                dict.emplace_back(GetPod<std::int64_t>(cur, end));
            }
        }
    }

    const unsigned bw = GetPod<std::uint8_t>(cur, end);
    std::vector<std::uint64_t> indices(n);
    UnpackBits(cur, n, bw, indices.data());

    for (std::uint32_t i = 0; i < n; ++i) {
        if (!TestValidity(validity, i)) {
            out.AppendNull();
            continue;
        }
        out.AppendValue(dict[indices[i]]);
    }
}

// === BITPACK (int/bigint, frame-of-reference) ==============================
// Layout: validity | i64 frame_min | u8 bit_width | packed deltas.

EncodedChunk EncodeBitpack(ColumnType type, const std::vector<Value>& values,
                           std::int64_t min_v, std::int64_t max_v) {
    EncodedChunk c;
    c.encoding = Encoding::kBitpack;
    WriteValidity(c.payload, values);
    PutPod(c.payload, min_v);

    // Compute range and deltas in unsigned space so a min/max spanning the
    // full int64 domain cannot overflow.
    const std::uint64_t range =
        static_cast<std::uint64_t>(max_v) - static_cast<std::uint64_t>(min_v);
    const unsigned bw = (max_v == min_v) ? 0 : BitsRequired(range);
    PutPod(c.payload, static_cast<std::uint8_t>(bw));

    std::vector<std::uint64_t> deltas(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (IsNull(values[i])) {
            deltas[i] = 0;
        } else {
            deltas[i] = static_cast<std::uint64_t>(AsI64(values[i])) -
                        static_cast<std::uint64_t>(min_v);
        }
    }
    PackBits(deltas.data(), deltas.size(), bw, c.payload);
    (void)type;
    return c;
}

void DecodeBitpack(ColumnType type, std::uint32_t n, const std::byte* p, std::size_t len,
                   Vector& out) {
    const std::byte* end = p + len;
    const std::byte* validity = p;
    const std::byte* cur = p + ValidityBytes(n);
    const std::int64_t frame_min = GetPod<std::int64_t>(cur, end);
    const unsigned bw = GetPod<std::uint8_t>(cur, end);
    std::vector<std::uint64_t> deltas(n);
    UnpackBits(cur, n, bw, deltas.data());
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!TestValidity(validity, i)) {
            out.AppendNull();
            continue;
        }
        const std::int64_t v = frame_min + static_cast<std::int64_t>(deltas[i]);
        if (type == ColumnType::kInt) {
            out.AppendInt32(static_cast<std::int32_t>(v));
        } else {
            out.AppendInt64(v);
        }
    }
}

}  // namespace

EncodedChunk EncodeChunkWith(ColumnType type, const std::vector<Value>& values, Encoding enc) {
    switch (enc) {
        case Encoding::kPlain:
            return EncodePlain(type, values);
        case Encoding::kRle:
            return EncodeRle(type, values);
        case Encoding::kDictionary:
            return EncodeDictionary(type, values);
        case Encoding::kBitpack: {
            std::int64_t mn = std::numeric_limits<std::int64_t>::max();
            std::int64_t mx = std::numeric_limits<std::int64_t>::min();
            bool any = false;
            for (const auto& v : values) {
                if (IsNull(v)) continue;
                any = true;
                std::int64_t x = AsI64(v);
                mn = std::min(mn, x);
                mx = std::max(mx, x);
            }
            if (!any) {
                mn = 0;
                mx = 0;
            }
            return EncodeBitpack(type, values, mn, mx);
        }
    }
    throw std::runtime_error("EncodeChunkWith: unknown encoding");
}

EncodedChunk EncodeChunk(ColumnType type, const std::vector<Value>& values) {
    // Compute null count and (for integers) zone-map min/max.
    std::uint32_t null_count = 0;
    std::int64_t mn = std::numeric_limits<std::int64_t>::max();
    std::int64_t mx = std::numeric_limits<std::int64_t>::min();
    bool any_int = false;
    const bool is_int = (type == ColumnType::kInt || type == ColumnType::kBigInt);
    for (const auto& v : values) {
        if (IsNull(v)) {
            ++null_count;
            continue;
        }
        if (is_int) {
            any_int = true;
            std::int64_t x = AsI64(v);
            mn = std::min(mn, x);
            mx = std::max(mx, x);
        }
    }

    // Candidate encodings per type.
    std::vector<Encoding> candidates;
    switch (type) {
        case ColumnType::kInt:
        case ColumnType::kBigInt:
            candidates = {Encoding::kPlain, Encoding::kRle, Encoding::kDictionary,
                          Encoding::kBitpack};
            break;
        case ColumnType::kFloat:
            candidates = {Encoding::kPlain, Encoding::kRle, Encoding::kDictionary};
            break;
        case ColumnType::kVarchar:
            candidates = {Encoding::kPlain, Encoding::kDictionary};
            break;
    }

    EncodedChunk best;
    bool have_best = false;
    for (Encoding e : candidates) {
        EncodedChunk c = EncodeChunkWith(type, values, e);
        if (!have_best || c.payload.size() < best.payload.size()) {
            best = std::move(c);
            have_best = true;
        }
    }

    best.num_rows = static_cast<std::uint32_t>(values.size());
    best.null_count = null_count;
    best.has_zone_map = is_int && any_int;
    best.min_value = best.has_zone_map ? mn : 0;
    best.max_value = best.has_zone_map ? mx : 0;
    return best;
}

void DecodeChunk(ColumnType type, Encoding enc, std::uint32_t num_rows, const std::byte* payload,
                 std::size_t len, Vector& out) {
    switch (enc) {
        case Encoding::kPlain:
            DecodePlain(type, num_rows, payload, len, out);
            return;
        case Encoding::kRle:
            DecodeRle(type, num_rows, payload, len, out);
            return;
        case Encoding::kDictionary:
            DecodeDictionary(type, num_rows, payload, len, out);
            return;
        case Encoding::kBitpack:
            DecodeBitpack(type, num_rows, payload, len, out);
            return;
    }
    throw std::runtime_error("DecodeChunk: unknown encoding");
}

}  // namespace liteolap
