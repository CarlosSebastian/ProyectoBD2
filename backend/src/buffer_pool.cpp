#include "db/buffer_pool.hpp"

#include "db/disk_counter.hpp"

namespace db {

BufferPool::BufferPool(DiskManager* dm, int pool_size) : disk_(dm) {
    if (pool_size < 4) pool_size = 4;   // el B+ Tree necesita varias paginas a la vez
    frames_.resize(pool_size);
}

BufferPool::~BufferPool() { flushAll(); }

int BufferPool::findVictim() {
    // 1) frame libre
    for (std::size_t i = 0; i < frames_.size(); ++i)
        if (frames_[i].page_id == INVALID_PAGE_ID) return static_cast<int>(i);

    // 2) LRU entre los no pineados
    int           victim = -1;
    std::uint64_t oldest = UINT64_MAX;
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].pin_count == 0 && frames_[i].last_used < oldest) {
            oldest = frames_[i].last_used;
            victim = static_cast<int>(i);
        }
    }
    if (victim < 0) return -1;

    Frame& f = frames_[victim];
    if (f.dirty) disk_->writePage(f.page_id, f.data.data());
    table_.erase(f.page_id);
    f.page_id = INVALID_PAGE_ID;
    f.dirty   = false;
    ++evictions_;
    return victim;
}

char* BufferPool::fetchPage(page_id_t pid) {
    DiskCounter::global().countPageAccess();
    auto it = table_.find(pid);
    if (it != table_.end()) {
        Frame& f = frames_[it->second];
        ++f.pin_count;
        f.last_used = ++clock_;
        ++hits_;
        DiskCounter::global().countBufferHit();
        return f.data.data();
    }

    int idx = findVictim();
    if (idx < 0) throw DBException("BufferPool lleno: todas las paginas estan pineadas");

    Frame& f = frames_[idx];
    disk_->readPage(pid, f.data.data());
    f.page_id   = pid;
    f.pin_count = 1;
    f.dirty     = false;
    f.last_used = ++clock_;
    table_[pid] = idx;
    ++misses_;
    return f.data.data();
}

char* BufferPool::newPage(page_id_t* out_pid) {
    DiskCounter::global().countPageAccess();
    page_id_t pid = disk_->allocatePage();
    int idx = findVictim();
    if (idx < 0) throw DBException("BufferPool lleno: todas las paginas estan pineadas");

    Frame& f = frames_[idx];
    std::fill(f.data.begin(), f.data.end(), 0);
    f.page_id   = pid;
    f.pin_count = 1;
    f.dirty     = true;
    f.last_used = ++clock_;
    table_[pid] = idx;
    if (out_pid) *out_pid = pid;
    return f.data.data();
}

void BufferPool::unpinPage(page_id_t pid, bool dirty) {
    auto it = table_.find(pid);
    if (it == table_.end()) return;
    Frame& f = frames_[it->second];
    if (dirty) f.dirty = true;
    if (f.pin_count > 0) --f.pin_count;
}

void BufferPool::flushPage(page_id_t pid) {
    auto it = table_.find(pid);
    if (it == table_.end()) return;
    Frame& f = frames_[it->second];
    if (f.dirty) {
        disk_->writePage(f.page_id, f.data.data());
        f.dirty = false;
    }
}

void BufferPool::invalidateAll() {
    for (auto& f : frames_) {
        f.page_id   = INVALID_PAGE_ID;
        f.pin_count = 0;
        f.dirty     = false;
        f.last_used = 0;
    }
    table_.clear();
}

void BufferPool::flushAll() {
    for (auto& f : frames_)
        if (f.page_id != INVALID_PAGE_ID && f.dirty) {
            disk_->writePage(f.page_id, f.data.data());
            f.dirty = false;
        }
}

}  // namespace db
