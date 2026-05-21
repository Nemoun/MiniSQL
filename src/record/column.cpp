#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

/**
* TODO: Student Implement
*/
uint32_t Column::SerializeTo(char *buf) const {
  uint32_t ofs = 0;
 
  // 1. 写入魔数
  MACH_WRITE_UINT32(buf + ofs, COLUMN_MAGIC_NUM);
  ofs += sizeof(uint32_t);
 
  // 2. 写入列名长度 + 列名字符串
  uint32_t name_len = name_.length();
  MACH_WRITE_UINT32(buf + ofs, name_len);
  ofs += sizeof(uint32_t);
  MACH_WRITE_STRING(buf + ofs, name_);
  ofs += name_len;
 
  // 3. 写入 type_
  MACH_WRITE_UINT32(buf + ofs, static_cast<uint32_t>(type_));
  ofs += sizeof(uint32_t);
 
  // 4. 写入 len_
  MACH_WRITE_UINT32(buf + ofs, len_);
  ofs += sizeof(uint32_t);
 
  // 5. 写入 table_ind_
  MACH_WRITE_UINT32(buf + ofs, table_ind_);
  ofs += sizeof(uint32_t);
 
  // 6. 写入 nullable_ 和 unique_
  *(buf + ofs) = static_cast<char>(nullable_);
  ofs += 1;
  *(buf + ofs) = static_cast<char>(unique_);
  ofs += 1;
 
  return ofs;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::GetSerializedSize() const {
  return sizeof(uint32_t) * 4 + name_.length() + 2;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  if (column != nullptr) {
    LOG(WARNING) << "Pointer to column is not null in column deserialize." << std::endl;
  }
 
  uint32_t ofs = 0;
 
  // 1. 读取并校验魔数
  uint32_t magic = MACH_READ_UINT32(buf + ofs);
  ASSERT(magic == COLUMN_MAGIC_NUM, "Invalid magic number when deserializing Column.");
  ofs += sizeof(uint32_t);
 
  // 2. 读取列名
  uint32_t name_len = MACH_READ_UINT32(buf + ofs);
  ofs += sizeof(uint32_t);
  std::string column_name(buf + ofs, name_len);
  ofs += name_len;
 
  // 3. 读取 type_
  TypeId type = static_cast<TypeId>(MACH_READ_UINT32(buf + ofs));
  ofs += sizeof(uint32_t);
 
  // 4. 读取 len_
  uint32_t col_len = MACH_READ_UINT32(buf + ofs);
  ofs += sizeof(uint32_t);
 
  // 5. 读取 table_ind_
  uint32_t col_ind = MACH_READ_UINT32(buf + ofs);
  ofs += sizeof(uint32_t);
 
  // 6. 读取 nullable_ 和 unique_
  bool nullable = static_cast<bool>(*(buf + ofs));
  ofs += 1;
  bool unique = static_cast<bool>(*(buf + ofs));
  ofs += 1;
 
  // 7. 根据类型选择合适的构造函数
  if (type == TypeId::kTypeChar) {
    column = new Column(column_name, type, col_len, col_ind, nullable, unique);
  } else {
    column = new Column(column_name, type, col_ind, nullable, unique);
  }
 
  return ofs;
}
