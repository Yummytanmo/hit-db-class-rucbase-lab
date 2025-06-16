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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // 遍历所有需要更新的记录
        for (auto& rid : rids_) {
            // 先读取原记录用于索引删除
            auto old_record = fh_->get_record(rid, context_);
            
            // 创建新记录，首先复制原记录
            RmRecord new_record(fh_->get_file_hdr().record_size);
            memcpy(new_record.data, old_record->data, fh_->get_file_hdr().record_size);
            
            // 根据 set_clauses_ 更新新记录的字段
            for (auto& set_clause : set_clauses_) {
                // 找到对应的列元数据
                auto col_meta = tab_.get_col(set_clause.lhs.col_name);
                
                // 检查类型兼容性
                if (col_meta->type != set_clause.rhs.type) {
                    throw IncompatibleTypeError(coltype2str(col_meta->type), coltype2str(set_clause.rhs.type));
                }
                
                // 创建一个新的 Value 对象来避免 raw 指针冲突
                Value new_val = set_clause.rhs;
                new_val.raw = nullptr;  // 重置 raw 指针
                new_val.init_raw(col_meta->len);
                
                // 将新值复制到记录中对应位置
                memcpy(new_record.data + col_meta->offset, new_val.raw->data, col_meta->len);
            }
            
            // 更新索引：先删除旧索引条目，再插入新索引条目
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto& index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                
                // 构造旧记录的索引键
                char* old_key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(old_key + offset, old_record->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                
                // 构造新记录的索引键
                char* new_key = new char[index.col_tot_len];
                offset = 0;
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(new_key + offset, new_record.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                
                // 删除旧索引条目
                ih->delete_entry(old_key, context_->txn_);
                
                // 插入新索引条目
                ih->insert_entry(new_key, rid, context_->txn_);
                
                delete[] old_key;
                delete[] new_key;
            }
            
            // 最后更新记录文件中的记录
            fh_->update_record(rid, new_record.data, context_);
        }
        
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};