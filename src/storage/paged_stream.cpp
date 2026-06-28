#include "storage/paged_stream.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace liteolap {

// === PagedOutputStream ======================================================

PagedOutputStream::PagedOutputStream(BufferPool& bp) : bp_(bp) { StartNewPage(); }

void PagedOutputStream::StartNewPage() {
    PageId new_id;
    Page* p = bp_.NewPage(&new_id);
    p->Header().page_type = PageType::kColumnData;
    p->Header().next_page_id = kInvalidPageId;
    p->Header().payload_bytes = 0;

    if (current_page_ != nullptr) {
        // Link previous page to this one, then flush+unpin it.
        current_page_->Header().next_page_id = new_id;
        current_page_->Header().payload_bytes = static_cast<std::uint32_t>(offset_);
        bp_.UnpinPage(current_page_id_, /*is_dirty=*/true);
    } else {
        first_page_id_ = new_id;
    }

    current_page_ = p;
    current_page_id_ = new_id;
    offset_ = 0;
}

void PagedOutputStream::Write(const std::byte* data, std::size_t n) {
    if (finished_) {
        throw std::logic_error("PagedOutputStream::Write after Finish");
    }
    std::size_t written = 0;
    while (written < n) {
        if (offset_ == Page::PayloadCapacity()) {
            StartNewPage();
        }
        const std::size_t room = Page::PayloadCapacity() - offset_;
        const std::size_t take = std::min(room, n - written);
        std::memcpy(current_page_->Payload() + offset_, data + written, take);
        offset_ += take;
        written += take;
    }
    bytes_written_ += n;
}

PageId PagedOutputStream::Finish() {
    if (finished_) {
        throw std::logic_error("PagedOutputStream::Finish called twice");
    }
    current_page_->Header().payload_bytes = static_cast<std::uint32_t>(offset_);
    current_page_->Header().next_page_id = kInvalidPageId;
    bp_.UnpinPage(current_page_id_, /*is_dirty=*/true);
    finished_ = true;
    current_page_ = nullptr;
    return first_page_id_;
}

// === PagedInputStream =======================================================

PagedInputStream::PagedInputStream(BufferPool& bp, PageId first_page_id) : bp_(bp) {
    if (first_page_id == kInvalidPageId) {
        at_end_ = true;
        return;
    }
    LoadPage(first_page_id);
}

void PagedInputStream::LoadPage(PageId pid) {
    Page* p = bp_.FetchPage(pid);
    const auto& h = p->Header();
    page_payload_ = h.payload_bytes;
    next_page_id_ = h.next_page_id;
    page_buf_.resize(page_payload_);
    std::memcpy(page_buf_.data(), p->Payload(), page_payload_);
    bp_.UnpinPage(pid, /*is_dirty=*/false);
    current_page_id_ = pid;
    offset_ = 0;
    if (page_payload_ == 0) {
        at_end_ = true;
    }
}

void PagedInputStream::Advance(std::byte* out, std::size_t n, bool copy) {
    std::size_t done = 0;
    while (done < n) {
        if (at_end_) {
            throw std::runtime_error("PagedInputStream: read past end of stream");
        }
        if (offset_ == page_payload_) {
            if (next_page_id_ == kInvalidPageId) {
                at_end_ = true;
                throw std::runtime_error("PagedInputStream: read past end of stream");
            }
            LoadPage(next_page_id_);
            continue;
        }
        const std::size_t avail = page_payload_ - offset_;
        const std::size_t take = std::min(avail, n - done);
        if (copy) {
            std::memcpy(out + done, page_buf_.data() + offset_, take);
        }
        offset_ += take;
        done += take;
    }
    // Detect end-of-stream once the final byte of the last page is consumed.
    if (offset_ == page_payload_ && next_page_id_ == kInvalidPageId) {
        at_end_ = true;
    }
}

void PagedInputStream::Read(std::byte* out, std::size_t n) { Advance(out, n, /*copy=*/true); }

void PagedInputStream::Skip(std::size_t n) { Advance(nullptr, n, /*copy=*/false); }

}  // namespace liteolap
