#ifndef __TRANSFORMTASKS__
#define __TRANSFORMTASKS__

#include "global.h"
#include "CustomAlias.h"

class TransformTasks {
	public:
		TransformTasks(Pass* _pass, Module* _m, Function* _wtg) { 
			pass = _pass;
			m = _m; 
			write_to_gbuf = _wtg;
		}
		void runTransformation(func_vals_map WARinFunc);
		void replaceToPriv(Function* F, val_vec WARs);
		void privatize(Instruction* firstInst, Value* v);
		inst_vec getArrReadPoint(Value* orig, Value* priv, Function* F);
		inst_inst_vec getArrWritePoint(Value* orig, Value* priv, Function* F);
		void insertDynamicPriv(Instruction* I, Value* orig, Value* priv);
		void insertDynamicCommit(Instruction* storeBegin, Instruction* storeEnd, Value* orig, Value* priv);
		Instruction* insertPreCommit(Value* oldVal, Value* newVal, Instruction* insertBefore);
	private:
		Pass* pass;
		Module* m;
		Function* write_to_gbuf;
};

#endif
