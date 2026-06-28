#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <random>

#include "storage/disk_manager.h"
#include "storage/page.h"
#include "test_helpers.h"

using namespace liteolap;
using liteolap::test::TempDbPath;

namespace {
void FillRandom(std::array<std::byte, kPageSize>& buf, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    for (auto& b : buf) b = static_cast<std::byte>(rng() & 0xFF);
}
}  // namespace

TEST_CASE("DiskManager creates a fresh file with page 0", "[disk]") {
    auto path = TempDbPath("disk_fresh");
    {
        DiskManager dm(path);
        REQUIRE(dm.NumPages() == 1);
    }
    REQUIRE(std::filesystem::file_size(path) == kPageSize);
    std::filesystem::remove(path);
}

TEST_CASE("DiskManager round-trips pages byte-for-byte", "[disk]") {
    auto path = TempDbPath("disk_rt");
    constexpr std::size_t kN = 32;
    std::vector<std::array<std::byte, kPageSize>> expected(kN);
    for (std::size_t i = 0; i < kN; ++i) FillRandom(expected[i], 1234 + i);

    {
        DiskManager dm(path);
        for (std::size_t i = 0; i < kN; ++i) {
            PageId pid = dm.AllocatePage();
            REQUIRE(pid == static_cast<PageId>(i + 1));
            dm.WritePage(pid, expected[i].data());
        }
    }
    {
        DiskManager dm(path);
        REQUIRE(dm.NumPages() == kN + 1);
        std::array<std::byte, kPageSize> buf{};
        for (std::size_t i = 0; i < kN; ++i) {
            dm.ReadPage(static_cast<PageId>(i + 1), buf.data());
            REQUIRE(std::memcmp(buf.data(), expected[i].data(), kPageSize) == 0);
        }
    }
    std::filesystem::remove(path);
}

TEST_CASE("DiskManager throws on out-of-range page", "[disk]") {
    auto path = TempDbPath("disk_oob");
    DiskManager dm(path);
    std::array<std::byte, kPageSize> buf{};
    REQUIRE_THROWS_AS(dm.ReadPage(99, buf.data()), std::out_of_range);
    std::filesystem::remove(path);
}
