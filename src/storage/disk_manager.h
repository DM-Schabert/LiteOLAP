#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "storage/page.h"

namespace liteolap {

/**
 * Owns the on-disk database file. The only component that issues raw
 * read/write syscalls; everything above operates on logical `PageId`s.
 * Page 0 is reserved for the catalog. Total page count is recovered from
 * the file size on open.
 */
class DiskManager {
   public:
    explicit DiskManager(std::filesystem::path path);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;
    DiskManager(DiskManager&&) = delete;
    DiskManager& operator=(DiskManager&&) = delete;

    /// Reads `kPageSize` bytes from `page_id` into `out`.
    void ReadPage(PageId page_id, std::byte* out);

    /// Writes `kPageSize` bytes from `in` to `page_id`.
    void WritePage(PageId page_id, const std::byte* in);

    /// Allocates a new zero-filled page by extending the file.
    PageId AllocatePage();

    void Sync();

    std::size_t NumPages() const noexcept { return num_pages_; }
    const std::filesystem::path& Path() const noexcept { return path_; }

   private:
    std::filesystem::path path_;
    std::fstream file_;
    std::size_t num_pages_{0};
    mutable std::mutex mutex_;
};

}  // namespace liteolap
