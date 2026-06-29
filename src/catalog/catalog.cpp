#include "catalog/catalog.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace liteolap {

namespace {
template <typename T>
void Put(std::vector<std::byte>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
template <typename T>
T Get(const std::byte*& cur, const std::byte* end) {
    if (cur + sizeof(T) > end) throw std::runtime_error("Catalog: truncated");
    T v{};
    std::memcpy(&v, cur, sizeof(T));
    cur += sizeof(T);
    return v;
}
}  // namespace

Catalog Catalog::Load(DiskManager& dm) {
    Catalog c;
    if (dm.NumPages() == 0) return c;

    std::array<std::byte, kPageSize> page{};
    dm.ReadPage(0, page.data());
    if (reinterpret_cast<const PageHeader*>(page.data())->page_type != PageType::kCatalog)
        return c;  // fresh DB, no catalog yet

    // Reassemble the catalog body by walking the page chain.
    std::vector<std::byte> body;
    PageId pid = 0;
    while (pid != kInvalidPageId) {
        dm.ReadPage(pid, page.data());
        const auto* h = reinterpret_cast<const PageHeader*>(page.data());
        const std::byte* payload = page.data() + sizeof(PageHeader);
        body.insert(body.end(), payload, payload + h->payload_bytes);
        pid = h->next_page_id;
    }

    const std::byte* cur = body.data();
    const std::byte* end = body.data() + body.size();
    const auto num_tables = Get<std::uint32_t>(cur, end);
    for (std::uint32_t t = 0; t < num_tables; ++t) {
        TableMeta m;
        const auto name_len = Get<std::uint8_t>(cur, end);
        if (cur + name_len > end) throw std::runtime_error("Catalog: truncated name");
        m.name.assign(reinterpret_cast<const char*>(cur), name_len);
        cur += name_len;

        const auto schema_size = Get<std::uint32_t>(cur, end);
        std::size_t consumed = 0;
        m.schema = Schema::Deserialize(cur, schema_size, consumed);
        cur += schema_size;

        const auto num_rg = Get<std::uint32_t>(cur, end);
        const std::size_t ncols = m.schema.NumColumns();
        for (std::uint32_t r = 0; r < num_rg; ++r) {
            RowGroup rg;
            rg.num_rows = Get<std::uint64_t>(cur, end);
            rg.column_roots.resize(ncols);
            for (std::size_t k = 0; k < ncols; ++k) {
                rg.column_roots[k] = Get<PageId>(cur, end);
            }
            m.row_groups.push_back(std::move(rg));
        }
        c.tables_.emplace(m.name, std::move(m));
    }
    return c;
}








void Catalog::Persist(DiskManager& dm) const {
    std::vector<std::byte> body;
    Put(body, static_cast<std::uint32_t>(tables_.size()));
    for (const auto& [name, m] : tables_) {
        Put(body, static_cast<std::uint8_t>(m.name.size()));
        const auto* p = reinterpret_cast<const std::byte*>(m.name.data());
        body.insert(body.end(), p, p + m.name.size());

        const auto schema_bytes = m.schema.Serialize();
        Put(body, static_cast<std::uint32_t>(schema_bytes.size()));
        body.insert(body.end(), schema_bytes.begin(), schema_bytes.end());

        Put(body, static_cast<std::uint32_t>(m.row_groups.size()));
        for (const auto& rg : m.row_groups) {
            Put(body, rg.num_rows);
            for (PageId pid : rg.column_roots) Put(body, pid);
        }
    }


    // -> bis hier wurde der body super erstellt

    // Split the body across a chain of pages (4080 payload bytes each).
    constexpr std::size_t cap = kPageSize - sizeof(PageHeader);
    const std::size_t num_pages = std::max<std::size_t>(1, (body.size() + cap - 1) / cap);

    // Reuse the existing catalog page chain to avoid leaking pages on every
    // Persist. Page 0 always exists; only follow next_page_id if it is already
    // a catalog page (a fresh, zero-filled page 0 is kFree and points to 0).
    std::vector<PageId> chain;
    chain.push_back(0);
    {
        std::array<std::byte, kPageSize> probe{};
        dm.ReadPage(0, probe.data());
        const auto* h = reinterpret_cast<const PageHeader*>(probe.data());
        PageId next = (h->page_type == PageType::kCatalog) ? h->next_page_id : kInvalidPageId;
        while (next != kInvalidPageId) {
            chain.push_back(next);
            dm.ReadPage(next, probe.data());
            next = reinterpret_cast<const PageHeader*>(probe.data())->next_page_id;
        }
    }
    while (chain.size() < num_pages) chain.push_back(dm.AllocatePage());

    // Write each body slice onto its page, linking pages via next_page_id.
    for (std::size_t i = 0; i < num_pages; ++i) {
        const std::size_t start = i * cap;
        const std::size_t len = std::min(cap, body.size() - start);
        std::array<std::byte, kPageSize> page{};
        auto* hdr = reinterpret_cast<PageHeader*>(page.data());
        hdr->page_id = chain[i];
        hdr->page_type = PageType::kCatalog;
        hdr->next_page_id = (i + 1 < num_pages) ? chain[i + 1] : kInvalidPageId;
        hdr->payload_bytes = static_cast<std::uint32_t>(len);
        if (len > 0)
            std::memcpy(page.data() + sizeof(PageHeader), body.data() + start, len);
        dm.WritePage(chain[i], page.data());
    }
    dm.Sync();
}













void Catalog::CreateTable(std::string name, Schema schema) {
    if (tables_.count(name) != 0) {
        throw std::invalid_argument("CreateTable: table already exists: " + name);
    }
    TableMeta m;
    m.name = name;
    m.schema = std::move(schema);
    tables_.emplace(std::move(name), std::move(m));
}

bool Catalog::DropTable(std::string_view name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return false;
    tables_.erase(it);
    return true;
}

void Catalog::AddRowGroup(std::string_view table, RowGroup rg) {
    auto it = tables_.find(table);
    if (it == tables_.end()) throw std::invalid_argument("AddRowGroup: no such table");
    it->second.row_groups.push_back(std::move(rg));
}

void Catalog::ClearRowGroups(std::string_view table) {
    auto it = tables_.find(table);
    if (it == tables_.end()) throw std::invalid_argument("ClearRowGroups: no such table");
    it->second.row_groups.clear();
}

TableMeta* Catalog::GetTable(std::string_view name) {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

const TableMeta* Catalog::GetTable(std::string_view name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::ListTables() const {
    std::vector<std::string> out;
    for (const auto& [n, _] : tables_) out.push_back(n);
    return out;
}

}  // namespace liteolap
