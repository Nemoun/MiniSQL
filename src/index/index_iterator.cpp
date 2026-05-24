#include "index/index_iterator.h"

#include "index/basic_comparator.h"
#include "index/generic_key.h"

IndexIterator::IndexIterator() = default;

IndexIterator::IndexIterator(page_id_t page_id, BufferPoolManager *bpm, int index)
    : current_page_id(page_id), item_index(index), buffer_pool_manager(bpm) {
  page = reinterpret_cast<LeafPage *>(buffer_pool_manager->FetchPage(current_page_id)->GetData());
}

IndexIterator::~IndexIterator() {
  if (current_page_id != INVALID_PAGE_ID)
    buffer_pool_manager->UnpinPage(current_page_id, false);
}

/**
 * TODO: Student Implement
 */
std::pair<GenericKey *, RowId> IndexIterator::operator*() {
  ASSERT(current_page_id != INVALID_PAGE_ID, "Dereference on end iterator.");
  return page->GetItem(item_index);
}

/**
 * TODO: Student Implement
 */
IndexIterator &IndexIterator::operator++() {
  ASSERT(current_page_id != INVALID_PAGE_ID, "Increment on end iterator.");
 
  item_index++;
  if (item_index < page->GetSize()) {
    // 当前页还有数据，直接推进
    return *this;
  }
 
  // 跳到下一个叶结点
  page_id_t next_page_id = page->GetNextPageId();
  // Unpin 当前页
  buffer_pool_manager->UnpinPage(current_page_id, false);
 
  if (next_page_id == INVALID_PAGE_ID) {
    // 已经是最后一个叶结点，设为 End 状态
    current_page_id = INVALID_PAGE_ID;
    page = nullptr;
    item_index = 0;
  } else {
    current_page_id = next_page_id;
    page = reinterpret_cast<LeafPage *>(
        buffer_pool_manager->FetchPage(current_page_id)->GetData());
    item_index = 0;
  }
 
  return *this;
}

bool IndexIterator::operator==(const IndexIterator &itr) const {
  return current_page_id == itr.current_page_id && item_index == itr.item_index;
}

bool IndexIterator::operator!=(const IndexIterator &itr) const {
  return !(*this == itr);
}