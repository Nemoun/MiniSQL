#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

/**
 * TODO: Student Implement
 */
uint32_t CatalogMeta::GetSerializedSize() const {
  // magic(4) + table_count(4) + index_count(4) + (table_id + page_id) * N + (index_id + page_id) * M
  return 4 + 4 + 4 + (4 + 4) * table_meta_pages_.size() + (4 + 4) * index_meta_pages_.size();
}

CatalogMeta::CatalogMeta() {}

/**
 * TODO: Student Implement
 */
CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  if (init) {
    // 首次创建：建立空的 CatalogMeta
    catalog_meta_ = CatalogMeta::NewInstance();
    next_table_id_ = 0;
    next_index_id_ = 0;
    // 将空 CatalogMeta 写入磁盘
    FlushCatalogMetaPage();
  } else {
    // 重新打开：从磁盘恢复 CatalogMeta
    auto *meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
    ASSERT(meta_page != nullptr, "Failed to fetch catalog meta page.");
    catalog_meta_ = CatalogMeta::DeserializeFrom(meta_page->GetData());
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);
 
    // 恢复所有 TableInfo
    for (auto &[table_id, page_id] : *catalog_meta_->GetTableMetaPages()) {
      if (LoadTable(table_id, page_id) != DB_SUCCESS) {
        LOG(ERROR) << "Failed to load table " << table_id;
      }
    }
    // 恢复所有 IndexInfo
    for (auto &[index_id, page_id] : *catalog_meta_->GetIndexMetaPages()) {
      if (LoadIndex(index_id, page_id) != DB_SUCCESS) {
        LOG(ERROR) << "Failed to load index " << index_id;
      }
    }
 
    // 初始化 next_table_id_ / next_index_id_
    next_table_id_ = catalog_meta_->GetNextTableId();
    next_index_id_ = catalog_meta_->GetNextIndexId();
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
  // 1. 检查重名
  if (table_names_.find(table_name) != table_names_.end()) {
    return DB_TABLE_ALREADY_EXIST;
  }
 
  // 2. 分配 table_id 和元数据页
  table_id_t table_id = next_table_id_++;
  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    return DB_FAILED;
  }
 
  // 3. 创建 TableHeap（会分配第一个数据页）
  // Schema 由 TableMetadata 拥有，需要深拷贝以避免与外部 schema 共享所有权
  TableSchema *owned_schema = Schema::DeepCopySchema(schema);
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, owned_schema, txn, log_manager_, lock_manager_);
  page_id_t first_page_id = table_heap->GetFirstPageId();
 
  // 4. 创建 TableMetadata 并序列化
  auto *table_meta = TableMetadata::Create(table_id, table_name, first_page_id, owned_schema);
  table_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);
 
  // 5. 创建 TableInfo，更新内存映射
  table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);
 
  table_names_[table_name] = table_id;
  tables_[table_id] = table_info;
  index_names_[table_name] = {};
 
  // 6. 更新 CatalogMeta 并写回磁盘
  catalog_meta_->table_meta_pages_[table_id] = meta_page_id;
  FlushCatalogMetaPage();
 
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto it = table_names_.find(table_name);
  if (it == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = tables_[it->second];
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  for (auto &[id, info] : tables_) {
    tables.push_back(info);
  }
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
  // 1. 检查表存在
  auto table_it = table_names_.find(table_name);
  if (table_it == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_id_t table_id = table_it->second;
  TableInfo *table_info = tables_[table_id];
 
  // 检查索引名不重复
  auto &idx_map = index_names_[table_name];
  if (idx_map.find(index_name) != idx_map.end()) {
    return DB_INDEX_ALREADY_EXIST;
  }
 
  // 2. 将列名转换为列下标
  std::vector<uint32_t> key_map;
  for (const auto &col_name : index_keys) {
    uint32_t col_idx;
    if (table_info->GetSchema()->GetColumnIndex(col_name, col_idx) != DB_SUCCESS) {
      return DB_COLUMN_NAME_NOT_EXIST;
    }
    key_map.push_back(col_idx);
  }
 
  // 3. 分配 index_id 和元数据页
  index_id_t index_id = next_index_id_++;
  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    return DB_FAILED;
  }
 
  // 4. 创建 IndexMetadata 并序列化
  auto *index_meta = IndexMetadata::Create(index_id, index_name, table_id, key_map);
  index_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);
 
  // 5. 创建 IndexInfo
  index_info = IndexInfo::Create();
  index_info->Init(index_meta, table_info, buffer_pool_manager_);
 
  // 6. 更新内存映射和 CatalogMeta
  idx_map[index_name] = index_id;
  indexes_[index_id] = index_info;
  catalog_meta_->index_meta_pages_[index_id] = meta_page_id;
  FlushCatalogMetaPage();
 
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  auto table_it = index_names_.find(table_name);
  if (table_it == index_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  auto idx_it = table_it->second.find(index_name);
  if (idx_it == table_it->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  index_info = indexes_.at(idx_it->second);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  auto table_it = index_names_.find(table_name);
  if (table_it == index_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  for (auto &[name, id] : table_it->second) {
    indexes.push_back(indexes_.at(id));
  }
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropTable(const string &table_name) {
  auto it = table_names_.find(table_name);
  if (it == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_id_t table_id = it->second;
 
  // 删除该表上的所有索引
  auto idx_it = index_names_.find(table_name);
  if (idx_it != index_names_.end()) {
    // 收集需要删除的索引名（避免在迭代中修改）
    std::vector<std::string> to_drop;
    for (auto &[idx_name, _] : idx_it->second) {
      to_drop.push_back(idx_name);
    }
    for (auto &idx_name : to_drop) {
      DropIndex(table_name, idx_name);
    }
    index_names_.erase(idx_it);
  }
 
  // 释放表的元数据页
  auto meta_it = catalog_meta_->table_meta_pages_.find(table_id);
  if (meta_it != catalog_meta_->table_meta_pages_.end()) {
    buffer_pool_manager_->DeletePage(meta_it->second);
    catalog_meta_->table_meta_pages_.erase(meta_it);
  }
 
  // 释放堆表数据页并删除 TableInfo
  TableInfo *table_info = tables_[table_id];
  table_info->GetTableHeap()->FreeTableHeap();
  delete table_info;
 
  tables_.erase(table_id);
  table_names_.erase(it);
 
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto table_it = index_names_.find(table_name);
  if (table_it == index_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  auto idx_it = table_it->second.find(index_name);
  if (idx_it == table_it->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }
 
  index_id_t index_id = idx_it->second;
 
  // 销毁 B+ 树数据
  IndexInfo *index_info = indexes_[index_id];
  index_info->GetIndex()->Destroy();
 
  // 释放索引元数据页
  catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, index_id);
 
  // 删除 IndexInfo
  delete index_info;
  indexes_.erase(index_id);
  table_it->second.erase(idx_it);
 
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::FlushCatalogMetaPage() const {
  auto *meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  if (meta_page == nullptr) {
    return DB_FAILED;
  }
  catalog_meta_->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }
 
  // 反序列化 TableMetadata
  TableMetadata *table_meta = nullptr;
  TableMetadata::DeserializeFrom(page->GetData(), table_meta);
  buffer_pool_manager_->UnpinPage(page_id, false);
 
  if (table_meta == nullptr) {
    return DB_FAILED;
  }
 
  // 重建 TableHeap（用已有的 first_page_id）
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, table_meta->GetFirstPageId(),
                                       table_meta->GetSchema(), log_manager_, lock_manager_);
 
  // 建立 TableInfo
  TableInfo *table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);
 
  // 注册到内存映射
  std::string table_name = table_meta->GetTableName();
  table_names_[table_name] = table_id;
  tables_[table_id] = table_info;
  // 确保 index_names_ 中有该表的条目
  if (index_names_.find(table_name) == index_names_.end()) {
    index_names_[table_name] = {};
  }
 
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }
 
  // 反序列化 IndexMetadata
  IndexMetadata *index_meta = nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(), index_meta);
  buffer_pool_manager_->UnpinPage(page_id, false);
 
  if (index_meta == nullptr) {
    return DB_FAILED;
  }
 
  // 找到对应的 TableInfo
  auto table_it = tables_.find(index_meta->GetTableId());
  if (table_it == tables_.end()) {
    delete index_meta;
    return DB_TABLE_NOT_EXIST;
  }
  TableInfo *table_info = table_it->second;
 
  // 建立 IndexInfo
  IndexInfo *index_info = IndexInfo::Create();
  index_info->Init(index_meta, table_info, buffer_pool_manager_);
 
  // 注册到内存映射
  std::string table_name = table_info->GetTableName();
  std::string index_name = index_meta->GetIndexName();
  indexes_[index_id] = index_info;
  index_names_[table_name][index_name] = index_id;
 
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto it = tables_.find(table_id);
  if (it == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = it->second;
  return DB_SUCCESS;
}