/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "common/catalog_change.h"
#include "load_manager.h"
#include "record_printer.h"
#include "recovery/checkpoint_master.h"
#include "recovery/log_manager.h"

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

namespace {

constexpr size_t kOutputColumnWidth = 16;
std::mutex fuzzy_checkpoint_mutex;

void append_fixed_width_cell(std::string *row, std::string_view cell) {
    row->append("| ");
    if (cell.size() > kOutputColumnWidth) {
        row->append(cell.data(), kOutputColumnWidth - 3);
        row->append("...");
    } else {
        row->append(kOutputColumnWidth - cell.size(), ' ');
        if (!cell.empty()) {
            row->append(cell.data(), cell.size());
        }
    }
    row->push_back(' ');
}

size_t format_int_cell(int value, char *out) {
    char digits[16];
    size_t digit_count = 0;
    bool negative = value < 0;
    unsigned int magnitude = 0;
    if (negative) {
        magnitude = static_cast<unsigned int>(-(value + 1)) + 1;
    } else {
        magnitude = static_cast<unsigned int>(value);
    }
    do {
        digits[digit_count++] = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    size_t pos = 0;
    if (negative) {
        out[pos++] = '-';
    }
    while (digit_count > 0) {
        out[pos++] = digits[--digit_count];
    }
    return pos;
}

void append_cell_to_rows(const char *rec_buf, const ColMeta &col, std::string *client_row) {
    char number_buf[64];
    std::string_view cell;
    if (col.type == TYPE_INT) {
        size_t len = format_int_cell(rmdb::load_unaligned<int>(rec_buf), number_buf);
        cell = std::string_view(number_buf, len);
    } else if (col.type == TYPE_FLOAT) {
        int len = snprintf(number_buf, sizeof(number_buf), "%.6f", rmdb::load_unaligned<float>(rec_buf));
        cell = len <= 0 ? std::string_view()
                        : std::string_view(number_buf, std::min(static_cast<size_t>(len), sizeof(number_buf) - 1));
    } else if (col.type == TYPE_STRING) {
        const void *nul = std::memchr(rec_buf, '\0', static_cast<size_t>(col.len));
        size_t len = nul == nullptr ? static_cast<size_t>(col.len)
                                    : static_cast<const char *>(nul) - rec_buf;
        cell = std::string_view(rec_buf, len);
    } else {
        cell = std::string_view();
    }
    if (client_row != nullptr) {
        append_fixed_width_cell(client_row, cell);
    }
}

void append_client_row(std::string *row, Context *context) {
    if (context->ellipsis_) {
        return;
    }
    row->append("|\n");
    if (*context->offset_ + RECORD_COUNT_LENGTH + row->length() < BUFFER_LENGTH) {
        memcpy(context->data_send_ + *(context->offset_), row->data(), row->length());
        *(context->offset_) += row->length();
    } else {
        context->ellipsis_ = true;
    }
}

}  // namespace

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                rmdb::publish_schema_change();
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                rmdb::publish_schema_change();
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                rmdb::publish_schema_change();
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                rmdb::publish_schema_change();
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        if (x->tag == T_Load) {
            rmdb::load_csv_into_table(sm_manager_, x->tab_name_, x->file_name_);
            rmdb::publish_bulk_data_change();
            return;
        }
        throw InternalError("Unexpected dml type in run_mutli_query");
    }
}

void QlManager::create_fuzzy_checkpoint(LogManager *log_manager, Transaction *exclude_txn) {
    if (log_manager == nullptr) {
        throw InternalError("checkpoint requires a WAL manager");
    }
    // SQL and background checkpoints share this mutex. Callers hold the statement
    // gate's read side, so DML remains concurrent while catalog DDL stays excluded.
    std::lock_guard<std::mutex> checkpoint_lock(fuzzy_checkpoint_mutex);
    BeginCheckpointLogRecord begin_record;
    const lsn_t begin_lsn = log_manager->add_log_to_buffer(&begin_record);

    auto txn_snapshot = txn_mgr_->CollectCheckpointTxnTable(exclude_txn);
    // Snapshot logical tombstones before the buffer-pool DPT. GC either leaves a
    // DELETE in this snapshot, or finalizes its heap page into the later DPT.
    auto logical_delete_pages = txn_mgr_->CollectCheckpointLogicalDeletePages();
    auto runtime_dpt = sm_manager_->get_bpm()->snapshot_dirty_page_table();
    lsn_t reclaim_horizon = begin_lsn;
    if (txn_snapshot.oldest_first_lsn != INVALID_LSN) {
        reclaim_horizon = std::min(reclaim_horizon, txn_snapshot.oldest_first_lsn);
    }
    std::unordered_map<int, rmdb::u64> table_hash_by_fd;
    table_hash_by_fd.reserve(sm_manager_->fhs_.size());
    for (const auto &entry : sm_manager_->fhs_) {
        table_hash_by_fd.emplace(entry.second->GetFd(), HashTableName(entry.first));
    }
    std::vector<CheckpointDirtyPageInfo> dirty_pages;
    dirty_pages.reserve(logical_delete_pages.size() + runtime_dpt.size());
    for (const auto &page : logical_delete_pages) {
        dirty_pages.push_back(page);
        reclaim_horizon = std::min(reclaim_horizon, page.rec_lsn_);
    }
    for (const auto &[page_id, rec_lsn] : runtime_dpt) {
        auto table = table_hash_by_fd.find(page_id.fd);
        if (table == table_hash_by_fd.end()) {
            // The ARIES DPT is populated only by heap DML finalization. Index
            // pages are derived state and never receive WAL pageLSNs.
            throw InternalError("checkpoint DPT references an unknown heap file");
        }
        dirty_pages.push_back(
            CheckpointDirtyPageInfo{table->second, page_id.page_no, rec_lsn});
        reclaim_horizon = std::min(reclaim_horizon, rec_lsn);
    }
    std::sort(dirty_pages.begin(), dirty_pages.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.table_hash_ != rhs.table_hash_) return lhs.table_hash_ < rhs.table_hash_;
        return lhs.page_no_ < rhs.page_no_;
    });
    size_t compacted_pages = 0;
    for (const auto &page : dirty_pages) {
        if (compacted_pages > 0 &&
            dirty_pages[compacted_pages - 1].table_hash_ == page.table_hash_ &&
            dirty_pages[compacted_pages - 1].page_no_ == page.page_no_) {
            dirty_pages[compacted_pages - 1].rec_lsn_ =
                std::min(dirty_pages[compacted_pages - 1].rec_lsn_, page.rec_lsn_);
            continue;
        }
        dirty_pages[compacted_pages++] = page;
    }
    dirty_pages.resize(compacted_pages);

    EndCheckpointLogRecord end_record(begin_lsn, txn_mgr_->NextTransactionId(),
                                       std::move(txn_snapshot.entries),
                                       std::move(dirty_pages));
    const lsn_t end_lsn = log_manager->add_log_to_buffer(&end_record);
    log_manager->flush_log_to_disk_until(end_lsn + end_record.log_tot_len_ - 1);

    CheckpointMasterRecord previous;
    CheckpointMasterRecord master;
    const bool has_previous = rmdb::checkpoint_master::Read(&previous);
    master.generation = has_previous ? previous.generation + 1 : 1;
    master.end_checkpoint_lsn = end_lsn;
    const lsn_t retained_segment = reclaim_horizon / DiskManager::WAL_SEGMENT_SIZE;
    master.first_retained_lsn = retained_segment > 1
                                    ? retained_segment * DiskManager::WAL_SEGMENT_SIZE
                                    : 0;
    if (has_previous && master.first_retained_lsn < previous.first_retained_lsn) {
        throw InternalError("checkpoint WAL retention boundary moved backwards");
    }
    if (!rmdb::checkpoint_master::Publish(master)) {
        throw InternalError("failed to publish checkpoint master record");
    }
    sm_manager_->get_disk_manager()->reclaim_log_segments_before(master.first_retained_lsn);
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto set_isolation = std::dynamic_pointer_cast<SetTransactionIsolationPlan>(plan)) {
        if (context != nullptr && context->session_isolation_ != nullptr) {
            *(context->session_isolation_) = set_isolation->isolation_level_;
        }
        return;
    }
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                if (context->txn_ != nullptr) {
                    context->txn_->set_txn_mode(true);
                    *txn_id = context->txn_->get_transaction_id();
                }
                break;
            }  
            case T_Transaction_commit:
            {
                if (context->txn_ != nullptr) {
                    txn_mgr_->commit(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                }
                *txn_id = INVALID_TXN_ID;
                break;
            }    
            case T_Transaction_rollback:
            {
                if (context->txn_ != nullptr) {
                    txn_mgr_->abort(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                }
                *txn_id = INVALID_TXN_ID;
                break;
            }    
            case T_Transaction_abort:
            {
                if (context->txn_ != nullptr) {
                    txn_mgr_->abort(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                }
                *txn_id = INVALID_TXN_ID;
                break;
            }
            case T_Checkpoint:
            {
                if (context == nullptr || context->log_mgr_ == nullptr) {
                    throw InternalError("checkpoint requires a WAL manager");
                }
                Transaction *exclude_txn =
                    context->txn_ != nullptr && !context->txn_->get_txn_mode()
                        ? context->txn_
                        : nullptr;
                create_fuzzy_checkpoint(context->log_mgr_, exclude_txn);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }

    } else if(auto x = std::dynamic_pointer_cast<ExplainPlan>(plan)) {
        for (auto &line : x->lines_) {
            std::string out = line + "\n";
            memcpy(context->data_send_ + *(context->offset_), out.c_str(), out.length());
            *(context->offset_) += out.length();
        }
    } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
        switch (x->set_knob_type_)
        {
        case ast::SetKnobType::EnableNestLoop: {
            planner_->set_enable_nestedloop_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableSortMerge: {
            planner_->set_enable_sortmerge_join(x->bool_value_);
            break;
        }
        default: {
            throw RMDBError("Not implemented!\n");
            break;
        }
        }
    }
}

// 执行select语句并将结果返回客户端。
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols, 
                            Context *context) {
    executorTreeRoot->beginTuple();

    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.output_name.empty() ? sel_col.col_name : sel_col.output_name);
    }

    std::vector<ColMeta> output_cols;
    std::vector<size_t> output_col_idxs;
    output_cols.reserve(sel_cols.size());
    output_col_idxs.reserve(sel_cols.size());
    const auto &root_cols = executorTreeRoot->cols();
    for (const auto &sel_col : sel_cols) {
        auto pos = executorTreeRoot->get_col(root_cols, sel_col);
        output_cols.push_back(*pos);
        if (!sel_col.output_name.empty()) {
            output_cols.back().name = sel_col.output_name;
        }
        output_col_idxs.push_back(static_cast<size_t>(pos - root_cols.begin()));
        const auto &col = output_cols.back();
        if (col.offset < 0 || col.len < 0 ||
            static_cast<size_t>(col.offset + col.len) > executorTreeRoot->tupleLen()) {
            throw RMDBError("invalid output column offset");
        }
    }

    if (context->result_writer_ != nullptr) {
        context->result_writer_->BeginResult(output_cols);
        rmdb::u64 row_count = 0;
        std::vector<const char *> cells(output_cols.size());
        for (; !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
            auto tuple = executorTreeRoot->ReadTupleView();
            if (!tuple) {
                break;
            }
            for (size_t i = 0; i < output_cols.size(); ++i) {
                cells[i] = tuple->cell_at(output_cols[i], output_col_idxs[i]);
            }
            context->result_writer_->WriteRow(output_cols, cells);
            ++row_count;
        }
        context->result_writer_->EndResult(row_count);
        return;
    }

    // Print header into buffer
    RecordPrinter rec_printer(sel_cols.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // Print records
    size_t num_rec = 0;
    // 执行query_plan
    for (; !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto tuple = executorTreeRoot->ReadTupleView();
        if (!tuple) {
            break;
        }
        std::string client_row;
        client_row.reserve(output_cols.size() * (kOutputColumnWidth + 3) + 2);
        for (size_t i = 0; i < output_cols.size(); ++i) {
            const auto &col = output_cols[i];
            append_cell_to_rows(tuple->cell_at(col, output_col_idxs[i]), col, &client_row);
        }
        append_client_row(&client_row, context);
        num_rec++;
    }
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
