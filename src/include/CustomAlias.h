#ifndef __CUSTOMALIAS__
#define __CUSTOMALIAS__

#include "global.h"
#include "BackwardSearcher.h"

class CustomAlias {
	public:
		CustomAlias(AliasAnalysis* _AA, Instruction* _curInst) {
			depth = 0;
			AA = _AA;
			curInst = _curInst;
		}
		inst_vec alias(Value* v, GlobalVariable* gv, Instruction* I);
		val_inst_vec* getPossiblePointee(Value* pointer, Instruction* I);
	private:
		unsigned depth;
		AliasAnalysis* AA;
		Instruction* curInst;
		val_vec checkedVal;
};

#endif
