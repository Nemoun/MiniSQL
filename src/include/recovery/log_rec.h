#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};

// used for testing only
using KeyType = std::string;
using ValType = int32_t;

/**
 * DONE: Student Implement
 */
struct LogRec {
    LogRec() = default;
 
    LogRecType type_{LogRecType::kInvalid};
    lsn_t      lsn_{INVALID_LSN};
    lsn_t      prev_lsn_{INVALID_LSN};
    txn_id_t   txn_id_{INVALID_TXN_ID};
 
    // Insert 字段
    KeyType ins_key_{};
    ValType ins_val_{};
 
    // Delete 字段
    KeyType del_key_{};
    ValType del_val_{};
 
    // Update 字段
    KeyType old_key_{};
    ValType old_val_{};
    KeyType new_key_{};
    ValType new_val_{};
 
    /* used for testing only */
    static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
    static lsn_t next_lsn_;
};

std::unordered_map<txn_id_t, lsn_t> LogRec::prev_lsn_map_ = {};
lsn_t LogRec::next_lsn_ = 0;

typedef std::shared_ptr<LogRec> LogRecPtr;

/**
 * 辅助函数：分配 lsn，设置 prev_lsn，并更新 prev_lsn_map_。
 */
static void FillLsnAndPrev(LogRec &rec, txn_id_t txn_id) {
    rec.txn_id_   = txn_id;
    rec.lsn_      = LogRec::next_lsn_++;
    // 若该事务之前有日志，prev_lsn 指向上一条；否则为 INVALID_LSN
    auto it = LogRec::prev_lsn_map_.find(txn_id);
    rec.prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
    // 更新该事务的最新 lsn
    LogRec::prev_lsn_map_[txn_id] = rec.lsn_;
}


static LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
    auto rec = std::make_shared<LogRec>();
    rec->type_    = LogRecType::kInsert;
    rec->ins_key_ = std::move(ins_key);
    rec->ins_val_ = ins_val;
    FillLsnAndPrev(*rec, txn_id);
    return rec;
}


static LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
    auto rec = std::make_shared<LogRec>();
    rec->type_    = LogRecType::kDelete;
    rec->del_key_ = std::move(del_key);
    rec->del_val_ = del_val;
    FillLsnAndPrev(*rec, txn_id);
    return rec;
}

static LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val) {
    auto rec = std::make_shared<LogRec>();
    rec->type_    = LogRecType::kUpdate;
    rec->old_key_ = std::move(old_key);
    rec->old_val_ = old_val;
    rec->new_key_ = std::move(new_key);
    rec->new_val_ = new_val;
    FillLsnAndPrev(*rec, txn_id);
    return rec;
}

static LogRecPtr CreateBeginLog(txn_id_t txn_id) {
    auto rec = std::make_shared<LogRec>();
    rec->type_ = LogRecType::kBegin;
    FillLsnAndPrev(*rec, txn_id);
    return rec;
}

static LogRecPtr CreateCommitLog(txn_id_t txn_id) {
    auto rec = std::make_shared<LogRec>();
    rec->type_ = LogRecType::kCommit;
    FillLsnAndPrev(*rec, txn_id);
    return rec;
}

static LogRecPtr CreateAbortLog(txn_id_t txn_id) {
    auto rec = std::make_shared<LogRec>();
    rec->type_ = LogRecType::kAbort;
    FillLsnAndPrev(*rec, txn_id);
    return rec;
}

#endif  // MINISQL_LOG_REC_H
