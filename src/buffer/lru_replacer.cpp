#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) : capacity_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

/**
 * TODO: Student Implement
 */
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  std::lock_guard<std::mutex> lock(latch_);
  if (lru_list_.empty()) return false;
  *frame_id = lru_list_.back();   // 尾部 = 最久未使用
  lru_map_.erase(*frame_id);
  lru_list_.pop_back();
  return true;
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Pin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(latch_);
  if (lru_map_.count(frame_id)) {
    lru_list_.erase(lru_map_[frame_id]);
    lru_map_.erase(frame_id);
  }
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(latch_);
  if (lru_map_.count(frame_id)) return;  // 已在 replacer 中，不重复添加
  if (lru_list_.size() >= capacity_) return;
  lru_list_.push_front(frame_id);        // 头部 = 最新加入
  lru_map_[frame_id] = lru_list_.begin();
}

/**
 * TODO: Student Implement
 */
size_t LRUReplacer::Size() {
  std::lock_guard<std::mutex> lock(latch_);
  return lru_list_.size();
}