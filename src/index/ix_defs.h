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

#include <cstring>
#include <vector>

#include "common/binary.h"
#include "defs.h"
#include "storage/buffer_pool_manager.h"

constexpr int IX_NO_PAGE = -1;
constexpr int IX_PAGE_TOMBSTONE = -2;
constexpr int IX_PAGE_DRAINING = -3;
constexpr int IX_PAGE_FREE = -4;
constexpr int IX_FILE_HDR_PAGE = 0;
constexpr int IX_LEAF_HEADER_PAGE = 1;
constexpr int IX_INIT_ROOT_PAGE = 2;
constexpr int IX_INIT_NUM_PAGES = 3;
constexpr int IX_MAX_COL_LEN = 512;

class IxFileHdr;

using IxKeyCompareFn = int (*)(const char *a, const char *b, const IxFileHdr *file_hdr);
using IxColumnCompareFn = int (*)(const char *a, const char *b, int len);

struct IxComparePart {
    int offset;
    int len;
    IxColumnCompareFn compare;
};

inline int ix_compare_part_int(const char *a, const char *b, int) {
    int ia = rmdb::load_unaligned<int>(a);
    int ib = rmdb::load_unaligned<int>(b);
    return (ia < ib) ? -1 : ((ia > ib) ? 1 : 0);
}

inline int ix_compare_part_float(const char *a, const char *b, int) {
    float fa = rmdb::load_unaligned<float>(a);
    float fb = rmdb::load_unaligned<float>(b);
    return (fa < fb) ? -1 : ((fa > fb) ? 1 : 0);
}

inline int ix_compare_part_string(const char *a, const char *b, int len) {
    return std::memcmp(a, b, len);
}

inline int ix_compare_key_generic(const char *a, const char *b, const IxFileHdr *file_hdr);

template <int N>
int ix_compare_key_ints(const char *a, const char *b, const IxFileHdr *) {
    for (int i = 0; i < N; ++i) {
        int ia = rmdb::load_unaligned<int>(a + i * static_cast<int>(sizeof(int)));
        int ib = rmdb::load_unaligned<int>(b + i * static_cast<int>(sizeof(int)));
        if (ia != ib) {
            return (ia < ib) ? -1 : 1;
        }
    }
    return 0;
}

inline int ix_compare_key_string1(const char *a, const char *b, const IxFileHdr *file_hdr);

class IxFileHdr {
public: 
    page_id_t first_free_page_no_;      // 文件中第一个空闲的磁盘页面的页面号
    int num_pages_;                     // 磁盘文件中页面的数量
    page_id_t root_page_;               // B+树根节点对应的页面号
    int col_num_;                       // 索引包含的字段数量
    std::vector<ColType> col_types_;    // 字段的类型
    std::vector<int> col_lens_;         // 字段的长度
    int col_tot_len_;                   // 索引包含的字段的总长度
    int btree_order_;                   // # children per page 每个结点最多可插入的键值对数量
    int keys_size_;                     // keys_size = (btree_order + 1) * col_tot_len
    int persistent_entry_count_;        // compatibility field kept for old index header layout
    std::vector<IxComparePart> compare_parts_;  // transient, not serialized
    IxKeyCompareFn compare_key_fn_;             // transient, not serialized
    // first_leaf初始化之后没有进行修改，只不过是在测试文件中遍历叶子结点的时候用了
    page_id_t first_leaf_;              // 首叶节点对应的页号，在上层IxManager的open函数进行初始化，初始化为root page_no
    page_id_t last_leaf_;               // 尾叶节点对应的页号
    int tot_len_;                       // 记录结构体的整体长度

    IxFileHdr() {
        tot_len_ = col_num_ = 0;
        compare_key_fn_ = ix_compare_key_generic;
    }

    IxFileHdr(page_id_t first_free_page_no, int num_pages, page_id_t root_page, int col_num,
        int col_tot_len, int btree_order, int keys_size, page_id_t first_leaf, page_id_t last_leaf)
                : first_free_page_no_(first_free_page_no), num_pages_(num_pages), root_page_(root_page), col_num_(col_num),
                col_tot_len_(col_tot_len), btree_order_(btree_order), keys_size_(keys_size), first_leaf_(first_leaf), last_leaf_(last_leaf) {
                    persistent_entry_count_ = 0;
                    tot_len_ = 0;
                    compare_key_fn_ = ix_compare_key_generic;
                } 

    int compare_key(const char *a, const char *b) const {
        return compare_key_fn_(a, b, this);
    }

    void build_compare_plan() {
        compare_parts_.clear();
        compare_parts_.reserve(col_num_);
        bool all_int_4 = col_num_ > 0;
        int offset = 0;
        for (int i = 0; i < col_num_; ++i) {
            IxColumnCompareFn compare = nullptr;
            switch (col_types_[i]) {
                case TYPE_INT:
                    compare = ix_compare_part_int;
                    all_int_4 = all_int_4 && col_lens_[i] == static_cast<int>(sizeof(int));
                    break;
                case TYPE_FLOAT:
                    compare = ix_compare_part_float;
                    all_int_4 = false;
                    break;
                case TYPE_STRING:
                    compare = ix_compare_part_string;
                    all_int_4 = false;
                    break;
                default:
                    throw InternalError("Unexpected data type");
            }
            compare_parts_.push_back(IxComparePart{offset, col_lens_[i], compare});
            offset += col_lens_[i];
        }

        compare_key_fn_ = ix_compare_key_generic;
        if (all_int_4) {
            switch (col_num_) {
                case 1: compare_key_fn_ = ix_compare_key_ints<1>; break;
                case 2: compare_key_fn_ = ix_compare_key_ints<2>; break;
                case 3: compare_key_fn_ = ix_compare_key_ints<3>; break;
                case 4: compare_key_fn_ = ix_compare_key_ints<4>; break;
                case 5: compare_key_fn_ = ix_compare_key_ints<5>; break;
                case 6: compare_key_fn_ = ix_compare_key_ints<6>; break;
                case 7: compare_key_fn_ = ix_compare_key_ints<7>; break;
                case 8: compare_key_fn_ = ix_compare_key_ints<8>; break;
                default: break;
            }
        } else if (col_num_ == 1 && col_types_[0] == TYPE_STRING) {
            compare_key_fn_ = ix_compare_key_string1;
        }
    }

    void update_tot_len() {
        tot_len_ = 0;
        tot_len_ += sizeof(page_id_t) * 4 + sizeof(int) * 7;
        tot_len_ += sizeof(ColType) * col_num_ + sizeof(int) * col_num_;
    }

    void serialize(char* dest) {
        int offset = 0;
        memcpy(dest + offset, &tot_len_, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, &first_free_page_no_, sizeof(page_id_t));
        offset += sizeof(page_id_t);
        memcpy(dest + offset, &num_pages_, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, &root_page_, sizeof(page_id_t));
        offset += sizeof(page_id_t);
        memcpy(dest + offset, &col_num_, sizeof(int));
        offset += sizeof(int);
        for(int i = 0; i < col_num_; ++i) {
            memcpy(dest + offset, &col_types_[i], sizeof(ColType));
            offset += sizeof(ColType);
        }
        for(int i = 0; i < col_num_; ++i) {
            memcpy(dest + offset, &col_lens_[i], sizeof(int));
            offset += sizeof(int);
        }
        memcpy(dest + offset, &col_tot_len_, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, &btree_order_, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, &keys_size_, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, &persistent_entry_count_, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, &first_leaf_, sizeof(page_id_t));
        offset += sizeof(page_id_t);
        memcpy(dest + offset, &last_leaf_, sizeof(page_id_t));
        offset += sizeof(page_id_t);
        assert(offset == tot_len_);
    }

    void deserialize(const char* src) {
        int offset = 0;
        tot_len_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        first_free_page_no_ = rmdb::load_unaligned<page_id_t>(src + offset);
        offset += sizeof(page_id_t);
        num_pages_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        root_page_ = rmdb::load_unaligned<page_id_t>(src + offset);
        offset += sizeof(page_id_t);
        col_num_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        for(int i = 0; i < col_num_; ++i) {
            ColType type = rmdb::load_unaligned<ColType>(src + offset);
            offset += sizeof(ColType);
            col_types_.push_back(type);
        }
        for(int i = 0; i < col_num_; ++i) {
            int len = rmdb::load_unaligned<int>(src + offset);
            offset += sizeof(int);
            col_lens_.push_back(len);
        }
        col_tot_len_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        btree_order_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        keys_size_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        persistent_entry_count_ = rmdb::load_unaligned<int>(src + offset);
        offset += sizeof(int);
        first_leaf_ = rmdb::load_unaligned<page_id_t>(src + offset);
        offset += sizeof(page_id_t);
        last_leaf_ = rmdb::load_unaligned<page_id_t>(src + offset);
        offset += sizeof(page_id_t);
        assert(offset == tot_len_);
        build_compare_plan();
    }
};

inline int ix_compare_key_generic(const char *a, const char *b, const IxFileHdr *file_hdr) {
    for (const auto &part : file_hdr->compare_parts_) {
        int res = part.compare(a + part.offset, b + part.offset, part.len);
        if (res != 0) {
            return res;
        }
    }
    return 0;
}

inline int ix_compare_key_string1(const char *a, const char *b, const IxFileHdr *file_hdr) {
    return std::memcmp(a, b, file_hdr->col_lens_[0]);
}

class IxPageHdr {
public:
    // Active pages store IX_NO_PAGE here. Empty leaves being drained use
    // IX_PAGE_DRAINING/IX_PAGE_TOMBSTONE; reclaimed pages store the next free
    // page number and are tagged by parent == IX_PAGE_FREE.
    page_id_t next_free_page_no;
    page_id_t parent;               // 父亲节点所在页面的叶号
    int num_key;                    // # current keys (always equals to #child - 1) 已插入的keys数量，key_idx∈[0,num_key)
    bool is_leaf;                   // 是否为叶节点
    page_id_t prev_leaf;            // previous leaf node's page_no, effective only when is_leaf is true
    page_id_t next_leaf;            // next leaf node's page_no, effective only when is_leaf is true
};

class Iid {
public:
    int page_no;
    int slot_no;

    friend bool operator==(const Iid &x, const Iid &y) { return x.page_no == y.page_no && x.slot_no == y.slot_no; }

    friend bool operator!=(const Iid &x, const Iid &y) { return !(x == y); }
};
