#ifndef __REPLACE__
#define __REPLACE__

#include "AlpacaPass.h"
#include "check.h"
extern Instruction* replaceOperand(Value* val, Instruction* op);
extern void declare_dbuf(BasicBlock* b, Function &F);
extern void privatize(Instruction* op, BasicBlock* b);
extern void pre_commit(Instruction* op, Function &F);
extern void double_buffer(GlobalVariable* val, BasicBlock* b, Function &F);
extern void start_timer(Instruction* endOp, Instruction* startOp, BasicBlock* b);
extern Instruction* write_to_gbuf(Value* oldVal, Value* newVal, Instruction* op, Function &F);
extern Instruction* modify_gbuf(Value* oldVal, Value* numDirtyGV, Instruction* op, Function &F);
extern Instruction* reset_array(Value* array, Instruction* InsertBefore, Function &F);
#endif
