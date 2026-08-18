#pragma once

#include "transaction_registry.h"

class SerializableMetadataRetention {
   public:
    explicit SerializableMetadataRetention(const TransactionRegistry::AllView &transactions);

    void ReleaseIfSafe(txn_id_t txn_id, Transaction *txn, TransactionRegistry::AllView &transactions) const;

   private:
    bool has_active_serializable_{false};
    timestamp_t oldest_active_start_ts_{INVALID_TS};
};
