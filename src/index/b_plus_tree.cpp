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
  int key_size = KM.GetKeySize();
  // 自动计算 max_size
  if (leaf_max_size_ == UNDEFINED_SIZE) {
    leaf_max_size_ = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (key_size + sizeof(RowId));
  }
  if (internal_max_size_ == UNDEFINED_SIZE) {
    internal_max_size_ = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (key_size + sizeof(page_id_t));
  }
 
  // 从 INDEX_ROOTS_PAGE 恢复 root_page_id
  auto *roots_page = reinterpret_cast<IndexRootsPage *>(
      buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID));
  if (!roots_page->GetRootId(index_id_, &root_page_id_)) {
    root_page_id_ = INVALID_PAGE_ID;
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  if (current_page_id == INVALID_PAGE_ID) {
    current_page_id = root_page_id_;
  }
  if (current_page_id == INVALID_PAGE_ID) return;
 
  auto *page = buffer_pool_manager_->FetchPage(current_page_id);
  if (page == nullptr) return;
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
 
  if (!node->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(node);
    for (int i = 0; i < internal->GetSize(); ++i) {
      Destroy(internal->ValueAt(i));
    }
  }
  buffer_pool_manager_->UnpinPage(current_page_id, false);
  buffer_pool_manager_->DeletePage(current_page_id);
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  return root_page_id_ == INVALID_PAGE_ID;
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
  if (IsEmpty()) return false;
 
  Page *leaf_page = FindLeafPage(key);
  if (leaf_page == nullptr) return false;
 
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
 
  if (found) {
    result.push_back(value);
  }
  return found;
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
  page_id_t new_page_id;
  auto *page = buffer_pool_manager_->NewPage(new_page_id);
  ASSERT(page != nullptr, "Out of memory when starting new B+ tree.");
 
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  leaf->Init(new_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  leaf->Insert(key, value, processor_);
 
  root_page_id_ = new_page_id;
  UpdateRootPageId(1);  // insert_record=1：在 INDEX_ROOTS_PAGE 中新建记录
 
  buffer_pool_manager_->UnpinPage(new_page_id, true);
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
  Page *leaf_page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
 
  RowId dummy;
  if (leaf->Lookup(key, dummy, processor_)) {
    // 重复 key
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }
 
  int new_size = leaf->Insert(key, value, processor_);
  if (new_size >= leaf->GetMaxSize()) {
    // 叶结点溢出，分裂
    LeafPage *sibling = Split(leaf, transaction);
    // 将分裂出的右兄弟的第一个 key 上推到父结点
    InsertIntoParent(leaf, sibling->KeyAt(0), sibling, transaction);
    buffer_pool_manager_->UnpinPage(sibling->GetPageId(), true);
  }
 
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
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
  page_id_t new_page_id;
  auto *new_page = buffer_pool_manager_->NewPage(new_page_id);
  ASSERT(new_page != nullptr, "Out of memory when splitting internal node.");
 
  auto *sibling = reinterpret_cast<InternalPage *>(new_page->GetData());
  sibling->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);
 
  node->MoveHalfTo(sibling, buffer_pool_manager_);
 
  return sibling;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
  page_id_t new_page_id;
  auto *new_page = buffer_pool_manager_->NewPage(new_page_id);
  ASSERT(new_page != nullptr, "Out of memory when splitting leaf.");
 
  auto *sibling = reinterpret_cast<LeafPage *>(new_page->GetData());
  sibling->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);
 
  node->MoveHalfTo(sibling);
 
  // 维护叶链表：node -> sibling -> node 原来的 next
  sibling->SetNextPageId(node->GetNextPageId());
  node->SetNextPageId(new_page_id);
 
  return sibling;
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
  if (old_node->IsRootPage()) {
    // 创建新根
    page_id_t new_root_id;
    auto *new_root_page = buffer_pool_manager_->NewPage(new_root_id);
    ASSERT(new_root_page != nullptr, "Out of memory when creating new root.");
 
    auto *new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
    new_root->Init(new_root_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
 
    // 更新两个子结点的父指针
    old_node->SetParentPageId(new_root_id);
    new_node->SetParentPageId(new_root_id);
 
    root_page_id_ = new_root_id;
    UpdateRootPageId(0);  // update
 
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    return;
  }
 
  // 找父结点
  page_id_t parent_id = old_node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
 
  new_node->SetParentPageId(parent_id);
 
  int new_size = parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
 
  if (new_size > parent->GetMaxSize()) {
    // 父结点溢出，递归分裂
    InternalPage *parent_sibling = Split(parent, transaction);
    // 上推 parent_sibling 的第一个有效 key
    InsertIntoParent(parent, parent_sibling->KeyAt(0), parent_sibling, transaction);
    buffer_pool_manager_->UnpinPage(parent_sibling->GetPageId(), true);
  }
 
  buffer_pool_manager_->UnpinPage(parent_id, true);
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
  if (IsEmpty()) return;
 
  Page *leaf_page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
 
  int old_size = leaf->GetSize();
  int new_size = leaf->RemoveAndDeleteRecord(key, processor_);
 
  if (new_size == old_size) {
    // key 不存在
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return;
  }
 
  // 检查是否需要合并或借用
  bool should_delete = CoalesceOrRedistribute(leaf, transaction);
  if (should_delete) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    buffer_pool_manager_->DeletePage(leaf->GetPageId());
  } else {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
  }
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  // 根结点特殊处理
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }
 
  // 元素足够，无需操作
  if (node->GetSize() >= node->GetMinSize()) {
    return false;
  }
 
  // 找父结点
  page_id_t parent_id = node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
 
  // 找 node 在父结点中的下标
  int node_idx = parent->ValueIndex(node->GetPageId());
 
  // 优先选左兄弟（index-1），若没有则选右兄弟（index+1）
  int sibling_idx = (node_idx > 0) ? node_idx - 1 : node_idx + 1;
  page_id_t sibling_id = parent->ValueAt(sibling_idx);
  auto *sibling_page = buffer_pool_manager_->FetchPage(sibling_id);
  auto *sibling = reinterpret_cast<N *>(sibling_page->GetData());
 
  bool result;
  
  if (sibling->GetSize() + node->GetSize() <= node->GetMaxSize()) {
    // 合并
    if (node_idx == 0) {
      // node 是左边，sibling 是右边 → 把 sibling 合并到 node（交换角色）
      std::swap(sibling, node);
      sibling_idx = node_idx + 1;
    }
    // 此时 node 在右，sibling 在左，将 node 合并到 sibling
    result = Coalesce(sibling, node, parent, node_idx, transaction);
  } else {
    // 借用
    Redistribute(sibling, node, node_idx);
    result = false;
  }
 
  buffer_pool_manager_->UnpinPage(sibling_id, true);
  buffer_pool_manager_->UnpinPage(parent_id, true);
  return result;
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
  node->MoveAllTo(neighbor_node);
  parent->Remove(index);
  return CoalesceOrRedistribute(parent, transaction);
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  GenericKey *middle_key = parent->KeyAt(index);
  node->MoveAllTo(neighbor_node, middle_key, buffer_pool_manager_);
  parent->Remove(index);
  return CoalesceOrRedistribute(parent, transaction);
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
  page_id_t parent_id = node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
 
  if (index == 0) {
    // node 在左，neighbor 在右，从 neighbor 借第一个给 node
    neighbor_node->MoveFirstToEndOf(node);
    // 更新父结点中 neighbor 对应的分隔键
    int neighbor_idx = parent->ValueIndex(neighbor_node->GetPageId());
    parent->SetKeyAt(neighbor_idx, neighbor_node->KeyAt(0));
  } else {
    // node 在右，neighbor 在左，从 neighbor 借最后一个给 node
    neighbor_node->MoveLastToFrontOf(node);
    // 更新父结点中 node 对应的分隔键（node 新的第一个 key）
    int node_idx = parent->ValueIndex(node->GetPageId());
    parent->SetKeyAt(node_idx, node->KeyAt(0));
  }
 
  buffer_pool_manager_->UnpinPage(parent_id, true);
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  page_id_t parent_id = node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
 
  if (index == 0) {
    int neighbor_idx = parent->ValueIndex(neighbor_node->GetPageId());
    GenericKey *middle_key = parent->KeyAt(neighbor_idx);
    // neighbor 的第一个有效 key（index=1）将上升到父结点
    neighbor_node->MoveFirstToEndOf(node, middle_key, buffer_pool_manager_);
    parent->SetKeyAt(neighbor_idx, neighbor_node->KeyAt(0));
  } else {
    // node 在右，neighbor 在左
    int node_idx = parent->ValueIndex(node->GetPageId());
    GenericKey *middle_key = parent->KeyAt(node_idx);
    neighbor_node->MoveLastToFrontOf(node, middle_key, buffer_pool_manager_);
    parent->SetKeyAt(node_idx, node->KeyAt(0));
  }
 
  buffer_pool_manager_->UnpinPage(parent_id, true);
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
  // 叶根变空
  if (old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    return true;
  }
 
  // 内部根只剩一个子指针
  if (!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1) {
    auto *root = reinterpret_cast<InternalPage *>(old_root_node);
    page_id_t new_root_id = root->RemoveAndReturnOnlyChild();
    root_page_id_ = new_root_id;
    UpdateRootPageId(0);
 
    // 将新根的 parent 设为 INVALID
    auto *new_root_page = buffer_pool_manager_->FetchPage(new_root_id);
    auto *new_root = reinterpret_cast<BPlusTreePage *>(new_root_page->GetData());
    new_root->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    return true;
  }
 
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
  if (IsEmpty()) return End();
  Page *leaf_page = FindLeafPage(nullptr, INVALID_PAGE_ID, true);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, 0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) return End();
  Page *leaf_page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  int idx = leaf->KeyIndex(key, processor_);
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  // 若 idx == size，当前页已没有 >= key 的元素，跳到下一页
  if (idx >= leaf->GetSize()) {
    page_id_t next = leaf->GetNextPageId();
    if (next == INVALID_PAGE_ID) return End();
    return IndexIterator(next, buffer_pool_manager_, 0);
  }
  return IndexIterator(page_id, buffer_pool_manager_, idx);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  return IndexIterator(INVALID_PAGE_ID, nullptr, 0);
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
  if (IsEmpty()) return nullptr;
 
  page_id_t cur_id = (page_id == INVALID_PAGE_ID) ? root_page_id_ : page_id;
  auto *page = buffer_pool_manager_->FetchPage(cur_id);
 
  while (true) {
    auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    if (node->IsLeafPage()) break;
 
    auto *internal = reinterpret_cast<InternalPage *>(node);
    page_id_t child_id;
    if (leftMost) {
      child_id = internal->ValueAt(0);
    } else {
      child_id = internal->Lookup(key, processor_);
    }
    buffer_pool_manager_->UnpinPage(cur_id, false);
    cur_id = child_id;
    page = buffer_pool_manager_->FetchPage(cur_id);
  }
 
  return page;
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
  auto *roots_page = reinterpret_cast<IndexRootsPage *>(
      buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID));
 
  if (insert_record == 1) {
    roots_page->Insert(index_id_, root_page_id_);
  } else {
    if (root_page_id_ == INVALID_PAGE_ID) {
      roots_page->Delete(index_id_);
    } else {
      roots_page->Update(index_id_, root_page_id_);
    }
  }
 
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
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