#ifndef __ANALYZE_TASKS__
#define __ANALYZE_TASKS__

#include "global.h"
#include "CustomAlias.h"
using namespace llvm;

struct non_task {
	val_vec usedGlobal;
	std::vector<non_task*> callee;
};

class AnalyzeTasks {
	public:
		AnalyzeTasks(Pass* _pass) { 
			pass = _pass; 
			maxCommitSize = 0;
		}

		void runTaskAnalysis(Module &M);
		void runNonTaskAnalysis(Module &M);
		void findWriteToGlobal(Value* v, Instruction* I, val_insts_map* Writelist);
		void findReadPrecedingWrite(Value* v, Instruction* I, val_insts_map* Writelist, val_vec* WARlist);
		void addWARinNonTask(non_task* curTask, val_vec* WARlist);
		std::map<Function*, val_vec> getWARinFunc() {
			return WARinFunc;
		}
		uint64_t calcCommitSize(val_vec WARlist);
		uint64_t getMaxCommitSize() {
			return maxCommitSize;
		}
	private:
		uint64_t maxCommitSize;
		Pass* pass;
		std::map<Function*, val_vec> WARinFunc;
		std::map<Function*, non_task*> WARinNonTask;
		std::vector<non_task*> visitedFunc;
};

#endif
