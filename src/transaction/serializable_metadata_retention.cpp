#include "serializable_metadata_retention.h"

SerializableMetadataRetention::SerializableMetadataRetention(const TransactionRegistry::AllView &transactions) {
    transactions.ForEach([&](txn_id_t, Transaction *txn) {
        if (txn == nullptr || txn->get_state() != TransactionState::GROWING ||
            txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return;
        }
        has_active_serializable_ = true;
        if (oldest_active_start_ts_ == INVALID_TS || txn->get_start_ts() < oldest_active_start_ts_) {
            oldest_active_start_ts_ = txn->get_start_ts();
        }
    });
}

void SerializableMetadataRetention::ReleaseIfSafe(txn_id_t txn_id, Transaction *txn,
                                                  TransactionRegistry::AllView &transactions) const {
    // 非 SERIALIZABLE 事务从不参与 SSI 依赖图,直接跳过:防御任何路径漏设
    // ssi_metadata_released,避免对每个 SI 事务做 O(注册表) 遍历。
    if (txn == nullptr || txn->ssi_metadata_released() ||
        txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    bool aborted = txn->get_state() == TransactionState::ABORTED;
    bool snapshot_overlap_done = !has_active_serializable_ ||
        (txn->get_commit_ts() != INVALID_TS && txn->get_commit_ts() <= oldest_active_start_ts_);
    if (!aborted && !snapshot_overlap_done) {
        return;
    }
    transactions.ForEach([&](txn_id_t, Transaction *other) {
        if (other != nullptr && other != txn) {
            other->remove_rw_dependency(txn_id);
        }
    });
    txn->clear_serializable_state();
    txn->set_ssi_metadata_released(true);
}
