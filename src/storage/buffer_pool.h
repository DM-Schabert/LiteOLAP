#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/page.h"

namespace liteolap {

inline constexpr std::size_t kDefaultBufferPoolFrames = 1024;

/**
 * In-memory cache of database pages with pinning + LRU eviction. Identical
 * in spirit to the OLTP engine's pool; the larger default frame count
 * reflects the scan-heavy analytical workload.
 */
class BufferPool {
   public:
    BufferPool(DiskManager& disk_manager, std::size_t num_frames = kDefaultBufferPoolFrames);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    Page* FetchPage(PageId page_id);
    bool UnpinPage(PageId page_id, bool is_dirty);
    bool FlushPage(PageId page_id);
    Page* NewPage(PageId* out_page_id);
    void FlushAll();

    std::size_t Capacity() const noexcept { return num_frames_; }

   private:
    struct Frame {
        Page page{};
        PageId page_id{kInvalidPageId};
        int pin_count{0};
        bool dirty{false};
        bool valid{false};
    };

    std::size_t LoadIntoFrame(PageId page_id);
    std::size_t PickVictim();
    void Touch(std::size_t frame_idx);

    DiskManager& disk_manager_;
    std::size_t num_frames_;
    std::vector<std::unique_ptr<Frame>> frames_;
    std::unordered_map<PageId, std::size_t> page_table_;
    std::list<std::size_t> lru_;
    std::unordered_map<std::size_t, std::list<std::size_t>::iterator> lru_pos_;
    std::vector<std::size_t> free_list_;
    mutable std::mutex mutex_;
};

}  // namespace liteolap
