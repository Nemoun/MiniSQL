#include "storage/table_iterator.h"

#include "common/macros.h"
#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), row_(new Row(rid)), txn_(txn) {}

TableIterator::TableIterator(const TableIterator &other) {

}

TableIterator::~TableIterator() {
  delete row_;
}

bool TableIterator::operator==(const TableIterator &itr) const {
  return row_->GetRowId() == itr.row_->GetRowId();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this == itr);
}

const Row &TableIterator::operator*() {
  ASSERT(row_ != nullptr && row_->GetRowId().GetPageId() != INVALID_PAGE_ID,
         "Dereference on invalid iterator.");
  // 先清空旧 fields，再从堆表重新读取
  row_->destroy();
  table_heap_->GetTuple(row_, txn_);
  return *row_;
}

Row *TableIterator::operator->() {
  // 与 operator* 保持一致，触发一次数据读取后返回指针
  return &(operator*());
}

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  if (this != &itr) {
    table_heap_ = itr.table_heap_;
    txn_ = itr.txn_;
    delete row_;
    row_ = new Row(*itr.row_);
  }
  return *this;
}

// ++iter
TableIterator &TableIterator::operator++() {
  // 当前已是 End，直接返回
  auto cur_rid = row_->GetRowId();
  if (cur_rid.GetPageId() == INVALID_PAGE_ID) {
    return *this;
  }
 
  // 取当前页
  auto page = reinterpret_cast<TablePage *>(
      table_heap_->buffer_pool_manager_->FetchPage(cur_rid.GetPageId()));
  ASSERT(page != nullptr, "Failed to fetch current page in iterator++.");
 
  RowId next_rid;
  // 在当前页内寻找下一条记录
  if (page->GetNextTupleRid(cur_rid, &next_rid)) {
    // 找到了，直接更新 RowId
    table_heap_->buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
    row_->SetRowId(next_rid);
    return *this;
  }
 
  // 当前页没有更多记录，跳到下一页
  page_id_t next_page_id = page->GetNextPageId();
  table_heap_->buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
 
  while (next_page_id != INVALID_PAGE_ID) {
    auto next_page = reinterpret_cast<TablePage *>(
        table_heap_->buffer_pool_manager_->FetchPage(next_page_id));
    ASSERT(next_page != nullptr, "Failed to fetch next page in iterator++.");
 
    RowId first_rid;
    if (next_page->GetFirstTupleRid(&first_rid)) {
      // 在这一页找到了第一条有效记录
      table_heap_->buffer_pool_manager_->UnpinPage(next_page_id, false);
      row_->SetRowId(first_rid);
      return *this;
    }
 
    // 这一页也没有记录，继续往后
    next_page_id = next_page->GetNextPageId();
    table_heap_->buffer_pool_manager_->UnpinPage(next_page->GetTablePageId(), false);
  }
 
  // 整个堆表遍历完毕，设为 End 状态
  row_->SetRowId(RowId(INVALID_PAGE_ID, 0));
  return *this;
}

// iter++
TableIterator TableIterator::operator++(int) {
  TableIterator old(*this);
  ++(*this);
  return old;
}
