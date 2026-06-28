#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace liteolap {

/// Fixed page size for the entire engine — 4 KiB. Kept identical to the
/// sibling OLTP engine so the storage layer is shared verbatim. Column
/// chunks span multiple pages via the paged-stream abstraction, so the
/// small page size does not constrain analytical chunk sizes.
inline constexpr std::size_t kPageSize = 4096;

/// Sentinel for "no page" / end of a page chain.
inline constexpr std::uint32_t kInvalidPageId = 0xFFFFFFFFu;

using PageId = std::uint32_t;

/// Discriminates how the bytes inside a Page should be interpreted.
enum class PageType : std::uint16_t {
    kFree = 0,
    kCatalog = 1,
    kColumnData = 2,  ///< a link in a column's paged byte stream
};

/// Header laid out at the very beginning of every page.
struct PageHeader {
    PageId page_id;
    PageType page_type;
    std::uint16_t reserved;
    std::uint32_t payload_bytes;  ///< meaningful bytes in this page's payload
    PageId next_page_id;          ///< next page in this stream, or kInvalidPageId
};

static_assert(sizeof(PageHeader) == 16, "PageHeader must be 16 bytes");

/// A raw, fixed-size page buffer.
struct Page {
    std::array<std::byte, kPageSize> data{};

    PageHeader& Header() noexcept { return *reinterpret_cast<PageHeader*>(data.data()); }
    const PageHeader& Header() const noexcept {
        return *reinterpret_cast<const PageHeader*>(data.data());
    }

    std::byte* Payload() noexcept { return data.data() + sizeof(PageHeader); }
    const std::byte* Payload() const noexcept { return data.data() + sizeof(PageHeader); }

    static constexpr std::size_t PayloadCapacity() noexcept {
        return kPageSize - sizeof(PageHeader);
    }
};

static_assert(sizeof(Page) == kPageSize, "Page must be exactly one page in size");

}  // namespace liteolap
