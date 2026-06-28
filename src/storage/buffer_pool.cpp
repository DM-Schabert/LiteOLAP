#include "storage/buffer_pool.h"

#include <cstring>
#include <stdexcept>

namespace liteolap {

BufferPool::BufferPool(DiskManager& disk_manager, std::size_t num_frames)
    : disk_manager_(disk_manager), num_frames_(num_frames) {
    if (num_frames_ == 0) {
        throw std::invalid_argument("BufferPool: num_frames must be > 0");
    }
    frames_.reserve(num_frames_);
    for (std::size_t i = 0; i < num_frames_; ++i) {
        frames_.emplace_back(std::make_unique<Frame>());
        free_list_.push_back(i);
    }
}

BufferPool::~BufferPool() {
    try {
        FlushAll();
    } catch (...) {
    }
}

void BufferPool::Touch(std::size_t frame_idx) {
    auto it = lru_pos_.find(frame_idx);
    if (it != lru_pos_.end()) {
        lru_.erase(it->second);
    }
    lru_.push_front(frame_idx);
    lru_pos_[frame_idx] = lru_.begin();
}

std::size_t BufferPool::PickVictim() {
    for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
        if (frames_[*it]->pin_count == 0) {
            return *it;
        }
    }
    throw std::runtime_error("BufferPool: all frames pinned, cannot evict");
}

std::size_t BufferPool::LoadIntoFrame(PageId page_id) {
    if (auto it = page_table_.find(page_id); it != page_table_.end()) {
        return it->second;
    }

    std::size_t frame_idx;
    if (!free_list_.empty()) {
        frame_idx = free_list_.back();
        free_list_.pop_back();
    } else {
        frame_idx = PickVictim();
        Frame& victim = *frames_[frame_idx];
        if (victim.dirty) {
            disk_manager_.WritePage(victim.page_id, victim.page.data.data());
        }
        page_table_.erase(victim.page_id);
        if (auto lit = lru_pos_.find(frame_idx); lit != lru_pos_.end()) {
            lru_.erase(lit->second);
            lru_pos_.erase(lit);
        }
        victim.valid = false;
    }

    Frame& f = *frames_[frame_idx];
    disk_manager_.ReadPage(page_id, f.page.data.data());
    f.page_id = page_id;
    f.pin_count = 0;
    f.dirty = false;
    f.valid = true;
    page_table_[page_id] = frame_idx;
    return frame_idx;
}

Page* BufferPool::FetchPage(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t frame_idx = LoadIntoFrame(page_id);
    Frame& f = *frames_[frame_idx];
    ++f.pin_count;
    Touch(frame_idx);
    return &f.page;
}

bool BufferPool::UnpinPage(PageId page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Frame& f = *frames_[it->second];
    if (f.pin_count <= 0) {
        return false;
    }
    --f.pin_count;
    if (is_dirty) {
        f.dirty = true;
    }
    return true;
}

bool BufferPool::FlushPage(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Frame& f = *frames_[it->second];
    if (!f.valid) {
        return false;
    }
    disk_manager_.WritePage(f.page_id, f.page.data.data());
    f.dirty = false;
    return true;
}

Page* BufferPool::NewPage(PageId* out_page_id) {
    const PageId pid = disk_manager_.AllocatePage();
    if (out_page_id) {
        *out_page_id = pid;
    }
    Page* p = FetchPage(pid);
    std::memset(p->data.data(), 0, kPageSize);
    p->Header().page_id = pid;
    p->Header().page_type = PageType::kFree;
    p->Header().next_page_id = kInvalidPageId;
    p->Header().payload_bytes = 0;
    return p;
}

void BufferPool::FlushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& fp : frames_) {
        Frame& f = *fp;
        if (f.valid && f.dirty) {
            disk_manager_.WritePage(f.page_id, f.page.data.data());
            f.dirty = false;
        }
    }
}

}  // namespace liteolap
