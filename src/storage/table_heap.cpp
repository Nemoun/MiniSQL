#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  // 若 row 本身就超过一页大小，无法存储
  if (row.GetSerializedSize(schema_) >= PAGE_SIZE) {
    return false;
  }
 
  // 从第一页开始遍历，寻找第一个有足够空间的页（First Fit）
  page_id_t cur_page_id = first_page_id_;
  while (cur_page_id != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(cur_page_id));
    if (page == nullptr) return false;
 
    page->WLatch();
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      // 插入成功
      page->WUnlatch();
      buffer_pool_manager_->UnpinPage(cur_page_id, true);
      return true;
    }
    page->WUnlatch();
 
    // 当前页空间不足，移向下一页
    page_id_t next_page_id = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(cur_page_id, false);
 
    if (next_page_id == INVALID_PAGE_ID) {
      // 已到达链表末尾，需要新建一个 TablePage
      page_id_t new_page_id;
      auto new_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(new_page_id));
      if (new_page == nullptr) return false;  // Buffer Pool 已满
 
      // 初始化新页，并将其追加到链表末尾
      new_page->Init(new_page_id, cur_page_id, log_manager_, txn);
 
      // 更新旧末尾页的 next_page_id
      auto prev_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(cur_page_id));
      if (prev_page != nullptr) {
        prev_page->WLatch();
        prev_page->SetNextPageId(new_page_id);
        prev_page->WUnlatch();
        buffer_pool_manager_->UnpinPage(cur_page_id, true);
      }
 
      // 在新页中插入
      new_page->WLatch();
      bool ok = new_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);
      new_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(new_page_id, true);
      return ok;
    }
 
    cur_page_id = next_page_id;
  }
 
  return false;
}

bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // If the page could not be found, then abort the recovery.
  if (page == nullptr) {
    return false;
  }
  // Otherwise, mark the tuple as deleted.
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

/**
 * TODO: Student Implement
 */
bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) return false;
 
  // 先获取旧记录（供日志记录等使用）
  Row old_row(rid);
  page->RLatch();
  bool got = page->GetTuple(&old_row, schema_, txn, lock_manager_);
  page->RUnlatch();
  if (!got) {
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), false);
    return false;
  }
 
  page->WLatch();
  bool updated = page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), updated);
 
  if (updated) {
    // 原地更新成功，row.rid_ 继承原 rid
    row.SetRowId(rid);
    return true;
  }
 
  // 原地更新失败（空间不足）：逻辑删除旧记录 + 插入新记录
  MarkDelete(rid, txn);
  ApplyDelete(rid, txn);
  return InsertTuple(row, txn);
}

/**
 * TODO: Student Implement
 */
void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  ASSERT(page != nullptr, "Failed to fetch page for ApplyDelete.");
  page->WLatch();
  page->ApplyDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  // Rollback to delete.
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

/**
 * TODO: Student Implement
 */
bool TableHeap::GetTuple(Row *row, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if (page == nullptr) return false;
 
  page->RLatch();
  bool ok = page->GetTuple(row, schema_, txn, lock_manager_);
  page->RUnlatch();
  buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);
  return ok;
}

void TableHeap::DeleteTable(page_id_t page_id) {
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));  // 删除table_heap
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

/**
 * TODO: Student Implement
 */
TableIterator TableHeap::Begin(Txn *txn) {
  // 从第一页找第一条有效记录
  page_id_t cur_page_id = first_page_id_;
  while (cur_page_id != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(cur_page_id));
    if (page == nullptr) break;
 
    RowId first_rid;
    if (page->GetFirstTupleRid(&first_rid)) {
      buffer_pool_manager_->UnpinPage(cur_page_id, false);
      return TableIterator(this, first_rid, txn);
    }
 
    page_id_t next_page_id = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(cur_page_id, false);
    cur_page_id = next_page_id;
  }
 
  // 堆表为空
  return End();
}

/**
 * TODO: Student Implement
 */
TableIterator TableHeap::End() {
  return TableIterator(nullptr, RowId(INVALID_PAGE_ID, 0), nullptr);
}
