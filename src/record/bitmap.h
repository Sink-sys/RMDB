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

#include <algorithm>
#include <cstring>

#include "common/types.h"

static constexpr int BITMAP_WIDTH = 8;
static constexpr unsigned BITMAP_HIGHEST_BIT = 0x80u;  // 128 (2^7)

class Bitmap {
   public:
    // 从地址bm开始的size个字节全部置0
    static void init(char *bm, int size) { memset(bm, 0, size); }

    // pos位 置1
    static void set(char *bm, int pos) { bm[get_bucket(pos)] |= get_bit(pos); }

    // pos位 置0
    static void reset(char *bm, int pos) { bm[get_bucket(pos)] &= static_cast<char>(~get_bit(pos)); }

    // 如果pos位是1，则返回true
    static bool is_set(const char *bm, int pos) { return (bm[get_bucket(pos)] & get_bit(pos)) != 0; }

    /**
     * @brief 找下一个为0 or 1的位
     * @param bit false表示要找下一个为0的位，true表示要找下一个为1的位
     * @param bm 要找的起始地址为bm
     * @param max_n 要找的从起始地址开始的偏移为[curr+1,max_n)
     * @param curr 要找的从起始地址开始的偏移为[curr+1,max_n)
     * @return 找到了就返回偏移位置，没找到就返回max_n
     */
    static int next_bit(bool bit, const char *bm, int max_n, int curr) {
        int pos = std::max(0, curr + 1);
        if (pos >= max_n) {
            return max_n;
        }

        const auto *bytes = reinterpret_cast<const unsigned char *>(bm);

        if ((pos % BITMAP_WIDTH) != 0) {
            int byte_idx = get_bucket(pos);
            int end_bit = std::min(BITMAP_WIDTH, max_n - byte_idx * BITMAP_WIDTH);
            int found = next_bit_in_byte(bit, bytes[byte_idx], pos % BITMAP_WIDTH, end_bit);
            if (found < end_bit) {
                return byte_idx * BITMAP_WIDTH + found;
            }
            pos = (byte_idx + 1) * BITMAP_WIDTH;
        }

        while (pos + 64 <= max_n) {
            rmdb::u64 raw = 0;
            std::memcpy(&raw, bytes + get_bucket(pos), sizeof(raw));
            rmdb::u64 candidates = reverse_bits_in_bytes(bit ? raw : ~raw);
            if (candidates != 0) {
                return pos + static_cast<int>(__builtin_ctzll(candidates));
            }
            pos += 64;
        }

        if (pos < max_n) {
            rmdb::u64 raw = 0;
            int remaining_bits = max_n - pos;
            int remaining_bytes = (remaining_bits + BITMAP_WIDTH - 1) / BITMAP_WIDTH;
            std::memcpy(&raw, bytes + get_bucket(pos), remaining_bytes);
            rmdb::u64 candidates = reverse_bits_in_bytes(bit ? raw : ~raw);
            rmdb::u64 valid_mask = remaining_bits == 64 ? ~rmdb::u64{0} : ((rmdb::u64{1} << remaining_bits) - 1);
            candidates &= valid_mask;
            if (candidates != 0) {
                return pos + static_cast<int>(__builtin_ctzll(candidates));
            }
        }

        return max_n;
    }

    // 找第一个为0 or 1的位
    static int first_bit(bool bit, const char *bm, int max_n) { return next_bit(bit, bm, max_n, -1); }

    // Return logical bits for slots [word_base, word_base + 64). word_base must be 64-bit aligned.
    // Bit 0 in the return value corresponds to slot word_base.
    static rmdb::u64 aligned_word_bits(bool bit, const char *bm, int max_n, int word_base) {
        if (word_base < 0 || word_base >= max_n) {
            return 0;
        }
        const auto *bytes = reinterpret_cast<const unsigned char *>(bm);
        rmdb::u64 raw = 0;
        int remaining_bits = std::min(64, max_n - word_base);
        int remaining_bytes = (remaining_bits + BITMAP_WIDTH - 1) / BITMAP_WIDTH;
        std::memcpy(&raw, bytes + get_bucket(word_base), remaining_bytes);
        rmdb::u64 candidates = reverse_bits_in_bytes(bit ? raw : ~raw);
        if (remaining_bits < 64) {
            candidates &= (rmdb::u64{1} << remaining_bits) - 1;
        }
        return candidates;
    }

    // for example:
    // rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page,
    // rid_.slot_no); int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);

   private:
    static int get_bucket(int pos) { return pos / BITMAP_WIDTH; }

    static char get_bit(int pos) { return BITMAP_HIGHEST_BIT >> static_cast<char>(pos % BITMAP_WIDTH); }

    static rmdb::u64 reverse_bits_in_bytes(rmdb::u64 x) {
        x = ((x & 0xF0F0F0F0F0F0F0F0ULL) >> 4) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
        x = ((x & 0xCCCCCCCCCCCCCCCCULL) >> 2) | ((x & 0x3333333333333333ULL) << 2);
        x = ((x & 0xAAAAAAAAAAAAAAAAULL) >> 1) | ((x & 0x5555555555555555ULL) << 1);
        return x;
    }

    static int next_bit_in_byte(bool bit, unsigned char byte, int start_bit, int end_bit) {
        for (int i = start_bit; i < end_bit; ++i) {
            if (((byte & static_cast<unsigned char>(BITMAP_HIGHEST_BIT >> i)) != 0) == bit) {
                return i;
            }
        }
        return end_bit;
    }
};
