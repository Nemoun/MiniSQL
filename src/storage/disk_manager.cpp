#include "storage/disk_manager.h"

#include <sys/stat.h>

#include <filesystem>
#include <stdexcept>

#include "glog/logging.h"
#include "page/bitmap_page.h"

DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
  // directory or file does not exist
  if (!db_io_.is_open()) {
    db_io_.clear();
    // create a new file
    std::filesystem::path p = db_file;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out);
    db_io_.close();
    // reopen with original mode
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_io_.is_open()) {
      throw std::exception();
    }
  }
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
}

void DiskManager::Close() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  WritePhysicalPage(META_PAGE_ID, meta_data_);
  if (!closed) {
    db_io_.close();
    closed = true;
  }
}

void DiskManager::ReadPage(page_id_t logical_page_id, char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  ReadPhysicalPage(MapPageId(logical_page_id), page_data);
}

void DiskManager::WritePage(page_id_t logical_page_id, const char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  WritePhysicalPage(MapPageId(logical_page_id), page_data);
}

/**
 * TODO: Student Implement
 */
page_id_t DiskManager::AllocatePage() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  // 获取元数据页
  DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  // 如果所有extent都满了，则返回INVALID_PAGE_ID
  if (meta_page->GetAllocatedPages() == MAX_VALID_PAGE_ID) {
    return INVALID_PAGE_ID;
  }

  // 找到第一个未满的extent
  uint32_t extent_id = 0;
  bool found = false;
  for (uint32_t i = 0; i < meta_page->GetExtentNums(); i++) {
    // 如果extent已满，则跳过
    if (meta_page->GetExtentUsedPage(i) == BITMAP_SIZE) {
      continue;
    }
    extent_id = i;
    found = true;
    break;
  }

  // 如果所有extent都满了，则新建一个extent
  if (!found) {
    extent_id = meta_page->GetExtentNums();
    meta_page->extent_used_page_[extent_id] = 0;
    meta_page->num_extents_++;
  }

  // 读取位图页，分配新页
  page_id_t bitmap_page_id = 1 + (BITMAP_SIZE + 1) * extent_id;
  char *bitmap_page = new char[PAGE_SIZE];
  ReadPhysicalPage(bitmap_page_id, bitmap_page);
  BitmapPage<PAGE_SIZE> *bitmap_page_obj = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_page);
  uint32_t page_offset;
  if (!bitmap_page_obj->AllocatePage(page_offset)) {
    return INVALID_PAGE_ID;
  }
  // 写回位图页
  WritePhysicalPage(bitmap_page_id, bitmap_page);

  // 更新元数据页
  meta_page->extent_used_page_[extent_id]++;
  meta_page->num_allocated_pages_++;
  // 将 meta 页写回磁盘，确保持久化（防止其他 DiskManager 实例读取到过期数据）
  WritePhysicalPage(META_PAGE_ID, meta_data_);
  return BITMAP_SIZE * extent_id + page_offset;
}

/**
 * TODO: Student Implement
 */
void DiskManager::DeAllocatePage(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
 
  uint32_t extent_id      = logical_page_id / BITMAP_SIZE;
  uint32_t page_in_extent = logical_page_id % BITMAP_SIZE;
 
  if (extent_id >= meta->GetExtentNums()) {
    return;  // 非法页号
  }
 
  // 读取对应 extent 的 BitmapPage
  page_id_t bitmap_physical_id = 1 + extent_id * (BITMAP_SIZE + 1);
  char buf[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_id, buf);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(buf);
 
  if (bitmap->DeAllocatePage(page_in_extent)) {
    // 写回 BitmapPage
    WritePhysicalPage(bitmap_physical_id, buf);
    // 更新 MetaPage 统计
    meta->num_allocated_pages_--;
    meta->extent_used_page_[extent_id]--;
    // 将 meta 页写回磁盘，确保持久化
    WritePhysicalPage(META_PAGE_ID, meta_data_);
  }
}

/**
 * TODO: Student Implement
 */
bool DiskManager::IsPageFree(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
 
  uint32_t extent_id      = logical_page_id / BITMAP_SIZE;
  uint32_t page_in_extent = logical_page_id % BITMAP_SIZE;
 
  if (extent_id >= meta->GetExtentNums()) {
    return true;  // 尚未被 extent 覆盖的页视为空闲
  }
 
  page_id_t bitmap_physical_id = 1 + extent_id * (BITMAP_SIZE + 1);
  char buf[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_id, buf);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(buf);
 
  return bitmap->IsPageFree(page_in_extent);
}

/**
 * TODO: Student Implement
 */
page_id_t DiskManager::MapPageId(page_id_t logical_page_id) {
  uint32_t extent_id      = logical_page_id / BITMAP_SIZE;
  uint32_t page_in_extent = logical_page_id % BITMAP_SIZE;
  // +1 跳过 MetaPage，再 +1 跳过本 extent 的 BitmapPage
  return 1 + extent_id * (BITMAP_SIZE + 1) + 1 + page_in_extent;
}

int DiskManager::GetFileSize(const std::string &file_name) {
  struct stat stat_buf;
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

void DiskManager::ReadPhysicalPage(page_id_t physical_page_id, char *page_data) {
  int offset = physical_page_id * PAGE_SIZE;
  // check if read beyond file length
  if (offset >= GetFileSize(file_name_)) {
#ifdef ENABLE_BPM_DEBUG
    LOG(INFO) << "Read less than a page" << std::endl;
#endif
    memset(page_data, 0, PAGE_SIZE);
  } else {
    // set read cursor to offset
    db_io_.seekp(offset);
    db_io_.read(page_data, PAGE_SIZE);
    // if file ends before reading PAGE_SIZE
    int read_count = db_io_.gcount();
    if (read_count < PAGE_SIZE) {
#ifdef ENABLE_BPM_DEBUG
      LOG(INFO) << "Read less than a page" << std::endl;
#endif
      memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
  }
}

void DiskManager::WritePhysicalPage(page_id_t physical_page_id, const char *page_data) {
  size_t offset = static_cast<size_t>(physical_page_id) * PAGE_SIZE;
  // set write cursor to offset
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);
  // check for I/O error
  if (db_io_.bad()) {
    LOG(ERROR) << "I/O error while writing";
    return;
  }
  // needs to flush to keep disk file in sync
  db_io_.flush();
}