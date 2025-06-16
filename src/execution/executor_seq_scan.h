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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void beginTuple() override {
        // 1. 创建该表的记录迭代器 scan_
        scan_ = std::make_unique<RmScan>(fh_);
        
        // 2. 使用记录迭代器 scan_ 扫描表中元组，直至遇到第一条满足选择条件的元组，将该元组的Rid记录在 rid_ 中
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            
            // 获取当前记录
            auto record = fh_->get_record(rid_, context_);
            
            // 检查是否满足条件
            if (eval_conds(record.get(), fed_conds_, cols_)) {
                // 找到满足条件的元组，停止扫描
                return;
            }
            
            // 移动到下一条记录
            scan_->next();
        }
        
        // 如果没有找到满足条件的元组，rid_ 保持为扫描结束时的状态
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void nextTuple() override {
        // 确保扫描器已经初始化
        if (!scan_) {
            return;
        }
        
        // 移动到下一条记录
        scan_->next();
        
        // 使用记录迭代器 scan_ 继续扫描表中元组，直至遇到下一条满足选择条件的元组，将该元组的Rid记录在 rid_ 中
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            
            // 获取当前记录
            auto record = fh_->get_record(rid_, context_);
            
            // 检查是否满足条件
            if (eval_conds(record.get(), fed_conds_, cols_)) {
                // 找到满足条件的元组，停止扫描
                return;
            }
            
            // 移动到下一条记录
            scan_->next();
        }
        
        // 如果没有找到满足条件的元组，rid_ 保持为扫描结束时的状态
    }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    std::unique_ptr<RmRecord> Next() override {
        // 返回 rid_ 标识的元组，调用 RmFileHandle::get_record 函数
        if (is_end()) {
            return nullptr;  // 如果已经到达扫描末尾，返回空指针
        }
        
        // 使用 rid_ 从文件中获取记录
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { 
        // 使用记录迭代器 scan_ 判断是否没有输入元组了
        if (!scan_) {
            return true;  // 如果扫描器未初始化，认为已结束
        }
        return scan_->is_end();
    }
    
    std::string getType() override { return "SeqScanExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    private:
    bool eval_cond(const RmRecord *rec, const Condition &cond, const std::vector<ColMeta> &rec_cols) {
        // 1. 获取条件左部表达式的类型和值
        auto lhs_col_it = std::find_if(rec_cols.begin(), rec_cols.end(),
            [&](const ColMeta &col_meta) {
                return col_meta.name == cond.lhs_col.col_name;
            });
        
        if (lhs_col_it == rec_cols.end()) {
            throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
        }
        
        // 获取左部值在记录中的数据
        char *lhs_data = rec->data + lhs_col_it->offset;
        ColType lhs_type = lhs_col_it->type;
        int lhs_len = lhs_col_it->len;
        
        // 2. 获取条件右部表达式的类型和值
        char *rhs_data = nullptr;
        ColType rhs_type;
        int rhs_len;
        
        Value temp_rhs_val;  // 用于存储临时的右部值
        
        if (cond.is_rhs_val) {
            // 右部是常量值
            temp_rhs_val = cond.rhs_val;
            // 只有在 raw 为空时才调用 init_raw
            if (temp_rhs_val.raw == nullptr) {
                temp_rhs_val.init_raw(lhs_len);  // 初始化为与左部相同的长度
            }
            rhs_data = temp_rhs_val.raw->data;
            rhs_type = temp_rhs_val.type;
            rhs_len = lhs_len;
        } else {
            // 右部是另一列
            auto rhs_col_it = std::find_if(rec_cols.begin(), rec_cols.end(),
                [&](const ColMeta &col_meta) {
                    return col_meta.name == cond.rhs_col.col_name;
                });
            
            if (rhs_col_it == rec_cols.end()) {
                throw ColumnNotFoundError(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
            }
            
            rhs_data = rec->data + rhs_col_it->offset;
            rhs_type = rhs_col_it->type;
            rhs_len = rhs_col_it->len;
        }
        
        // 检查类型是否匹配
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
        
        // 3. 使用 ix_compare 函数进行比较
        int cmp_result = ix_compare(lhs_data, rhs_data, lhs_type, lhs_len);
        
        // 4. 根据条件中的比较运算符进行判断
        switch (cond.op) {
            case OP_EQ: return cmp_result == 0;
            case OP_NE: return cmp_result != 0;
            case OP_LT: return cmp_result < 0;
            case OP_GT: return cmp_result > 0;
            case OP_LE: return cmp_result <= 0;
            case OP_GE: return cmp_result >= 0;
            default:
                throw InternalError("Unknown comparison operator");
        }
    }

    bool eval_conds(const RmRecord *rec, const std::vector<Condition> &conds, const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(),
            [&](const Condition &cond) { return eval_cond(rec, cond, rec_cols); }
        );
    }
};