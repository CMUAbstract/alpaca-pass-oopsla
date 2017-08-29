#ifndef __ALPACAPASS__
#define __ALPACAPASS__

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/ilist.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/SymbolTableListTraits.h"
#include <map>

using namespace llvm;
extern std::map<Instruction*, std::vector<std::string>> readPerInst;
extern std::map<Instruction*, std::vector<std::string>> writePerInst;
extern Instruction* firstInst;
extern AllocaInst* al;
extern std::map<std::string,bool> isR;
extern std::map<std::string,Value*> valPointer;
extern std::map<std::string,Value*> newValPointer;
extern std::vector<BasicBlock*> visitedBb;
extern Function* wtg;	
extern Function* mg;	
extern Function* array_memset;	
extern BasicBlock::iterator curOp;
extern BasicBlock::iterator firstOp;
extern BasicBlock* curBb;
extern BasicBlock::iterator curOpSaved;
extern BasicBlock::iterator firstOpSaved;
extern BasicBlock* curBbSaved;
struct non_task {
	std::vector<Value*> used_globalVars;
	std::vector<non_task*> callees;
};
extern std::map<std::string, non_task*> non_tasks;
//extern DataLayout* dl;
extern uint64_t dirtyBufSize;
extern uint64_t maxDirtyBufSize;
extern bool isReplace;
extern std::vector<GlobalVariable*> isDirtys;

#endif
