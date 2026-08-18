/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "system/sm_meta.h"
#include "ix_defs.h"
#include "ix_index_handle.h"

class IxManager {
   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;

   public:
    IxManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager) {}

    std::string get_index_name(const std::string &filename, const std::vector<std::string>& index_cols) {
        std::string index_name = filename;
        for(size_t i = 0; i < index_cols.size(); ++i) 
            index_name += "_" + index_cols[i];
        index_name += ".idx";

        return index_name;
    }

    std::string get_index_name(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        std::string index_name = filename;
        for(size_t i = 0; i < index_cols.size(); ++i) 
            index_name += "_" + index_cols[i].name;
        index_name += ".idx";

        return index_name;
    }

    std::string get_index_name(const std::string &filename, const IndexMeta &index) {
        if (!index.index_name.empty()) {
            return index.index_name;
        }
        return get_index_name(filename, index.cols);
    }

    bool exists(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        auto ix_name = get_index_name(filename, index_cols);
        return disk_manager_->is_file(ix_name);
    }

    bool exists(const std::string &filename, const std::vector<std::string>& index_cols) {
        auto ix_name = get_index_name(filename, index_cols);
        return disk_manager_->is_file(ix_name);
    }

    bool exists(const std::string &filename, const IndexMeta &index) {
        auto ix_name = get_index_name(filename, index);
        return disk_manager_->is_file(ix_name);
    }

    void create_index(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        IndexMeta index;
        index.tab_name = filename;
        index.index_name = get_index_name(filename, index_cols);
        index.col_num = static_cast<int>(index_cols.size());
        index.cols = index_cols;
        index.col_tot_len = index.logical_col_tot_len();
        index.unique = true;
        index.hidden = false;
        create_index(filename, index);
    }

    void create_index(const std::string &filename, const IndexMeta &index) {
        std::string ix_name = get_index_name(filename, index);
        // Create index file
        disk_manager_->create_file(ix_name);
        // Open index file
        int fd = disk_manager_->open_file(ix_name);

        // Create file header and write to file
        // Theoretically we have: |page_hdr| + (|attr| + |rid|) * n <= PAGE_SIZE
        // but we reserve one slot for convenient inserting and deleting, i.e.
        // |page_hdr| + (|attr| + |rid|) * (n + 1) <= PAGE_SIZE
        std::vector<ColType> physical_col_types;
        std::vector<int> physical_col_lens;
        physical_col_types.reserve(index.cols.size() + (index.unique ? 0 : 2));
        physical_col_lens.reserve(index.cols.size() + (index.unique ? 0 : 2));
        int col_tot_len = 0;
        for(auto& col: index.cols) {
            physical_col_types.push_back(col.type);
            physical_col_lens.push_back(col.len);
            col_tot_len += col.len;
        }
        if (!index.unique) {
            physical_col_types.push_back(TYPE_INT);
            physical_col_lens.push_back(sizeof(int));
            physical_col_types.push_back(TYPE_INT);
            physical_col_lens.push_back(sizeof(int));
            col_tot_len += sizeof(Rid);
        }
        int max_col_len = index.unique ? IX_MAX_COL_LEN : IX_MAX_COL_LEN + static_cast<int>(sizeof(Rid));
        if (col_tot_len > max_col_len || col_tot_len != index.col_tot_len) {
            throw InvalidColLengthError(col_tot_len);
        }
        int col_num = static_cast<int>(physical_col_lens.size());
        // 根据 |page_hdr| + (|attr| + |rid|) * (n + 1) <= PAGE_SIZE 求得n的最大值btree_order
        // 即 n <= btree_order，那么btree_order就是每个结点最多可插入的键值对数量（实际还多留了一个空位，但其不可插入）
        int btree_order = static_cast<int>((PAGE_SIZE - sizeof(IxPageHdr)) / (col_tot_len + sizeof(Rid)) - 1);
        assert(btree_order > 2);

        // Create file header and write to file
        IxFileHdr* fhdr = new IxFileHdr(IX_NO_PAGE, IX_INIT_NUM_PAGES, IX_INIT_ROOT_PAGE,
                                col_num, col_tot_len, btree_order, (btree_order + 1) * col_tot_len,
                                IX_INIT_ROOT_PAGE, IX_INIT_ROOT_PAGE);
        for(int i = 0; i < col_num; ++i) {
            fhdr->col_types_.push_back(physical_col_types[i]);
            fhdr->col_lens_.push_back(physical_col_lens[i]);
        }
        fhdr->update_tot_len();
        
        char* data = new char[fhdr->tot_len_];
        fhdr->serialize(data);

        disk_manager_->write_page(fd, IX_FILE_HDR_PAGE, data, fhdr->tot_len_);

        alignas(IxPageHdr) char page_buf[PAGE_SIZE];  // 在内存中初始化page_buf中的内容，然后将其写入磁盘
        memset(page_buf, 0, PAGE_SIZE);
        // 注意leaf header页号为1，也标记为叶子结点，其前一个/后一个叶子均指向root node
        // Create leaf list header page and write to file
        {
            memset(page_buf, 0, PAGE_SIZE);
            auto phdr = reinterpret_cast<IxPageHdr *>(page_buf);
            *phdr = {
                .next_free_page_no = IX_NO_PAGE,
                .parent = IX_NO_PAGE,
                .num_key = 0,
                .is_leaf = true,
                .prev_leaf = IX_INIT_ROOT_PAGE,
                .next_leaf = IX_INIT_ROOT_PAGE,
            };
            disk_manager_->write_page(fd, IX_LEAF_HEADER_PAGE, page_buf, PAGE_SIZE);
        }
        // 注意root node页号为2，也标记为叶子结点，其前一个/后一个叶子均指向leaf header
        // Create root node and write to file
        {
            memset(page_buf, 0, PAGE_SIZE);
            auto phdr = reinterpret_cast<IxPageHdr *>(page_buf);
            *phdr = {
                .next_free_page_no = IX_NO_PAGE,
                .parent = IX_NO_PAGE,
                .num_key = 0,
                .is_leaf = true,
                .prev_leaf = IX_LEAF_HEADER_PAGE,
                .next_leaf = IX_LEAF_HEADER_PAGE,
            };
            // Must write PAGE_SIZE here in case of future fetch_node()
            disk_manager_->write_page(fd, IX_INIT_ROOT_PAGE, page_buf, PAGE_SIZE);
        }

        disk_manager_->set_fd2pageno(fd, IX_INIT_NUM_PAGES - 1);  // DEBUG

        // Close index file
        disk_manager_->close_file(fd);
    }

    void destroy_index(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        disk_manager_->destroy_file(ix_name);
    }

    void destroy_index(const std::string &filename, const std::vector<std::string>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        disk_manager_->destroy_file(ix_name);
    }

    void destroy_index(const std::string &filename, const IndexMeta &index) {
        std::string ix_name = get_index_name(filename, index);
        disk_manager_->destroy_file(ix_name);
    }

    // 注意这里打开文件，创建并返回了index file handle的指针
    std::unique_ptr<IxIndexHandle> open_index(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        int fd = disk_manager_->open_file(ix_name);
        buffer_pool_manager_->set_fd_page_wal_policy(fd, PageWalPolicy::DerivedIndex);
        return std::make_unique<IxIndexHandle>(disk_manager_, buffer_pool_manager_, fd);
    }

    std::unique_ptr<IxIndexHandle> open_index(const std::string &filename, const std::vector<std::string>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        int fd = disk_manager_->open_file(ix_name);
        buffer_pool_manager_->set_fd_page_wal_policy(fd, PageWalPolicy::DerivedIndex);
        return std::make_unique<IxIndexHandle>(disk_manager_, buffer_pool_manager_, fd);
    }

    std::unique_ptr<IxIndexHandle> open_index(const std::string &filename, const IndexMeta &index) {
        std::string ix_name = get_index_name(filename, index);
        int fd = disk_manager_->open_file(ix_name);
        buffer_pool_manager_->set_fd_page_wal_policy(fd, PageWalPolicy::DerivedIndex);
        return std::make_unique<IxIndexHandle>(disk_manager_, buffer_pool_manager_, fd);
    }

    void close_index(IxIndexHandle *ih) {
        ih->flush();
        disk_manager_->sync_file(ih->fd_);
        buffer_pool_manager_->set_fd_page_wal_policy(ih->fd_, PageWalPolicy::WalProtected);
        disk_manager_->close_file(ih->fd_);
    }
};
