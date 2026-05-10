#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

frame_id_t BufferPoolManager::TryToFindFreePage() {
  frame_id_t frame_id;
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }
  if (replacer_->Victim(&frame_id)) {
    return frame_id;
  }
  return INVALID_PAGE_ID;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  // 1.     Search the page table for the requested page (P).
  // 1.1    If P exists, pin it and return it immediately.
  // 1.2    If P does not exist, find a replacement page (R) from either the free list or the replacer.
  //        Note that pages are always found from the free list first.
  // 2.     If R is dirty, write it back to the disk.
  // 3.     Delete R from the page table and insert P.
  // 4.     Update P's metadata, read in the page content from disk, and then return a pointer to P.
  scoped_lock<recursive_mutex> lock(latch_);
 
  // 1. 页已在 Buffer Pool 中
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t fid = it->second;
    replacer_->Pin(fid);           // 从 replacer 移除，使之不被替换
    pages_[fid].pin_count_++;
    return &pages_[fid];
  }
 
  // 2. 找一个空闲 frame
  frame_id_t fid = TryToFindFreePage();
  if (fid == INVALID_PAGE_ID) {
    return nullptr;  // Buffer Pool 全部被 pin，无法腾出空间
  }
 
  Page &frame = pages_[fid];
 
  // 3. 若该 frame 原来持有脏页，先写回磁盘
  if (frame.is_dirty_) {
    disk_manager_->WritePage(frame.page_id_, frame.data_);
    frame.is_dirty_ = false;
  }
 
  // 4. 从 page_table_ 中删除旧映射
  if (frame.page_id_ != INVALID_PAGE_ID) {
    page_table_.erase(frame.page_id_);
  }
 
  // 5. 建立新映射，读入页内容，更新元数据
  page_table_[page_id] = fid;
  frame.page_id_  = page_id;
  frame.pin_count_ = 1;
  frame.is_dirty_  = false;
  disk_manager_->ReadPage(page_id, frame.data_);
  replacer_->Pin(fid);
 
  return &frame;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  // 0.   Make sure you call AllocatePage!
  // 1.   If all the pages in the buffer pool are pinned, return nullptr.
  // 2.   Pick a victim page P from either the free list or the replacer. Always pick from the free list first.
  // 3.   Update P's metadata, zero out memory and add P to the page table.
  // 4.   Set the page ID output parameter. Return a pointer to P.
  scoped_lock<recursive_mutex> lock(latch_);
 
  // 找一个空闲 frame
  frame_id_t fid = TryToFindFreePage();
  if (fid == INVALID_PAGE_ID) {
    return nullptr;  // 所有页都被 pin，无法分配
  }
 
  Page &frame = pages_[fid];
 
  // 若该 frame 原来持有脏页，先写回磁盘
  if (frame.is_dirty_) {
    disk_manager_->WritePage(frame.page_id_, frame.data_);
    frame.is_dirty_ = false;
  }
 
  // 删除旧映射
  if (frame.page_id_ != INVALID_PAGE_ID) {
    page_table_.erase(frame.page_id_);
  }
 
  // 从磁盘分配一个新的逻辑页号
  page_id = AllocatePage();
 
  // 建立新映射，清空 frame 内存，更新元数据
  page_table_[page_id] = fid;
  frame.page_id_  = page_id;
  frame.pin_count_ = 1;
  frame.is_dirty_  = false;
  memset(frame.data_, 0, PAGE_SIZE);
  replacer_->Pin(fid);
 
  return &frame;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  // 0.   Make sure you call DeallocatePage!
  // 1.   Search the page table for the requested page (P).
  // 1.   If P does not exist, return true.
  // 2.   If P exists, but has a non-zero pin-count, return false. Someone is using the page.
  // 3.   Otherwise, P can be deleted. Remove P from the page table, reset its metadata and return it to the free list.
  scoped_lock<recursive_mutex> lock(latch_);
 
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // 页不在 Buffer Pool，直接释放磁盘空间
    DeallocatePage(page_id);
    return true;
  }
 
  frame_id_t fid = it->second;
  Page &frame = pages_[fid];
 
  // 有线程正在使用该页，无法删除
  if (frame.pin_count_ > 0) {
    return false;
  }
 
  // 从 replacer 中移除（防止后续被 Victim 选中）
  replacer_->Pin(fid);
 
  // 重置 frame 状态
  page_table_.erase(page_id);
  frame.page_id_  = INVALID_PAGE_ID;
  frame.pin_count_ = 0;
  frame.is_dirty_  = false;
  memset(frame.data_, 0, PAGE_SIZE);
 
  // 归还到 free_list_
  free_list_.push_back(fid);
 
  // 释放磁盘空间
  DeallocatePage(page_id);
 
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  scoped_lock<recursive_mutex> lock(latch_);
 
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;  // 页不在 Buffer Pool
  }
 
  frame_id_t fid = it->second;
  Page &frame = pages_[fid];
 
  if (frame.pin_count_ == 0) {
    return false;  // 已经是 0，不能再减
  }
 
  // 脏标记：只要调用者说脏就设置，不会清除已有的脏标记
  frame.is_dirty_ |= is_dirty;
  frame.pin_count_--;
 
  // pin_count 归零：允许 replacer 将来替换该 frame
  if (frame.pin_count_ == 0) {
    replacer_->Unpin(fid);
  }
 
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  scoped_lock<recursive_mutex> lock(latch_);
 
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;  // 页不在 Buffer Pool
  }
 
  frame_id_t fid = it->second;
  Page &frame = pages_[fid];
 
  disk_manager_->WritePage(page_id, frame.data_);
  frame.is_dirty_ = false;
 
  return true;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}