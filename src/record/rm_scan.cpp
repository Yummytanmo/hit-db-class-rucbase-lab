/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    rid_.page_no = RM_FIRST_RECORD_PAGE;  // Start from the first data page
    rid_.slot_no = -1;  // Start before first slot so next() will find first valid record
    next();  // Move to the first valid record
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    while (rid_.page_no < file_handle_->file_hdr_.num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
        
        // Try to find next used slot in current page
        rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap, 
                                      file_handle_->file_hdr_.num_records_per_page,
                                      rid_.slot_no);
        
        if (rid_.slot_no < file_handle_->file_hdr_.num_records_per_page) {
            // Found a valid record in current page
            return;
        }
        
        // No more records in current page, move to next page
        rid_.page_no++;
        rid_.slot_no = -1;  // Reset slot for next page
    }
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}