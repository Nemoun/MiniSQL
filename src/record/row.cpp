#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
 
  // 若 Row 为空，直接返回 0
  if (fields_.empty()) return 0;
 
  uint32_t ofs = 0;
  uint32_t field_cnt = static_cast<uint32_t>(fields_.size());
 
  // 1. 写入 Field 数量
  MACH_WRITE_UINT32(buf + ofs, field_cnt);
  ofs += sizeof(uint32_t);
 
  // 2. 写入 null bitmap（ceil(field_cnt / 8) 字节）
  uint32_t bitmap_bytes = (field_cnt + 7) / 8;
  memset(buf + ofs, 0, bitmap_bytes);
  for (uint32_t i = 0; i < field_cnt; i++) {
    if (fields_[i]->IsNull()) {
      buf[ofs + i / 8] |= static_cast<char>(1 << (i % 8));
    }
  }
  ofs += bitmap_bytes;
 
  // 3. 逐个序列化非 null 的 Field
  for (uint32_t i = 0; i < field_cnt; i++) {
    if (!fields_[i]->IsNull()) {
      ofs += fields_[i]->SerializeTo(buf + ofs);
    }
  }
 
  return ofs;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");
 
  uint32_t ofs = 0;
 
  // 1. 读取 Field 数量
  uint32_t field_cnt = MACH_READ_UINT32(buf + ofs);
  ofs += sizeof(uint32_t);
 
  // 2. 读取 null bitmap
  uint32_t bitmap_bytes = (field_cnt + 7) / 8;
  const char *bitmap_ptr = buf + ofs;
  ofs += bitmap_bytes;
 
  // 3. 逐个反序列化 Field
  for (uint32_t i = 0; i < field_cnt; i++) {
    bool is_null = (bitmap_ptr[i / 8] >> (i % 8)) & 1;
    TypeId type = schema->GetColumn(i)->GetType();
    Field *field = nullptr;
    ofs += Field::DeserializeFrom(buf + ofs, type, &field, is_null);
    fields_.push_back(field);
  }
 
  return ofs;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
 
  // 空 Row 返回 0
  if (fields_.empty()) return 0;
 
  uint32_t field_cnt = static_cast<uint32_t>(fields_.size());
 
  // header = field_cnt(4) + null_bitmap(ceil(N/8))
  uint32_t size = sizeof(uint32_t) + (field_cnt + 7) / 8;
 
  // 只统计非 null Field 的序列化大小
  for (uint32_t i = 0; i < field_cnt; i++) {
    if (!fields_[i]->IsNull()) {
      size += fields_[i]->GetSerializedSize();
    }
  }
 
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
