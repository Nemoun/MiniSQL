#ifndef MINISQL_RECOVERY_MANAGER_H
#define MINISQL_RECOVERY_MANAGER_H

#include <map>
#include <unordered_map>
#include <vector>

#include "recovery/log_rec.h"

using KvDatabase = std::unordered_map<KeyType, ValType>;
using ATT = std::unordered_map<txn_id_t, lsn_t>;

struct CheckPoint {
    lsn_t checkpoint_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase persist_data_{};

    inline void AddActiveTxn(txn_id_t txn_id, lsn_t last_lsn) { active_txns_[txn_id] = last_lsn; }

    inline void AddData(KeyType key, ValType val) { persist_data_.emplace(std::move(key), val); }
};

class RecoveryManager {
public:
    /**
    * DONE: 从 checkpoint 恢复数据库状态和活跃事务表。
    */
    void Init(CheckPoint &last_checkpoint) {
        persist_lsn_  = last_checkpoint.checkpoint_lsn_;
        data_         = last_checkpoint.persist_data_;
        active_txns_  = last_checkpoint.active_txns_;
    }

    /**
    * DONE: 从 persist_lsn_ 之后，按 lsn 顺序重放所有日志
    */
    void RedoPhase() {
        for (auto &[lsn, rec] : log_recs_) {
            if (lsn <= persist_lsn_) continue;
 
            switch (rec->type_) {
                case LogRecType::kInsert:
                    data_[rec->ins_key_] = rec->ins_val_;
                    active_txns_[rec->txn_id_] = lsn;
                    break;
                case LogRecType::kDelete:
                    // Redo Delete：写入被删的旧值（Undo 阶段才决定是否恢复）
                    data_[rec->del_key_] = rec->del_val_;
                    active_txns_[rec->txn_id_] = lsn;
                    break;
                case LogRecType::kUpdate:
                    data_[rec->new_key_] = rec->new_val_;
                    active_txns_[rec->txn_id_] = lsn;
                    break;
                case LogRecType::kBegin:
                    if (active_txns_.find(rec->txn_id_) == active_txns_.end()) {
                        active_txns_[rec->txn_id_] = lsn;
                    }
                    break;
                case LogRecType::kCommit:
                    active_txns_.erase(rec->txn_id_);
                    break;
                case LogRecType::kAbort:
                    // Abort：立即回滚该事务（包含 checkpoint 之前的日志）
                    UndoTxn(rec->prev_lsn_);
                    active_txns_.erase(rec->txn_id_);
                    break;
                default:
                    break;
            }
        }
    }
    /**
    * DONE: 对 active_txns_ 中仍活跃的事务（未 Commit/Abort）逐一回滚
    */
    void UndoPhase() {
        for (auto &[txn_id, last_lsn] : active_txns_) {
            UndoTxn(last_lsn);
        }
        active_txns_.clear();
    }

    // used for test only
    void AppendLogRec(LogRecPtr log_rec) { log_recs_.emplace(log_rec->lsn_, log_rec); }

    // used for test only
    inline KvDatabase &GetDatabase() { return data_; }

private:
    /**
     * 辅助函数：沿 prev_lsn 链回滚一个事务的所有日志。
     * @param last_lsn 该事务最后一条需要 Undo 的日志的 lsn
     */
    void UndoTxn(lsn_t last_lsn) {
        lsn_t cur = last_lsn;
        while (cur != INVALID_LSN) {
            auto it = log_recs_.find(cur);
            if (it == log_recs_.end()) break;
            auto &rec = it->second;
            switch (rec->type_) {
                case LogRecType::kInsert:
                    data_.erase(rec->ins_key_);
                    break;
                case LogRecType::kDelete:
                    data_[rec->del_key_] = rec->del_val_;
                    break;
                case LogRecType::kUpdate:
                    data_[rec->old_key_] = rec->old_val_;
                    break;
                default:
                    break;
            }
            cur = rec->prev_lsn_;
        }
    }
    std::map<lsn_t, LogRecPtr> log_recs_{};
    lsn_t persist_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase data_{};  // all data in database
};

#endif  // MINISQL_RECOVERY_MANAGER_H
