#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {

    if (leaf_max_size == UNDEFINED_SIZE) {
      leaf_max_size_ = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (KM.GetKeySize() + sizeof(RowId));
    }
    if (internal_max_size == UNDEFINED_SIZE) {
      internal_max_size_ = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (KM.GetKeySize() + sizeof(page_id_t));
    }
    // get index roots page
    auto *index_roots_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID));

    bool found = index_roots_page->GetRootId(index_id_, &this->root_page_id_);
    if (!found) {
      // insert a new root
      root_page_id_ = INVALID_PAGE_ID; // invalid page id, update when start new tree
      index_roots_page->Insert(index_id_, root_page_id_);
    }

    buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
    // 立即将 IndexRootsPage 写回磁盘，确保持久化
    // （防止崩溃后丢失索引根页映射，导致下次启动时索引元数据损坏）
    buffer_pool_manager_->FlushPage(INDEX_ROOTS_PAGE_ID);
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  if (IsEmpty()) {
    return;
  }
  if (current_page_id == INVALID_PAGE_ID) {
    current_page_id = root_page_id_;
  }
  BPlusTreePage *node = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(current_page_id)->GetData());
  if (node->IsLeafPage()) {
    buffer_pool_manager_->DeletePage(current_page_id);
  } else {
    auto *internal_node = reinterpret_cast<InternalPage *>(node);
    for (int i = 0; i < internal_node->GetSize(); i++) {
      Destroy(internal_node->ValueAt(i));
    }
    buffer_pool_manager_->DeletePage(current_page_id);
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  return this->root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (IsEmpty()) {
    return false;
  }
  Page *page = FindLeafPage(key, root_page_id_);
  if (page == nullptr) {
    LOG(WARNING) << "Failed to find leaf page";
    return false;
  }
  auto *leaf_page = reinterpret_cast<LeafPage *>(page->GetData());
  RowId row_id;
  if (leaf_page->Lookup(key, row_id, processor_)) {
    result.push_back(row_id);
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
    return true;
  }
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  page_id_t page_id;
  Page* page = this->buffer_pool_manager_->NewPage(page_id);
  if (page_id == INVALID_PAGE_ID) {
    LOG(ERROR) << "Failed to allocate new page";
    return;
  }
  
  this->root_page_id_ = page_id;
  UpdateRootPageId(0);
  auto *root_page = reinterpret_cast<LeafPage *>(page->GetData());
  root_page->Init(page_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  root_page->Insert(key, value, processor_); // insert as a leaf page
  buffer_pool_manager_->UnpinPage(page_id, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  if (IsEmpty()) {
    LOG(ERROR) << "Cannot call InsertIntoLeaf on an empty tree";
    return false;
  }

  Page *page = FindLeafPage(key,root_page_id_);
  auto *leaf_page = reinterpret_cast<LeafPage *>(page->GetData());

  int size = leaf_page->GetSize();
  
  int new_size = leaf_page->Insert(key, value, processor_);
  if (new_size == size) {
    // 说明重复插入了
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
    return false;
  }
  // for debug
  if (new_size > leaf_page->GetMaxSize()) {
    LOG(WARNING) << "Unexpected size of leaf node("<< leaf_page->GetPageId() << ")";
  }
  // split if overflow
  if (new_size >= leaf_page->GetMaxSize()) {
    auto *recipient = Split(leaf_page, transaction);
    #ifdef ENABLE_BPT_DEBUG
    LOG(INFO) << "Leaf node(" << leaf_page->GetPageId() << ") is overflow, split to new node(" << recipient->GetPageId() << ")";
    #endif
    buffer_pool_manager_->UnpinPage(recipient->GetPageId(), true);
    // 对于叶子节点来说，需要提升的key就是右边节点的第一个key
    GenericKey *to_promote = recipient->KeyAt(0);
    InsertIntoParent(leaf_page, to_promote, recipient, transaction);
  }
  
  buffer_pool_manager_->UnpinPage(page->GetPageId(), true);
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
  // 新页
  page_id_t page_id;
  Page *page = this->buffer_pool_manager_->NewPage(page_id);
  if (page_id == INVALID_PAGE_ID) {
    LOG(ERROR) << "Failed to allocate new page";
    return nullptr;
  }

  auto *recipient = reinterpret_cast<InternalPage *>(page->GetData());
  recipient->Init(page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);

  // 移动一半
  node->MoveHalfTo(recipient, buffer_pool_manager_);
  return recipient; // The caller should remember to unpin
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
  // 新页
  page_id_t page_id;
  Page *page = this->buffer_pool_manager_->NewPage(page_id);
  if (page_id == INVALID_PAGE_ID) {
    LOG(ERROR) << "Failed to allocate new page";
    return nullptr;
  }

  auto *recipient = reinterpret_cast<LeafPage *>(page->GetData());
  recipient->Init(page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);

  // 移动一半
  node->MoveHalfTo(recipient);
  recipient->SetNextPageId(node->GetNextPageId());
  node->SetNextPageId(recipient->GetPageId());
  return recipient; // The caller should remember to unpin
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node, Txn *transaction) {
  // 如果想要往根节点的上方加入新节点，需要先创建一个新根节点
  if (old_node->IsRootPage()) {
    auto *page = buffer_pool_manager_->NewPage(root_page_id_);
    if (root_page_id_ == INVALID_PAGE_ID) {
      LOG(ERROR) << "Failed to allocate new page";
      return;
    }
    #ifdef ENABLE_BPT_DEBUG
    LOG(INFO) << "Create a new root(" << page->GetPageId() << "), with two children(" << old_node->GetPageId() << ", " << new_node->GetPageId() << ")";
    #endif
    auto *new_root = reinterpret_cast<InternalPage *>(page->GetData());
    new_root->Init(root_page_id_, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(root_page_id_);
    new_node->SetParentPageId(root_page_id_);
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(root_page_id_, true);
    return;
  }

  auto par_page = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(old_node->GetParentPageId())->GetData());
  int old_size = par_page->GetSize();
  int new_size = par_page->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
  if (new_size == old_size) {
    // 说明重复插入了
    buffer_pool_manager_->UnpinPage(par_page->GetPageId(), false);
    return;
  }
  // for debug
  if (new_size > par_page->GetMaxSize()) {
    LOG(WARNING) << "Unexpected size of internal node (" << par_page->GetPageId() << ")";
  }
  if (new_size >= par_page->GetMaxSize()) {
    auto *recipient = Split(par_page, transaction);
    #ifdef ENABLE_BPT_DEBUG
    LOG(INFO) << "Internal node (" << par_page->GetPageId() << ") is overflow, split to (" << recipient->GetPageId() << ")";
    #endif
    // 对于内部节点来说，需要提升的key就是右边节点的第一个key（分裂之后成为了无效Key）
    GenericKey *to_promote = recipient->KeyAt(0);
    InsertIntoParent(par_page, to_promote, recipient, transaction);
    buffer_pool_manager_->UnpinPage(recipient->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(par_page->GetPageId(), true);
  
  return;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
  // 如果为空，直接返回
  if (IsEmpty()) {
    LOG(ERROR) << "Cannot call Remove on an empty tree";
    return;
  }
  Page *page = FindLeafPage(key, root_page_id_);
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int size = leaf->RemoveAndDeleteRecord(key, processor_);

  // if underflow, try to coalesce or redistribute
  if (size < leaf->GetMinSize()) {
    #ifdef ENABLE_BPT_DEBUG
    LOG(INFO) << "Leaf node(" << leaf->GetPageId() << ") is underflow, try to coalesce or redistribute";
    #endif
    CoalesceOrRedistribute(leaf, transaction);
    return;
  }

  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
  bool BPlusTree::CoalesceOrRedistribute(/* into */ N *&node, Txn *transaction) {
  // For caller: remember to unpin the page, and delete if return true

  if (node->IsRootPage()) {
    if(AdjustRoot(node)) {
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      buffer_pool_manager_->DeletePage(node->GetPageId());
      node = nullptr;
      return false;
    }
    buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
    return false;
  }

  page_id_t parent_page_id = node->GetParentPageId();
  Page *parent_page = buffer_pool_manager_->FetchPage(parent_page_id);
  if (parent_page == nullptr) {
    LOG(ERROR) << "Failed to fetch parent page";
    return false;
  }
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  int index = parent->ValueIndex(node->GetPageId());

  page_id_t sibling_page_id = index == 0 ? parent->ValueAt(1) : parent->ValueAt(index - 1);
  Page *sibling_page = buffer_pool_manager_->FetchPage(sibling_page_id);
  auto *sibling = reinterpret_cast<N *>(sibling_page->GetData());

  // case 1: redistribute
  if (node->GetSize() + sibling->GetSize() >= node->GetMaxSize()) {
    // this will staticallydispatch to 2 overloaded functions of Redistribute()
    Redistribute(sibling, node, index);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    buffer_pool_manager_->UnpinPage(parent_page_id, true);
    return false;
  }

  // case 2: coalesce
  // node is invalid after call Coalesce(), should not use
  // return true to indicate caller that node should be deleted
  Coalesce(sibling, node, parent, index, transaction); 
  return false;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  #ifdef ENABLE_BPT_DEBUG
  LOG(INFO) << "Coalesce neighbor_page(" << neighbor_node->GetPageId() << "), node(" << node->GetPageId() << ")";
  #endif
  if (index == 0) { 
    // if neighbor_node is the right sibling
    node->SetNextPageId(neighbor_node->GetNextPageId());
    neighbor_node->MoveAllTo(node);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), false);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    buffer_pool_manager_->DeletePage(neighbor_node->GetPageId());
    parent->Remove(1);
  } else {
    // if neighbor_node is the left sibling
    neighbor_node->SetNextPageId(node->GetNextPageId());
    node->MoveAllTo(neighbor_node);
    parent->Remove(index);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), false);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    buffer_pool_manager_->DeletePage(node->GetPageId());
  }
  neighbor_node = nullptr;
  node = nullptr;

  if (parent->GetSize() < parent->GetMinSize()) {
    CoalesceOrRedistribute(parent, transaction);
    return false;
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  parent = nullptr;
  return false;
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  #ifdef ENABLE_BPT_DEBUG
  LOG(INFO) << "Coalesce neighbor_page(" << neighbor_node->GetPageId() << "), node(" << node->GetPageId() << ")";
  #endif
  if (index == 0) {
    // if neighbor_node is the right sibling
    neighbor_node->MoveAllTo(node, parent->KeyAt(1), buffer_pool_manager_);
    parent->Remove(1);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), false);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    buffer_pool_manager_->DeletePage(neighbor_node->GetPageId());
  } else {
    // if neighbor_node is the left sibling
    node->MoveAllTo(neighbor_node, parent->KeyAt(index), buffer_pool_manager_);
    parent->Remove(index);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), false);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    buffer_pool_manager_->DeletePage(node->GetPageId());
  }
  neighbor_node = nullptr;
  node = nullptr;

  if (parent->GetSize() < parent->GetMinSize()) {
    CoalesceOrRedistribute(parent, transaction);
    return false;
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  parent = nullptr;
  return false;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  #ifdef ENABLE_BPT_DEBUG
  LOG(INFO) << "Redistribute neighbor_page(" << neighbor_node->GetPageId() << "), node(" << node->GetPageId() << ")";
  #endif
  Page *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
  if (parent_page == nullptr) {
    LOG(ERROR) << "Failed to fetch parent page";
    return;
  }
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  
  if (index == 0) { // if neighbor_node is the right sibling
    neighbor_node->MoveFirstToEndOf(node);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } // if neighbor_node is the left sibling
  else {
    neighbor_node->MoveLastToFrontOf(node);
    parent->SetKeyAt(index, node->KeyAt(0));
  }
  buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  #ifdef ENABLE_BPT_DEBUG
  LOG(INFO) << "Redistribute neighbor_page(" << neighbor_node->GetPageId() << "), node(" << node->GetPageId() << ")";
  #endif
  page_id_t parent_page_id = node->GetParentPageId();
  Page *parent_page = buffer_pool_manager_->FetchPage(parent_page_id);
  if (parent_page == nullptr) {
    LOG(ERROR) << "Failed to fetch parent page";
    return;
  }
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  if (index == 0) { // if neighbor_node is the right sibling
    auto *middle_key = parent->KeyAt(1);
    neighbor_node->MoveFirstToEndOf(node, middle_key, buffer_pool_manager_);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } 
  else {  // if neighbor_node is the left sibling
    auto *middle_key = parent->KeyAt(index);
    neighbor_node->MoveLastToFrontOf(node, middle_key, buffer_pool_manager_);
    parent->SetKeyAt(index, neighbor_node->KeyAt(neighbor_node->GetSize()));
  }
  buffer_pool_manager_->UnpinPage(parent_page_id, true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  int size = old_root_node->GetSize();

  // case 1: 删除根节点之后，根节点只剩一个孩子，提升成新的根
  if (size == 1 && !old_root_node->IsLeafPage()) {
    InternalPage *old_page = reinterpret_cast<InternalPage *>(old_root_node);
    page_id_t child_page_id = old_page->RemoveAndReturnOnlyChild();
    this->root_page_id_ = child_page_id;
    UpdateRootPageId(0);
    Page *child_page = buffer_pool_manager_->FetchPage(child_page_id);
    if (child_page == nullptr) {
      LOG(ERROR) << "Failed to fetch page";
      return false;
    }
    auto *new_root = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
    new_root->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(child_page_id, true);
    return true;
  }

  // case 2: 删除了整个B+树的最后一个节点
  if (size == 0 && old_root_node->IsLeafPage()) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    return true;
  }

  // otherwise, no need to delete
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  if (IsEmpty()) {
    return End();
  }
  Page *page = FindLeafPage(nullptr, root_page_id_, true);
  IndexIterator itr(page->GetPageId(), buffer_pool_manager_, 0);
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  return itr;
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) {
    return End();
  }
  Page *page = FindLeafPage(key, root_page_id_, false);
  if (page == nullptr) {
    return End();
  }
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int index = leaf->KeyIndex(key, processor_);
  IndexIterator itr(page->GetPageId(), buffer_pool_manager_, index);
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  return itr;
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  return IndexIterator();
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  page_id_t cur_page_id = page_id;

  while (true) {
    Page* cur_page_raw = buffer_pool_manager_->FetchPage(cur_page_id);
    auto *cur_page = reinterpret_cast<BPlusTreePage *>(cur_page_raw->GetData());

    // if is leaf page break
    if (cur_page->IsLeafPage()) {
      return cur_page_raw;
      // NOTE: must unpin the returned page after use
    }

    auto *in_page = reinterpret_cast<InternalPage *>(cur_page);

    page_id_t next_page_id;
    if (leftMost) {
      next_page_id = in_page->ValueAt(0);
    } else {
      next_page_id = in_page->Lookup(key, processor_);
    }
    if (next_page_id == INVALID_PAGE_ID) {
      buffer_pool_manager_->UnpinPage(cur_page_id, false);
      return nullptr;
    }
    buffer_pool_manager_->UnpinPage(cur_page_id, false);
    cur_page_id = next_page_id;
  }
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
  // get index roots page
  auto *index_roots_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID));

  // insert or update index roots page
  if (insert_record) {
    index_roots_page->Insert(index_id_, root_page_id_);
  } else {
    index_roots_page->Update(index_id_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
  // 立即将 IndexRootsPage 写回磁盘，确保持久化
  buffer_pool_manager_->FlushPage(INDEX_ROOTS_PAGE_ID);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}