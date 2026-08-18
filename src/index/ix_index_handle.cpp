/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include <limits>
#include <numeric>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "common/index_test_barrier.h"
#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0;
    int right = page_hdr->num_key;
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (file_hdr->compare_key(get_key(mid), target) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int left = 0;
    int right = page_hdr->num_key;
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (file_hdr->compare_key(get_key(mid), target) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

bool IxNodeHandle::has_high_key() const {
    if (is_tombstone() || is_free_page()) {
        return false;
    }
    page_id_t link = right_link();
    if (is_leaf_page()) {
        return link != IX_LEAF_HEADER_PAGE && link != IX_NO_PAGE;
    }
    return link != IX_NO_PAGE;
}

bool IxNodeHandle::key_belongs_here(const char *key) const {
    if (is_tombstone() || is_free_page()) {
        return false;
    }
    if (!has_high_key()) {
        return true;
    }
    if (get_size() > 0 &&
        file_hdr->compare_key(get_key(get_size() - 1), high_key()) > 0) {
        return true;
    }
    return file_hdr->compare_key(key, high_key()) < 0;
}

void IxNodeHandle::assert_valid() const {
#ifndef NDEBUG
    assert(file_hdr != nullptr);
    assert(page != nullptr);
    assert(get_physical_key_capacity() == get_max_size() + 1);
    assert(get_size() >= 0 && get_size() <= get_max_size());

    if (is_free_page()) {
        assert(get_size() == 0);
        return;
    }
    if (is_tombstone()) {
        assert(is_leaf_page());
        assert(get_size() == 0);
    }

    page_id_t link = right_link();
    if (link != IX_NO_PAGE && link != IX_LEAF_HEADER_PAGE) {
        assert(link >= IX_INIT_ROOT_PAGE);
        assert(link != get_page_no());
    }
    if (is_leaf_page()) {
        assert(link == IX_LEAF_HEADER_PAGE || link == IX_NO_PAGE || link >= IX_INIT_ROOT_PAGE);
    } else {
        assert(link == IX_NO_PAGE || link >= IX_INIT_ROOT_PAGE);
    }
#endif
}

IxReadGuard::IxReadGuard(const IxIndexHandle *index) : index_(index) {
    if (index_ != nullptr) {
        reader_ticket_ = index_->enter_read();
    }
}

IxReadGuard::~IxReadGuard() {
    reset();
}

void IxReadGuard::reset() {
    if (index_ == nullptr) {
        return;
    }
    index_->leave_read(reader_ticket_);
    index_ = nullptr;
    reader_ticket_ = UINT16_MAX;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid *value) {
    int pos = lower_bound(key);
    if (pos < get_size() && file_hdr->compare_key(get_key(pos), key) == 0) {
        if (value != nullptr) {
            *value = get_rid(pos);
        }
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) const {
    int pos = upper_bound(key) - 1;
    if (pos < 0) {
        pos = 0;
    }
    return value_at(pos);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const void *rid_data, int n) {
    assert(pos >= 0 && pos <= get_size());
    assert(n >= 0);
    assert(get_size() + n <= get_max_size());
    int move_count = get_size() - pos;
    if (move_count > 0) {
        memmove(get_key(pos + n), get_key(pos), move_count * file_hdr->col_tot_len_);
        memmove(get_rid_bytes(pos + n), get_rid_bytes(pos), move_count * sizeof(Rid));
    }
    memcpy(get_key(pos), key, n * file_hdr->col_tot_len_);
    memcpy(get_rid_bytes(pos), rid_data, n * sizeof(Rid));
    set_size(get_size() + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < get_size() && file_hdr->compare_key(get_key(pos), key) == 0) {
        return get_size();
    }
    insert_pair(pos, key, value);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    int move_count = get_size() - pos - 1;
    if (move_count > 0) {
        memmove(get_key(pos), get_key(pos + 1), move_count * file_hdr->col_tot_len_);
        memmove(get_rid_bytes(pos), get_rid_bytes(pos + 1), move_count * sizeof(Rid));
    }
    set_size(get_size() - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < get_size() && file_hdr->compare_key(get_key(pos), key) == 0) {
        erase_pair(pos);
    }
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    
    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    if (now_page_no < file_hdr_->num_pages_) {
        disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
    }
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandleGuard, bool> IxIndexHandle::find_leaf_page_guard(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first,
                                                            std::vector<page_id_t> *ancestors) const {
    IxReadGuard read_guard(this);
    page_id_t root_page = root_page_no();
    if (root_page == IX_NO_PAGE) {
        return std::make_pair(IxNodeHandleGuard{}, false);
    }
    (void)operation;
    IxNodeHandleGuard node = fetch_node_guard(root_page, LatchMode::Shared);
    while (true) {
        while (node->is_tombstone()) {
            page_id_t next_page_no = node->right_link();
            if (next_page_no == IX_NO_PAGE || next_page_no == IX_LEAF_HEADER_PAGE) {
                return std::make_pair(IxNodeHandleGuard{}, false);
            }
            node.reset();
            node = fetch_node_guard(next_page_no, LatchMode::Shared);
        }
        while (!node->key_belongs_here(key)) {
            page_id_t next_page_no = node->right_link();
            if (next_page_no == IX_NO_PAGE || next_page_no == IX_LEAF_HEADER_PAGE) {
                return std::make_pair(std::move(node), false);
            }
            node.reset();
            node = fetch_node_guard(next_page_no, LatchMode::Shared);
        }
        if (node->is_leaf_page()) {
            break;
        }
        page_id_t child_page_no = node->internal_lookup(key);
        if (ancestors != nullptr) {
            ancestors->push_back(node->get_page_no());
        }
        node.reset();
        node = fetch_node_guard(child_page_no, LatchMode::Shared);
    }
    if (find_first) {
        while (node->get_prev_leaf() != IX_LEAF_HEADER_PAGE && node->get_size() > 0 &&
               file_hdr_->compare_key(node->get_key(0), key) == 0) {
            page_id_t current_page_no = node->get_page_no();
            page_id_t prev_leaf = node->get_prev_leaf();
            node.reset();
            IxNodeHandleGuard prev = fetch_node_guard(prev_leaf, LatchMode::Shared);
            while (prev->is_tombstone()) {
                page_id_t prev_next = prev->right_link();
                if (prev_next == IX_NO_PAGE || prev_next == IX_LEAF_HEADER_PAGE || prev_next == current_page_no) {
                    break;
                }
                prev.reset();
                prev = fetch_node_guard(prev_next, LatchMode::Shared);
            }
            if (prev->is_tombstone()) {
                prev.reset();
                node = fetch_node_guard(current_page_no, LatchMode::Shared);
                break;
            }
            if (prev->get_size() == 0 ||
                file_hdr_->compare_key(prev->get_key(prev->get_size() - 1), key) != 0) {
                prev.reset();
                node = fetch_node_guard(current_page_no, LatchMode::Shared);
                break;
            }
            node = std::move(prev);
        }
    }
    return std::make_pair(std::move(node), false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
IxNodeHandleGuard IxIndexHandle::try_hinted_leaf(page_id_t hint_page, const char *key) const {
    // 索引节点页从 2 起(0=FILE_HDR, 1=LEAF_HEADER): 非节点页的字段布局
    // 可能被 is_leaf/is_tombstone 误读,必须拒绝。
    if (hint_page < 2 || hint_page == IX_NO_PAGE) {
        return {};
    }
    IxNodeHandleGuard node = fetch_node_guard(hint_page, LatchMode::Shared);
    while (node->is_tombstone()) {
        page_id_t next_page_no = node->right_link();
        if (next_page_no == IX_NO_PAGE || next_page_no == IX_LEAF_HEADER_PAGE) {
            return {};
        }
        node = fetch_node_guard(next_page_no, LatchMode::Shared);
    }
    if (!node->is_leaf_page() || node->get_size() == 0) {
        return {};
    }
    // key_belongs_here 只保证 key 小于 high_key(右边界); 必须同时检查左边界
    // (key >= 页最小 key),否则 hint 页右侧的页也可能误命中。
    if (file_hdr_->compare_key(key, node->get_key(0)) < 0) {
        return {};
    }
    if (!node->key_belongs_here(key)) {
        return {};
    }
    return node;
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction,
                              page_id_t *leaf_hint) {
    bool from_hint = false;
    IxNodeHandleGuard leaf;
    if (leaf_hint != nullptr) {
        leaf = try_hinted_leaf(*leaf_hint, key);
        from_hint = static_cast<bool>(leaf);
    }
    if (!leaf) {
        auto [found_leaf, _] = find_leaf_page_guard(key, Operation::FIND, transaction);
        leaf = std::move(found_leaf);
    }
    if (!leaf) {
        return false;
    }
    if (leaf_hint != nullptr) {
        *leaf_hint = leaf->get_page_no();
    }
    Rid value;
    bool found = leaf->leaf_lookup(key, &value);
    if (!found && from_hint) {
        // hint 误命中: 清除 hint 并重查,保证与无 hint 路径一致(不漏查)。
        *leaf_hint = IX_NO_PAGE;
        auto [found_leaf, _] = find_leaf_page_guard(key, Operation::FIND, transaction);
        if (found_leaf && found_leaf->leaf_lookup(key, &value)) {
            found = true;
            *leaf_hint = found_leaf->get_page_no();
        }
    }
    if (found) {
        result->push_back(value);
    }
    return found;
}

bool IxIndexHandle::get_unique_value(const char *key, Rid *result, Transaction *transaction,
                                     page_id_t *leaf_hint) {
    if (result == nullptr) {
        return false;
    }
    bool from_hint = false;
    IxNodeHandleGuard leaf;
    if (leaf_hint != nullptr) {
        leaf = try_hinted_leaf(*leaf_hint, key);
        from_hint = static_cast<bool>(leaf);
    }
    if (!leaf) {
        auto [found_leaf, _] = find_leaf_page_guard(key, Operation::FIND, transaction);
        leaf = std::move(found_leaf);
    }
    if (!leaf) {
        return false;
    }
    if (leaf_hint != nullptr) {
        *leaf_hint = leaf->get_page_no();
    }
    Rid value;
    if (!leaf->leaf_lookup(key, &value)) {
        if (from_hint) {
            // hint 误命中(页被并发 split/merge/回收复用后 key 实际不在该页):
            // 清除 hint 并用 find_leaf 重查,保证结果与无 hint 路径一致(不漏查)。
            *leaf_hint = IX_NO_PAGE;
            auto [found_leaf, _] = find_leaf_page_guard(key, Operation::FIND, transaction);
            if (!found_leaf || !found_leaf->leaf_lookup(key, &value)) {
                return false;
            }
            *leaf_hint = found_leaf->get_page_no();
        } else {
            return false;
        }
    }
    *result = value;
    return true;
}

void IxIndexHandle::get_prefix_values(const char *prefix_key, int prefix_len, std::vector<Rid> *result) const {
    IxReadGuard read_guard(this);
    if (prefix_len <= 0) {
        get_all_rids(result);
        return;
    }
    if (prefix_len >= file_hdr_->col_tot_len_) {
        auto [leaf, _] = find_leaf_page_guard(prefix_key, Operation::FIND, nullptr);
        if (!leaf) {
            return;
        }
        Rid value;
        if (leaf->leaf_lookup(prefix_key, &value)) {
            result->push_back(value);
        }
        return;
    }
    // 等值前缀扫描:起始槽位按 key 定位并保持起始叶页 latch;结束靠前缀 key
    // 逐条判定(keys 有序,首个前缀不匹配后其后的 key 均不匹配,不会漏行)。
    // 这里不使用物理叶页末尾作为终止边界,避免槽位移动后越界或提前终止。
    IxScanBounds bounds;
    bounds.lower_key.assign(prefix_key, static_cast<size_t>(file_hdr_->col_tot_len_));
    bounds.lower_mode = IxBoundMode::LowerBound;
    auto scan = create_scan(bounds);
    while (scan != nullptr && !scan->is_end()) {
        if (std::memcmp(scan->key(), prefix_key, static_cast<size_t>(prefix_len)) != 0) {
            break;
        }
        result->push_back(scan->rid());
        scan->next();
    }
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandleGuard IxIndexHandle::split(IxNodeHandleGuard &node) {
    assert(node.latch_mode() == LatchMode::Exclusive);
    IxNodeHandleGuard new_node = create_node_guard(LatchMode::Exclusive);
    new_node->page_hdr->next_free_page_no = IX_NO_PAGE;
    new_node->page_hdr->parent = node->get_parent_page_no();
    new_node->page_hdr->num_key = 0;
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->page_hdr->prev_leaf = IX_NO_PAGE;
    new_node->page_hdr->next_leaf = IX_NO_PAGE;

    page_id_t old_right_link = node->right_link();
    bool inherit_high_key = node->has_high_key();
    std::vector<char> inherited_high_key(file_hdr_->col_tot_len_);
    if (inherit_high_key) {
        memcpy(inherited_high_key.data(), node->high_key(), file_hdr_->col_tot_len_);
    }

    int split_index = node->get_size() / 2;
    int move_count = node->get_size() - split_index;
    new_node->insert_pairs(0, node->get_key(split_index), node->get_rid_bytes(split_index), move_count);
    node->set_size(split_index);
    new_node->set_right_link(old_right_link);
    if (inherit_high_key) {
        new_node->set_high_key(inherited_high_key.data());
    }
    node->set_high_key(new_node->get_key(0));
    node->set_right_link(new_node->get_page_no());
    new_node.mark_dirty();

    if (node->is_leaf_page()) {
        new_node->set_prev_leaf(node->get_page_no());
        if (old_right_link != IX_LEAF_HEADER_PAGE && old_right_link != IX_NO_PAGE) {
            IxNodeHandleGuard next = fetch_node_guard(old_right_link, LatchMode::Exclusive);
            if (!inherit_high_key && next->get_size() > 0) {
                new_node->set_high_key(next->get_key(0));
            }
            next->set_prev_leaf(new_node->get_page_no());
            next.mark_dirty();
        } else {
            update_last_leaf_page_no(new_node->get_page_no());
        }
        node.mark_dirty();
    }
    node->assert_valid();
    new_node->assert_valid();
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandleGuard &old_node, const char *key, IxNodeHandleGuard &new_node,
    Transaction *transaction, std::vector<page_id_t> *ancestors) {
    assert(old_node.latch_mode() == LatchMode::Exclusive);
    assert(new_node.latch_mode() == LatchMode::Exclusive);
    bool split_current_root = old_node->get_page_no() == root_page_no() &&
                              (ancestors == nullptr || ancestors->empty());
    if (split_current_root) {
        IxNodeHandleGuard root = create_node_guard(LatchMode::Exclusive);
        root->page_hdr->next_free_page_no = IX_NO_PAGE;
        root->page_hdr->parent = IX_NO_PAGE;
        root->page_hdr->num_key = 0;
        root->page_hdr->is_leaf = false;
        root->page_hdr->prev_leaf = IX_NO_PAGE;
        root->page_hdr->next_leaf = IX_NO_PAGE;
        Rid old_child{old_node->get_page_no(), -1};
        Rid new_child{new_node->get_page_no(), -1};
        root->insert_pair(0, old_node->get_key(0), old_child);
        root->insert_pair(1, key, new_child);
        old_node->set_parent_page_no(root->get_page_no());
        new_node->set_parent_page_no(root->get_page_no());
        update_root_page_no(root->get_page_no());
        old_node.mark_dirty();
        new_node.mark_dirty();
        root.mark_dirty();
        return;
    }
    page_id_t parent_page_no = IX_NO_PAGE;
    if (ancestors != nullptr && !ancestors->empty()) {
        parent_page_no = ancestors->back();
        ancestors->pop_back();
    }
    if (parent_page_no == IX_NO_PAGE) {
        parent_page_no = old_node->get_parent_page_no();
    }
    IxNodeHandleGuard parent = fetch_parent_for_child(parent_page_no, old_node->get_page_no());
    if (!parent) {
        throw InternalError("IxIndexHandle::insert_into_parent could not find parent downlink");
    }
    int child_idx = parent->find_child_index(old_node->get_page_no());
    assert(child_idx >= 0);
    Rid new_child{new_node->get_page_no(), -1};
    parent->insert_pair(child_idx + 1, key, new_child);
    new_node->set_parent_page_no(parent->get_page_no());
    new_node.mark_dirty();
    parent.mark_dirty();
    if (parent->get_size() >= parent->get_max_size()) {
        IxNodeHandleGuard new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction, ancestors);
    }
}

IxBulkLoadEntries::IxBulkLoadEntries(size_t key_size) : key_size_(key_size) {
    if (key_size_ == 0) {
        throw InternalError("Bulk-load key size must be positive");
    }
}

void IxBulkLoadEntries::reserve(size_t entry_count) {
    if (entry_count > std::numeric_limits<rmdb::u32>::max() ||
        entry_count > std::numeric_limits<size_t>::max() / key_size_) {
        throw InternalError("Bulk-load entry count overflow");
    }
    keys_.reserve(entry_count * key_size_);
    rids_.reserve(entry_count);
}

void IxBulkLoadEntries::append(const char *key, const Rid &rid) {
    if (key == nullptr || rids_.size() >= std::numeric_limits<rmdb::u32>::max() ||
        rids_.size() + 1 > std::numeric_limits<size_t>::max() / key_size_) {
        throw InternalError("Invalid compact bulk-load entry");
    }
    keys_.insert(keys_.end(), key, key + key_size_);
    rids_.push_back(rid);
}

void IxBulkLoadEntries::release() {
    std::vector<char>().swap(keys_);
    std::vector<Rid>().swap(rids_);
    std::vector<rmdb::u32>().swap(order_);
}

const char *IxBulkLoadEntries::insertion_key(size_t storage_index) const {
    return keys_.data() + storage_index * key_size_;
}

const char *IxBulkLoadEntries::prepared_key(size_t sorted_index) const {
    return insertion_key(order_[sorted_index]);
}

Rid IxBulkLoadEntries::prepared_rid(size_t sorted_index) const {
    return rids_[order_[sorted_index]];
}

/**
 * @brief Sort and validate a compact (key, rid) list before mutating the tree.
 */
void IxIndexHandle::prepare_bulk_load(IxBulkLoadEntries &entries,
                                      bool enforce_unique) const {
    entries.order_.resize(entries.size());
    std::iota(entries.order_.begin(), entries.order_.end(), rmdb::u32{0});
    std::sort(entries.order_.begin(), entries.order_.end(), [this, &entries](rmdb::u32 a, rmdb::u32 b) {
        int cmp = file_hdr_->compare_key(entries.insertion_key(a), entries.insertion_key(b));
        if (cmp != 0) {
            return cmp < 0;
        }
        const Rid &a_rid = entries.rids_[a];
        const Rid &b_rid = entries.rids_[b];
        if (a_rid.page_no != b_rid.page_no) {
            return a_rid.page_no < b_rid.page_no;
        }
        return a_rid.slot_no < b_rid.slot_no;
    });

    if (enforce_unique) {
        for (size_t i = 1; i < entries.size(); ++i) {
            if (file_hdr_->compare_key(entries.prepared_key(i - 1), entries.prepared_key(i)) == 0) {
                throw RMDBError("Duplicate key");
            }
        }
    }
}

/**
 * @brief 从 (key, rid) 列表批量构建 B+ 树
 */
void IxIndexHandle::bulk_load(IxBulkLoadEntries &entries, bool enforce_unique) {
    prepare_bulk_load(entries, enforce_unique);
    bulk_load_prepared(entries);
}

/**
 * @brief Build a B+ tree from entries already sorted and uniqueness-checked.
 */
void IxIndexHandle::bulk_load_prepared(
    const IxBulkLoadEntries &entries) {
    if (entries.empty()) {
        return;
    }

    int node_load_limit = std::max(2, file_hdr_->btree_order_ - 1);
    
    // Phase 1: build leaf pages bottom-up
    std::vector<page_id_t> leaves;
    BufferAccessStrategy leaf_strategy(BufferAccessClass::IndexBuild);
    BufferAccessStrategy internal_strategy(BufferAccessClass::Hot);
    
    file_hdr_->first_free_page_no_ = IX_NO_PAGE;
    size_t idx = 0;
    page_id_t prev_leaf = IX_LEAF_HEADER_PAGE;
    while (idx < entries.size()) {
        IxNodeHandleGuard leaf = create_node_guard(LatchMode::None, &leaf_strategy);
        leaf->page_hdr->next_free_page_no = IX_NO_PAGE;
        leaf->page_hdr->parent = IX_NO_PAGE;
        leaf->page_hdr->num_key = 0;
        leaf->page_hdr->is_leaf = true;
        leaf->page_hdr->prev_leaf = prev_leaf;
        leaf->page_hdr->next_leaf = IX_LEAF_HEADER_PAGE;

        int count = 0;
        while (idx < entries.size() && count < node_load_limit) {
            leaf->set_key(count, entries.prepared_key(idx));
            leaf->set_rid(count, entries.prepared_rid(idx));
            idx++;
            count++;
        }
        leaf->set_size(count);
        page_id_t cur_page = leaf->get_page_no();
        if (prev_leaf != IX_LEAF_HEADER_PAGE) {
            IxNodeHandleGuard prev = fetch_node_guard(prev_leaf, LatchMode::None, &leaf_strategy);
            prev->set_next_leaf(cur_page);
            prev->set_high_key(leaf->get_key(0));
            prev.mark_dirty();
        } else {
            file_hdr_->first_leaf_ = cur_page;
        }
        leaf.mark_dirty();
        leaves.push_back(cur_page);
        prev_leaf = cur_page;
    }
    file_hdr_->last_leaf_ = leaves.back();
    
    // Phase 2: build internal levels bottom-up
    std::vector<page_id_t> current_level = std::move(leaves);
    bool current_level_is_leaf = true;
    
    while (current_level.size() > 1) {
        std::vector<page_id_t> next_level;
        size_t i = 0;
        page_id_t prev_internal = IX_NO_PAGE;
        while (i < current_level.size()) {
            IxNodeHandleGuard internal = create_node_guard(LatchMode::None, &internal_strategy);
            internal->page_hdr->next_free_page_no = IX_NO_PAGE;
            internal->page_hdr->parent = IX_NO_PAGE;
            internal->page_hdr->num_key = 0;
            internal->page_hdr->is_leaf = false;
            internal->page_hdr->prev_leaf = IX_NO_PAGE;
            internal->page_hdr->next_leaf = IX_NO_PAGE;
            
            int count = 0;
            page_id_t first_child = current_level[i];
            
            // First child: key is min key of its subtree
            {
                IxNodeHandleGuard child = fetch_node_guard(first_child, LatchMode::None,
                                                           current_level_is_leaf ? &leaf_strategy
                                                                                 : &internal_strategy);
                internal->set_key(count, child->get_key(0));
                internal->set_rid(count, Rid{first_child, -1});
                child->set_parent_page_no(internal->get_page_no());
                child.mark_dirty();
                count++;
                i++;
            }
            
            // Remaining children
            while (i < current_level.size() && count < node_load_limit) {
                page_id_t child_no = current_level[i];
                IxNodeHandleGuard child = fetch_node_guard(child_no, LatchMode::None,
                                                           current_level_is_leaf ? &leaf_strategy
                                                                                 : &internal_strategy);
                internal->set_key(count, child->get_key(0));
                internal->set_rid(count, Rid{child_no, -1});
                child->set_parent_page_no(internal->get_page_no());
                child.mark_dirty();
                count++;
                i++;
            }
            internal->set_size(count);
            page_id_t cur_page = internal->get_page_no();
            if (prev_internal != IX_NO_PAGE) {
                IxNodeHandleGuard prev = fetch_node_guard(prev_internal, LatchMode::None, &internal_strategy);
                prev->set_right_link(cur_page);
                prev->set_high_key(internal->get_key(0));
                prev.mark_dirty();
            }
            internal.mark_dirty();
            next_level.push_back(cur_page);
            prev_internal = cur_page;
        }
        current_level = std::move(next_level);
        current_level_is_leaf = false;
    }
    
    // Phase 3: set root
    update_root_page_no(current_level[0]);
    IxNodeHandleGuard root = fetch_node_guard(current_level[0], LatchMode::None, &internal_strategy);
    root->set_parent_page_no(IX_NO_PAGE);
    root.mark_dirty();
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return IxInsertOutcome 插入状态以及目标叶结点页号
 */
IxInsertOutcome IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::vector<page_id_t> ancestors;
    IxNodeHandleGuard leaf = find_leaf_page_for_write(key, &ancestors);
    if (!leaf) {
        throw InternalError("find_leaf_page_for_write returned null leaf");
    }
    assert(leaf.latch_mode() == LatchMode::Exclusive);
    if (leaf->leaf_lookup(key, nullptr)) {
        return {IxInsertResult::kDuplicate, leaf->get_page_no()};
    }
    leaf->insert(key, value);
    leaf.mark_dirty();
    page_id_t target_page_no = leaf->get_page_no();
    if (leaf->get_size() >= leaf->get_max_size()) {
        IxNodeHandleGuard new_leaf = split(leaf);
        if (file_hdr_->compare_key(key, new_leaf->get_key(0)) >= 0) {
            target_page_no = new_leaf->get_page_no();
        }
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction, &ancestors);
    }
    return {IxInsertResult::kInserted, target_page_no};
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::vector<page_id_t> ancestors;
    IxNodeHandleGuard leaf = find_leaf_page_for_write(key, &ancestors);
    if (!leaf) {
        return false;
    }
    assert(leaf.latch_mode() == LatchMode::Exclusive);
    int old_size = leaf->get_size();
    int new_size = leaf->remove(key);
    bool removed = new_size != old_size;
    if (removed) {
        leaf.mark_dirty();
        // Keep an empty leaf linked while transactions are admitted.  A
        // concurrent split can otherwise change the predecessor between the
        // parent unlink and leaf-chain unlink, leaving a live key range
        // unreachable from forward scans.  Empty leaves remain valid B+Tree
        // routing nodes and can accept later inserts into the same range.
        // Physical reclamation is only safe under a tree-wide quiescent
        // boundary; the online delete path deliberately does not attempt it.
    }
    return removed;
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxReadGuard read_guard(this);
    IxNodeHandleGuard node = fetch_node_guard(iid.page_no, LatchMode::Shared);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    return node->get_rid(iid.slot_no);
}

void IxIndexHandle::collect_all_rids(std::vector<Rid> *result) const {
    IxReadGuard read_guard(this);
    page_id_t root_page;
    page_id_t page_no;
    {
        std::shared_lock<std::shared_mutex> tree_lock(tree_latch_);
        root_page = file_hdr_->root_page_;
        page_no = file_hdr_->first_leaf_;
    }
    if (root_page == IX_NO_PAGE || page_no == IX_LEAF_HEADER_PAGE) {
        return;
    }
    while (page_no != IX_LEAF_HEADER_PAGE && page_no != IX_NO_PAGE) {
        IxNodeHandleGuard node = fetch_node_guard(page_no, LatchMode::Shared);
        if (!node->is_tombstone()) {
            for (int i = 0; i < node->get_size(); ++i) {
                result->push_back(node->get_rid(i));
            }
        }
        page_no = safe_next_leaf(node.get());
    }
}

void IxIndexHandle::get_all_rids(std::vector<Rid> *result) const {
    collect_all_rids(result);
}

page_id_t IxIndexHandle::root_page_no() const {
    page_id_t cached = cached_root_.load(std::memory_order_acquire);
    if (cached != IX_NO_PAGE) {
        return cached;
    }
    std::shared_lock<std::shared_mutex> tree_lock(tree_latch_);
    cached = file_hdr_->root_page_;
    cached_root_.store(cached, std::memory_order_release);
    return cached;
}

void IxIndexHandle::update_root_page_no(page_id_t root) {
    std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
    file_hdr_->root_page_ = root;
    cached_root_.store(root, std::memory_order_release);
}

void IxIndexHandle::update_leaf_bounds(page_id_t first_leaf, page_id_t last_leaf) {
    std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
    file_hdr_->first_leaf_ = first_leaf;
    file_hdr_->last_leaf_ = last_leaf;
}

void IxIndexHandle::update_last_leaf_page_no(page_id_t last_leaf) {
    std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
    file_hdr_->last_leaf_ = last_leaf;
}

IxReadGuard IxIndexHandle::make_read_guard() const {
    return IxReadGuard(this);
}

size_t IxIndexHandle::reader_shard_for_current_thread() {
    static std::atomic<size_t> next_shard{0};
    thread_local const size_t shard = next_shard.fetch_add(1, std::memory_order_relaxed) % kReaderShardCount;
    return shard;
}

rmdb::u16 IxIndexHandle::enter_read() const {
    const size_t shard = reader_shard_for_current_thread();
    while (true) {
        rmdb::u64 epoch = reader_epoch_.load(std::memory_order_acquire);
        rmdb::u16 slot = static_cast<rmdb::u16>(epoch & 1U);
        reader_shards_[shard].active[slot].fetch_add(1, std::memory_order_acq_rel);
        if (reader_epoch_.load(std::memory_order_acquire) == epoch) {
            return static_cast<rmdb::u16>((slot << 8U) | shard);
        }
        leave_read(static_cast<rmdb::u16>((slot << 8U) | shard));
    }
}

void IxIndexHandle::leave_read(rmdb::u16 reader_ticket) const {
    const size_t shard = reader_ticket & 0xffU;
    const size_t slot = reader_ticket >> 8U;
    if (reader_ticket == UINT16_MAX || shard >= kReaderShardCount || slot >= 2) {
        throw InternalError("Index reader count underflow");
    }
    auto &counter = reader_shards_[shard].active[slot];
    rmdb::u32 previous = counter.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        counter.fetch_add(1, std::memory_order_relaxed);
        throw InternalError("Index reader count underflow");
    }
    if (previous == 1 && reclamation_waiters_.load(std::memory_order_acquire) != 0) {
        std::lock_guard<std::mutex> lock(reclamation_latch_);
        reclamation_cv_.notify_all();
    }
}

page_id_t IxIndexHandle::safe_next_leaf(const IxNodeHandle &node) const {
    page_id_t next_leaf = node.get_next_leaf();
    if (next_leaf == node.get_page_no()) {
        throw InternalError("IxIndexHandle detected self-linked leaf");
    }
    return next_leaf;
}

/**
 * @brief 按 lower key 定位扫描起始位置
 *
 * 在 find_leaf_page_guard 下降后,从目标叶页(shared latch 保护)内按 key 定位
 * 起始槽位;槽位越出叶页末尾则沿 next_leaf 前进。返回的叶页 guard 保持
 * shared-latched,由 IxScan 持有——避免"seek 释放 latch 之后、扫描建立之前"
 * 叶页被并发修改导致起始 Iid 失效的窗口。
 *
 * @param key 目标 lower key
 * @param strict false → 第一个 key >= target;true → 第一个 key > target
 * @return {叶页 guard, 起始槽位};索引为空或 key 超出所有 key 时 guard 为空
 */
std::pair<IxNodeHandleGuard, int> IxIndexHandle::seek_lower_internal(const char *key,
                                                                     bool strict) const {
    auto [leaf, _] = find_leaf_page_guard(key, Operation::FIND, nullptr, true);
    if (!leaf) {
        return {IxNodeHandleGuard{}, 0};
    }
    while (true) {
        int pos = strict ? leaf->upper_bound(key) : leaf->lower_bound(key);
        if (pos < leaf->get_size()) {
            // 测试 barrier:seek 定位完成后暂停(仍持有叶页 shared latch)。
            // 写者此时阻塞在 latch 上,验证 seek 与扫描建立是原子的。
            if (rmdb::index_test::pause_after_seek.load(std::memory_order_acquire)) {
                rmdb::index_test::pause_after_seek_hits.fetch_add(1, std::memory_order_release);
                while (rmdb::index_test::pause_after_seek.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            return {std::move(leaf), pos};
        }
        if (leaf->get_next_leaf() == IX_LEAF_HEADER_PAGE || leaf->get_next_leaf() == IX_NO_PAGE) {
            leaf.reset();
            return {IxNodeHandleGuard{}, 0};
        }
        page_id_t next_leaf = safe_next_leaf(leaf.get());
        leaf.reset();
        leaf = fetch_next_live_leaf(next_leaf, LatchMode::Shared);
        if (!leaf) {
            return {IxNodeHandleGuard{}, 0};
        }
    }
}

bool IxIndexHandle::predecessor(const char *upper_key, std::string *key, Rid *rid, bool inclusive) const {
    if (upper_key == nullptr || key == nullptr || rid == nullptr) {
        return false;
    }
    IxReadGuard read_guard(this);
    auto [leaf, _] = find_leaf_page_guard(upper_key, Operation::FIND, nullptr, true);
    if (!leaf) {
        return false;
    }

    int slot = (inclusive ? leaf->upper_bound(upper_key) : leaf->lower_bound(upper_key)) - 1;
    while (slot < 0) {
        page_id_t prev_leaf = leaf->get_prev_leaf();
        if (prev_leaf == IX_LEAF_HEADER_PAGE || prev_leaf == IX_NO_PAGE) {
            return false;
        }
        leaf.reset();
        leaf = fetch_node_guard(prev_leaf, LatchMode::Shared);
        while (leaf->is_tombstone() || leaf->get_size() == 0) {
            prev_leaf = leaf->get_prev_leaf();
            if (prev_leaf == IX_LEAF_HEADER_PAGE || prev_leaf == IX_NO_PAGE) {
                return false;
            }
            leaf.reset();
            leaf = fetch_node_guard(prev_leaf, LatchMode::Shared);
        }
        slot = leaf->get_size() - 1;
    }

    key->assign(leaf->get_key(slot), static_cast<size_t>(file_hdr_->col_tot_len_));
    *rid = leaf->get_rid(slot);
    return true;
}

/**
 * @brief 以逻辑 key 边界创建范围扫描
 *
 * 起始位置由 seek_lower_internal 在持有起始叶页 shared latch 时确定;结束边界
 * 不保存为物理 Iid,而是在扫描推进到每个叶页时按 upper key 重新计算。因此
 * 并发叶页插入/删除/分裂造成的槽位移动不会导致漏行或越界。
 *
 * @param bounds 逻辑边界(lower_mode=None 表示从首叶开始;upper_mode=None 表示扫到末尾)
 * @return IxScan 游标;索引为空时返回立即 exhausted 的空游标
 */
std::unique_ptr<IxScan> IxIndexHandle::create_scan(const IxScanBounds &bounds) const {
    IxReadGuard read_guard(this);
    IxNodeHandleGuard guard;
    Iid start;
    if (bounds.lower_mode != IxBoundMode::None) {
        std::tie(guard, start.slot_no) = seek_lower_internal(
            bounds.lower_key.data(), bounds.lower_mode == IxBoundMode::UpperBound);
        if (guard) {
            start.page_no = guard->get_page_no();
        }
    } else {
        page_id_t root_page;
        page_id_t first_leaf;
        {
            std::shared_lock<std::shared_mutex> tree_lock(tree_latch_);
            root_page = file_hdr_->root_page_;
            first_leaf = file_hdr_->first_leaf_;
        }
        if (root_page == IX_NO_PAGE || first_leaf == IX_LEAF_HEADER_PAGE || first_leaf == IX_NO_PAGE) {
            return std::make_unique<IxScan>(this, bounds, Iid{}, IxNodeHandleGuard{});
        }
        guard = fetch_next_live_leaf(first_leaf, LatchMode::Shared);
        if (!guard) {
            return std::make_unique<IxScan>(this, bounds, Iid{}, IxNodeHandleGuard{});
        }
        start = Iid{guard->get_page_no(), 0};
    }
    return std::make_unique<IxScan>(this, bounds, start, std::move(guard));
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandleGuard IxIndexHandle::fetch_node_guard(int page_no, LatchMode latch_mode,
                                                  BufferAccessStrategy *strategy) const {
    if (page_no == IX_NO_PAGE || page_no == IX_LEAF_HEADER_PAGE) {
        throw InternalError("IxIndexHandle::fetch_node_guard called with sentinel page");
    }
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no}, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::fetch_page failed in IxIndexHandle::fetch_node");
    }
    return IxNodeHandleGuard(buffer_pool_manager_, file_hdr_, page, latch_mode);
}

IxNodeHandleGuard IxIndexHandle::fetch_next_live_leaf(page_id_t page_no, LatchMode latch_mode) const {
    while (page_no != IX_LEAF_HEADER_PAGE && page_no != IX_NO_PAGE) {
        IxNodeHandleGuard node = fetch_node_guard(page_no, latch_mode);
        if (!node->is_tombstone() && node->get_size() > 0) {
            return node;
        }
        page_no = safe_next_leaf(node.get());
        node.reset();
    }
    return IxNodeHandleGuard{};
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandleGuard IxIndexHandle::create_node_guard(LatchMode latch_mode, BufferAccessStrategy *strategy) {
    if (auto free_node = pop_free_node_guard(latch_mode)) {
        return free_node;
    }

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->new_page(&new_page_id, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::new_page failed in IxIndexHandle::create_node");
    }
    {
        std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
        file_hdr_->num_pages_++;
    }
    return IxNodeHandleGuard(buffer_pool_manager_, file_hdr_, page, latch_mode);
}

IxNodeHandleGuard IxIndexHandle::pop_free_node_guard(LatchMode latch_mode) {
    page_id_t free_page_no;
    {
        std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
        free_page_no = file_hdr_->first_free_page_no_;
        if (free_page_no == IX_NO_PAGE) {
            return IxNodeHandleGuard{};
        }
    }

    IxNodeHandleGuard node = fetch_node_guard(free_page_no, LatchMode::Exclusive);
    {
        std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
        if (file_hdr_->first_free_page_no_ != free_page_no) {
            node.reset();
            return pop_free_node_guard(latch_mode);
        }
    }

    if (!node->is_free_page()) {
        throw InternalError("IxIndexHandle free list points to non-free page");
    }
    page_id_t next_free = node->page_hdr->next_free_page_no;
    if (next_free != IX_NO_PAGE && next_free < IX_INIT_ROOT_PAGE) {
        throw InternalError("IxIndexHandle free list has invalid next pointer");
    }
    {
        std::unique_lock<std::shared_mutex> tree_lock(tree_latch_);
        if (file_hdr_->first_free_page_no_ != free_page_no) {
            node.reset();
            return pop_free_node_guard(latch_mode);
        }
        file_hdr_->first_free_page_no_ = next_free;
    }

    node->page_hdr->next_free_page_no = IX_NO_PAGE;
    node->page_hdr->parent = IX_NO_PAGE;
    node->page_hdr->num_key = 0;
    node->page_hdr->is_leaf = true;
    node->page_hdr->prev_leaf = IX_NO_PAGE;
    node->page_hdr->next_leaf = IX_NO_PAGE;
    node.mark_dirty();
    if (latch_mode == LatchMode::None) {
        node.unlock();
    } else if (latch_mode == LatchMode::Shared) {
        node.unlock();
        PageId page_id = node.page_id();
        node.reset();
        node = fetch_node_guard(page_id.page_no, LatchMode::Shared);
    }
    return node;
}

IxNodeHandleGuard IxIndexHandle::fetch_parent_for_child(page_id_t parent_page_no, page_id_t child_page_no) const {
    if (parent_page_no == IX_NO_PAGE || parent_page_no == IX_LEAF_HEADER_PAGE) {
        return IxNodeHandleGuard{};
    }
    IxNodeHandleGuard parent = fetch_node_guard(parent_page_no, LatchMode::Exclusive);
    while (parent && parent->find_child_index(child_page_no) < 0) {
        page_id_t next_parent = parent->right_link();
        if (next_parent == IX_NO_PAGE || next_parent == IX_LEAF_HEADER_PAGE) {
            return IxNodeHandleGuard{};
        }
        parent.reset();
        parent = fetch_node_guard(next_parent, LatchMode::Exclusive);
    }
    return parent;
}

IxNodeHandleGuard IxIndexHandle::find_leaf_page_for_write(const char *key, std::vector<page_id_t> *ancestors) const {
    IxReadGuard read_guard(this);
    if (ancestors != nullptr) {
        ancestors->clear();
    }
    auto [candidate, _] = find_leaf_page_guard(key, Operation::FIND, nullptr, false, ancestors);
    if (!candidate) {
        return IxNodeHandleGuard{};
    }
    assert(candidate.latch_mode() == LatchMode::Shared);
    page_id_t leaf_page_no = candidate->get_page_no();
    candidate.reset();

    IxNodeHandleGuard leaf = fetch_node_guard(leaf_page_no, LatchMode::Exclusive);
    while (leaf && !leaf->key_belongs_here(key)) {
        page_id_t next_page_no = leaf->right_link();
        if (next_page_no == IX_NO_PAGE || next_page_no == IX_LEAF_HEADER_PAGE) {
            if (leaf->is_tombstone()) {
                return IxNodeHandleGuard{};
            }
            break;
        }
        leaf.reset();
        leaf = fetch_node_guard(next_page_no, LatchMode::Exclusive);
    }
    assert(!leaf || leaf->is_leaf_page());
    return leaf;
}
