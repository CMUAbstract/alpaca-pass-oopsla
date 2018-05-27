#ifndef __ALPACAPASS__
#define __ALPACAPASS__

#include "AnalyzeTasks.h"
#include "TransformTasks.h"
#include "global.h"

using namespace llvm;

class AlpacaModulePass : public ModulePass {
	public:
		static char ID;
		AlpacaModulePass() : ModulePass(ID) {}

		virtual bool runOnModule(Module &M);
		void set_ulog();
		void set_my_memset();
		void set_clear_isDirty_function();
		void set_commit_buffer(uint64_t commitSize);
		void declare_globals();
		void declare_priv_buffers(func_vals_map WARinFunc);

		virtual void getAnalysisUsage(AnalysisUsage& AU) const {
			AU.setPreservesAll();
			AU.addRequired<AAResultsWrapperPass>();
		}
		Module* getModule() {
			return m;
		}
		Module* setModule(Module* _m) {
			return m = _m;
		}
	private:
		Module* m;
		Function* log_backup;
		Function* array_memset;
};

#endif
