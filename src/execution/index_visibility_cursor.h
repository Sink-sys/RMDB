#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "defs.h"

class RmFileHandle;
struct RmRecord;
class Transaction;
class TransactionManager;
struct IndexMeta;

class VisibleIndexCursor {
   public:
    enum class ReadState { Invisible, IndexOnly, Materialized };

    struct ReadResult {
        ReadState state{ReadState::Invisible};
        bool heap_fetched{false};

        explicit operator bool() const { return state != ReadState::Invisible; }
    };

    VisibleIndexCursor();
    VisibleIndexCursor(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name,
                       const std::vector<std::string> *index_col_names, const IndexMeta *index_meta,
                       RmFileHandle *file_handle);
    ~VisibleIndexCursor();

    VisibleIndexCursor(VisibleIndexCursor &&) noexcept;
    VisibleIndexCursor &operator=(VisibleIndexCursor &&) noexcept;
    VisibleIndexCursor(const VisibleIndexCursor &) = delete;
    VisibleIndexCursor &operator=(const VisibleIndexCursor &) = delete;

    void bind(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name,
              const std::vector<std::string> *index_col_names, const IndexMeta *index_meta,
              RmFileHandle *file_handle);
    void reset();
    void finish();

    const std::vector<Rid> &history_rids();
    // 记录一次从 current index 已返回/已计数的 RID。history 在首次
    // history_rids() 时加载并对这些 RID 去重,保证"先扫 current、后补 history"
    // 的顺序下不重复处理(修复 current-index 与 snapshot-history 的读取竞态)。
    void record_current_rid(const Rid &rid);
    bool is_historical(const Rid &rid);
    void deduplicate_and_append_history(std::vector<Rid> *candidates);
    ReadResult read_current(const Rid &rid, const char *index_key, RmRecord *out_record,
                            bool materialize_index_only, bool require_matching_key);
    ReadResult read_visible_entry(const Rid &rid, RmRecord *out_record);
    bool current_page_clean_visible() const;
    bool current_page_still_clean_visible(page_id_t page_no);

   private:
    class Impl;
    static std::vector<std::unique_ptr<Impl>> &impl_pool();
    static std::unique_ptr<Impl> acquire_impl();
    static void release_impl(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
