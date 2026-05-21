#include "record/schema.h"

/**
 * TODO: Student Implement
 */
uint32_t Schema::SerializeTo(char *buf) const {
  uint32_t ofs = 0;
 
  // 1. 写入魔数
  MACH_WRITE_UINT32(buf + ofs, SCHEMA_MAGIC_NUM);
  ofs += sizeof(uint32_t);
 
  // 2. 写入 Column 数量
  uint32_t col_cnt = static_cast<uint32_t>(columns_.size());
  MACH_WRITE_UINT32(buf + ofs, col_cnt);
  ofs += sizeof(uint32_t);
 
  // 3. 逐个序列化每个 Column
  for (const auto &col : columns_) {
    ofs += col->SerializeTo(buf + ofs);
  }
 
  return ofs;
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size = sizeof(uint32_t) * 2;
  for (const auto &col : columns_) {
    size += col->GetSerializedSize();
  }
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  uint32_t ofs = 0;
 
  // 1. 读取并校验魔数
  uint32_t magic = MACH_READ_UINT32(buf + ofs);
  ASSERT(magic == SCHEMA_MAGIC_NUM, "Invalid magic number when deserializing Schema.");
  ofs += sizeof(uint32_t);
 
  // 2. 读取 Column 数量
  uint32_t col_cnt = MACH_READ_UINT32(buf + ofs);
  ofs += sizeof(uint32_t);
 
  // 3. 逐个反序列化每个 Column
  std::vector<Column *> columns;
  for (uint32_t i = 0; i < col_cnt; i++) {
    Column *col = nullptr;
    ofs += Column::DeserializeFrom(buf + ofs, col);
    columns.push_back(col);
  }
 
  // 4. 构造 Schema 对象
  schema = new Schema(columns, true);
 
  return ofs;
}