#ifndef __TRANSFORMTASKS__
#define __TRANSFORMTASKS__

#include "global.h"
#include "CustomAlias.h"

class TransformTasks {
	public:
		TransformTasks(Pass* _pass, Module* _m, Function* _lb) { 
			pass = _pass;
			m = _m; 
			log_backup = _lb;
		}
		void runTransformation(func_vals_map WARinFunc);
		//void replaceToPriv(Function* F, val_vec WARs);
		void backup(Instruction* firstInst, Value* v);
		//inst_vec getArrReadPoint(Value* orig, Value* priv, Function* F);
		inst_inst_vec getArrWritePoint(Value* val, Function* F);
		void insertDynamicBackup(Instruction* I, Value* orig, Value* priv);
		//void insertDynamicCommit(Instruction* storeBegin, Instruction* storeEnd, Value* orig, Value* priv);
		Instruction* insertLogBackup(Value* oldVal, Value* newVal, Instruction* insertBefore);
	private:
		Pass* pass;
		Module* m;
		Function* log_backup;
};

#endif
