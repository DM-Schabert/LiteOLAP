#include <catch2/catch_test_macros.hpp>
#include <random>
#include <vector>

#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/paged_stream.h"
#include "test_helpers.h"

using namespace liteolap;
using liteolap::test::TempDbPath;

TEST_CASE("PagedStream round-trips a multi-page byte sequence", "[stream]") {
    auto path = TempDbPath("stream_rt");
    DiskManager dm(path);
    BufferPool bp(dm, 16);

    // ~50 KiB of data forces many page spills.
    std::vector<std::byte> payload(50'000);
    std::mt19937 rng(7);
    for (auto& b : payload) b = static_cast<std::byte>(rng() & 0xFF);

    PageId first;
    {
        PagedOutputStream out(bp);
        out.Write(payload.data(), payload.size());
        REQUIRE(out.BytesWritten() == payload.size());
        first = out.Finish();
    }

    PagedInputStream in(bp, first);
    std::vector<std::byte> readback(payload.size());
    in.Read(readback.data(), readback.size());
    REQUIRE(readback == payload);
    REQUIRE(in.AtEnd());

    std::filesystem::remove(path);
}

TEST_CASE("PagedStream Skip advances without copying", "[stream]") {
    auto path = TempDbPath("stream_skip");
    DiskManager dm(path);
    BufferPool bp(dm, 16);

    PageId first;
    {
        PagedOutputStream out(bp);
        for (std::uint32_t i = 0; i < 5000; ++i) out.WritePod(i);
        first = out.Finish();
    }

    PagedInputStream in(bp, first);
    in.Skip(sizeof(std::uint32_t) * 2500);
    REQUIRE(in.ReadPod<std::uint32_t>() == 2500);

    std::filesystem::remove(path);
}

TEST_CASE("PagedStream handles small writes interleaved", "[stream]") {
    auto path = TempDbPath("stream_pod");
    DiskManager dm(path);
    BufferPool bp(dm, 8);

    PageId first;
    {
        PagedOutputStream out(bp);
        for (std::int64_t i = 0; i < 2000; ++i) out.WritePod<std::int64_t>(i * 3);
        first = out.Finish();
    }
    PagedInputStream in(bp, first);
    for (std::int64_t i = 0; i < 2000; ++i) {
        REQUIRE(in.ReadPod<std::int64_t>() == i * 3);
    }
    std::filesystem::remove(path);
}
