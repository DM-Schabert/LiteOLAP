#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace liteolap::test {

/// Returns a unique temp path for a database file and removes any stale copy.
inline std::filesystem::path TempDbPath(const std::string& tag) {
    static std::atomic<std::uint64_t> counter{0};
    auto p = std::filesystem::temp_directory_path() /
             ("liteolap_" + tag + "_" + std::to_string(counter.fetch_add(1)) + ".loap");
    std::filesystem::remove(p);
    return p;
}

}  // namespace liteolap::test
