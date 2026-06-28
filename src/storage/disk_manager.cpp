#include "storage/disk_manager.h"

#include <array>
#include <stdexcept>

namespace liteolap {

namespace {

std::fstream OpenOrCreate(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        std::ofstream create(path, std::ios::binary);
        if (!create) {
            throw std::runtime_error("DiskManager: failed to create file " + path.string());
        }
    }
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        throw std::runtime_error("DiskManager: failed to open file " + path.string());
    }
    return f;
}

}  // namespace

DiskManager::DiskManager(std::filesystem::path path) : path_(std::move(path)) {
    file_ = OpenOrCreate(path_);
    file_.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file_.tellg());

    if (size == 0) {
        num_pages_ = 0;
        AllocatePage();  // page 0 = catalog
        return;
    }
    if (size % kPageSize != 0) {
        throw std::runtime_error("DiskManager: file size is not a multiple of page size");
    }
    num_pages_ = size / kPageSize;
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void DiskManager::ReadPage(PageId page_id, std::byte* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (page_id >= num_pages_) {
        throw std::out_of_range("DiskManager::ReadPage: page_id out of range");
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(page_id) * kPageSize, std::ios::beg);
    file_.read(reinterpret_cast<char*>(out), kPageSize);
    if (file_.gcount() != static_cast<std::streamsize>(kPageSize)) {
        throw std::runtime_error("DiskManager::ReadPage: short read");
    }
}

void DiskManager::WritePage(PageId page_id, const std::byte* in) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (page_id >= num_pages_) {
        throw std::out_of_range("DiskManager::WritePage: page_id out of range");
    }
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(page_id) * kPageSize, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(in), kPageSize);
    if (!file_) {
        throw std::runtime_error("DiskManager::WritePage: write failed");
    }
    file_.flush();
}

PageId DiskManager::AllocatePage() {
    std::lock_guard<std::mutex> lock(mutex_);
    const PageId new_id = static_cast<PageId>(num_pages_);
    std::array<std::byte, kPageSize> zeros{};
    file_.clear();
    file_.seekp(0, std::ios::end);
    file_.write(reinterpret_cast<const char*>(zeros.data()), kPageSize);
    if (!file_) {
        throw std::runtime_error("DiskManager::AllocatePage: extend failed");
    }
    file_.flush();
    ++num_pages_;
    return new_id;
}

void DiskManager::Sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
}

}  // namespace liteolap
