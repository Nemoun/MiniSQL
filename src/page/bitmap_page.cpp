#include "page/bitmap_page.h"

#include "glog/logging.h"

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  if (page_allocated_ >= GetMaxSupportedSize()) {
    return false;  // 位图已满，无法分配
  }
  // 从 next_free_page_ 开始查找空闲页
  page_offset = next_free_page_;
  // 标记该页为已分配（将对应 bit 设为 1）
  uint32_t byte_index = page_offset / 8;
  uint8_t  bit_index  = page_offset % 8;
  bytes[byte_index] |= (1 << bit_index);
  page_allocated_++;
  // 更新 next_free_page_（向后扫描找下一个空闲页）
  uint32_t max = GetMaxSupportedSize();
  for (uint32_t i = 1; i <= max; i++) {
    uint32_t next = (page_offset + i) % max;
    if (IsPageFreeLow(next / 8, next % 8)) {
      next_free_page_ = next;
      break;
    }
  }
  return true;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  if (page_offset >= GetMaxSupportedSize()) return false;
  if (IsPageFreeLow(page_offset / 8, page_offset % 8)) {
    return false;  // 该页本来就是空闲的，不能重复释放
  }
  // 将对应 bit 清 0
  bytes[page_offset / 8] &= ~(1 << (page_offset % 8));
  page_allocated_--;
  // 更新 next_free_page_（取较小的那个，方便下次快速找到）
  if (page_offset < next_free_page_) {
    next_free_page_ = page_offset;
  }
  return true;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  return IsPageFreeLow(page_offset / 8, page_offset % 8);
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  return (bytes[byte_index] & (1 << bit_index)) == 0;
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;