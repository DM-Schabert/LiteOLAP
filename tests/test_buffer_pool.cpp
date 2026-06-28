#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "test_helpers.h"

using namespace liteolap;
using liteolap::test::TempDbPath;

namespace {
void Stamp(Page* p, std::uint8_t v) { std::memset(p->data.data(), v, kPageSize); }
bool AllBytesAre(const Page& p, std::uint8_t v) {
    for (auto b : p.data)
        if (static_cast<std::uint8_t>(b) != v) return false;
    return true;
}
}  // namespace

TEST_CASE("BufferPool fetch/unpin round-trip", "[bufferpool]") {
    auto path = TempDbPath("bp_fetch");
    DiskManager dm(path);
    BufferPool bp(dm, 4);
    PageId pid;
    Page* p = bp.NewPage(&pid);
    Stamp(p, 0x42);
    REQUIRE(bp.UnpinPage(pid, true));
    Page* p2 = bp.FetchPage(pid);
    REQUIRE(AllBytesAre(*p2, 0x42));
    REQUIRE(bp.UnpinPage(pid, false));
    std::filesystem::remove(path);
}

TEST_CASE("BufferPool evicts LRU and writes back dirty", "[bufferpool]") {
    auto path = TempDbPath("bp_lru");
    DiskManager dm(path);
    BufferPool bp(dm, 3);
    std::vector<PageId> pids;
    for (std::size_t i = 0; i < 4; ++i) {
        PageId pid;
        Page* p = bp.NewPage(&pid);
        Stamp(p, static_cast<std::uint8_t>(0x10 + i));
        REQUIRE(bp.UnpinPage(pid, true));
        pids.push_back(pid);
    }
    Page* p0 = bp.FetchPage(pids[0]);  // was evicted, must come back from disk
    REQUIRE(AllBytesAre(*p0, 0x10));
    REQUIRE(bp.UnpinPage(pids[0], false));
    std::filesystem::remove(path);
}

TEST_CASE("BufferPool throws when all frames pinned", "[bufferpool]") {
    auto path = TempDbPath("bp_pinned");
    DiskManager dm(path);
    BufferPool bp(dm, 2);
    PageId p1, p2, p3;
    (void)bp.NewPage(&p1);
    (void)bp.NewPage(&p2);
    REQUIRE_THROWS_AS(bp.NewPage(&p3), std::runtime_error);
    std::filesystem::remove(path);
}
