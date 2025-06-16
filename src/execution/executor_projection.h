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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_;                  

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override {
        // 使用执行器 prev_ 定位到子节点算子的第一条结果元组
        prev_->beginTuple();
    }

    void nextTuple() override {
        // 使用执行器 prev_ 定位到子节点算子的下一条结果元组
        prev_->nextTuple();
    }

    std::unique_ptr<RmRecord> Next() override {
        // 1. 创建结果元组（RmRecord 类）
        RmRecord result_record(len_);
        
        // 2. 将子节点算子的当前元组进行投影，填充结果元组
        auto child_record = prev_->Next();
        if (child_record == nullptr) {
            return nullptr;
        }
        
        // 获取子节点的列信息
        auto &prev_cols = prev_->cols();
        
        // 根据 sel_idxs_ 复制需要投影的字段
        for (size_t i = 0; i < sel_idxs_.size(); i++) {
            size_t child_col_idx = sel_idxs_[i];
            const auto &child_col = prev_cols[child_col_idx];
            const auto &result_col = cols_[i];
            
            // 从子记录中复制数据到结果记录
            memcpy(result_record.data + result_col.offset,
                   child_record->data + child_col.offset,
                   child_col.len);
        }
        
        // 3. 返回结果元组指针
        return std::make_unique<RmRecord>(std::move(result_record));
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { 
        // 使用执行器 prev_ 判断是否没有输入元组了
        return prev_->is_end(); 
    }
    
    std::string getType() override { return "ProjectionExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};