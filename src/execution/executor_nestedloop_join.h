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
#include "index/ix_index_handle.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        // 1. 使用执行器 left_ 定位到左子节点算子的第一条结果元组
        left_->beginTuple();
        
        // 2. 使用执行器 right_ 定位到右子节点算子的第一条结果元组
        right_->beginTuple();
        
        // 初始化结束标志
        isend = left_->is_end() || right_->is_end();
        
        // 如果没有结束，查找第一个满足条件的记录对
        if (!isend) {
            findNextValidPair();
        }
    }

    void nextTuple() override {
        // 1. 如果执行器 right_ 未处于右子节点算子结果的末尾，则使用执行器 right_ 定位到右子节点算子的下一条结果元组
        if (!right_->is_end()) {
            right_->nextTuple();
            if (!right_->is_end()) {
                // 查找下一个满足条件的记录对
                findNextValidPair();
                return;
            }
        }
        
        // 2. 如果执行器 right_ 处于右子节点算子结果的末尾，则使用执行器 left_ 定位到左子节点算子的下一条结果元组，
        //    使用执行器 right_ 定位到右子节点算子的第一条结果元组
        left_->nextTuple();
        if (!left_->is_end()) {
            right_->beginTuple(); // 重新开始右侧扫描
            // 查找下一个满足条件的记录对
            findNextValidPair();
        } else {
            // 更新结束标志
            isend = true;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        // 如果已经结束，返回 nullptr
        if (isend) {
            return nullptr;
        }
        
        // 获取当前左右记录
        auto left_record = left_->Next();
        auto right_record = right_->Next();
        
        // 检查是否有记录（这里应该总是有，因为 findNextValidPair 已经确保了）
        if (left_record == nullptr || right_record == nullptr) {
            isend = true;
            return nullptr;
        }
        
        // 创建结果元组
        RmRecord result_record(len_);
        
        // 连接左右记录的数据
        // 先复制左侧记录的数据
        memcpy(result_record.data, left_record->data, left_->tupleLen());
        
        // 再复制右侧记录的数据（偏移量为左侧记录的长度）
        memcpy(result_record.data + left_->tupleLen(), right_record->data, right_->tupleLen());
        
        // 返回结果元组指针
        return std::make_unique<RmRecord>(std::move(result_record));
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend; }
    
    std::string getType() override { return "NestedLoopJoinExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    private:
    // 查找下一个满足连接条件的记录对
    void findNextValidPair() {
        while (!left_->is_end() && !right_->is_end()) {
            auto left_record = left_->Next();
            auto right_record = right_->Next();
            
            if (left_record == nullptr || right_record == nullptr) {
                isend = true;
                return;
            }
            
            // 检查连接条件
            if (eval_conds(left_record.get(), right_record.get(), fed_conds_, cols_)) {
                // 找到满足条件的记录对，停止查找
                return;
            }
            
            // 当前记录对不满足条件，移动到下一个记录对
            if (!right_->is_end()) {
                right_->nextTuple();
                if (right_->is_end()) {
                    // 右侧结束，移动左侧
                    left_->nextTuple();
                    if (!left_->is_end()) {
                        right_->beginTuple();
                    }
                }
            } else {
                // 右侧已经结束，移动左侧
                left_->nextTuple();
                if (!left_->is_end()) {
                    right_->beginTuple();
                }
            }
        }
        
        // 如果到达这里，说明没有找到满足条件的记录对
        isend = true;
    }

    bool eval_cond(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const Condition &cond, const std::vector<ColMeta> &rec_cols) {
        // 1. 获取连接条件左部表达式的类型和值
        auto lhs_col_it = get_col(rec_cols, cond.lhs_col);
        char *lhs_data = nullptr;
        ColType lhs_type = lhs_col_it->type;
        int lhs_len = lhs_col_it->len;
        
        // 判断左部列来自哪个记录（左记录还是右记录）
        if (lhs_col_it->offset < left_->tupleLen()) {
            // 来自左记录
            lhs_data = lhs_rec->data + lhs_col_it->offset;
        } else {
            // 来自右记录，需要调整偏移量
            lhs_data = rhs_rec->data + (lhs_col_it->offset - left_->tupleLen());
        }
        
        // 2. 获取连接条件右部表达式的类型和值
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
            auto rhs_col_it = get_col(rec_cols, cond.rhs_col);
            rhs_type = rhs_col_it->type;
            rhs_len = rhs_col_it->len;
            
            // 判断右部列来自哪个记录（左记录还是右记录）
            if (rhs_col_it->offset < left_->tupleLen()) {
                // 来自左记录
                rhs_data = lhs_rec->data + rhs_col_it->offset;
            } else {
                // 来自右记录，需要调整偏移量
                rhs_data = rhs_rec->data + (rhs_col_it->offset - left_->tupleLen());
            }
        }
        
        // 检查类型是否匹配
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
        
        // 3. 使用 ix_compare 函数进行比较
        int cmp_result = ix_compare(lhs_data, rhs_data, lhs_type, lhs_len);
        
        // 4. 根据连接条件中的比较运算符进行判断
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

    bool eval_conds(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const std::vector<Condition> &conds, const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(),
            [&](const Condition &cond) { return eval_cond(lhs_rec, rhs_rec, cond, rec_cols); }
        );
    }
};