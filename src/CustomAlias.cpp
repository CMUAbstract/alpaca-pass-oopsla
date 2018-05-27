#include "include/CustomAlias.h"

/*
 *	Return the aliasing points instruction vector if it alias.
 *	Return empty vector otherwise.
 */
inst_vec CustomAlias::alias(Value* v, GlobalVariable* gv, Instruction* I) {
	inst_vec aliasPoints;
//	bool isAlias = false;
	checkedVal.push_back(v);
	if (AA->alias(v, gv) == AliasResult::MustAlias) {
		aliasPoints.push_back(I);
		return aliasPoints;
	}
	else {
		// In case of pointer, we need some backward search
		// 1) LoadInst
		if (LoadInst* ld = dyn_cast<LoadInst>(v)) {
			// is this pointing to gv?
			Value* pointer = ld->getOperand(0);

			// search for any pointee that this might point to
			val_inst_vec* pointee = getPossiblePointee(pointer, ld);
			for(val_inst_vec::iterator VI = pointee->begin(); VI != pointee->end(); ++VI) {
				if(std::find(checkedVal.begin(), checkedVal.end(), VI->first) == checkedVal.end()) {
					checkedVal.push_back(VI->first);
					inst_vec a = alias((VI)->first, gv, (VI)->second);
					aliasPoints.insert(aliasPoints.end(), a.begin(), a.end());
				}
			}
		}
		else if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(v)) {
			Value* pointee = gep->getOperand(0);
			inst_vec a = alias(pointee, gv, gep);
			aliasPoints.insert(aliasPoints.end(), a.begin(), a.end());
		}
		else if (BitCastInst* bc = dyn_cast<BitCastInst>(v)) {
			Value* pointee = bc->getOperand(0);
			inst_vec a = alias(pointee, gv, bc);
			aliasPoints.insert(aliasPoints.end(), a.begin(), a.end());
		}
		else if (GEPOperator* gep = dyn_cast<GEPOperator>(v)) {
			Value* pointee = gep->getOperand(0);
			inst_vec a = alias(pointee, gv, I);
			aliasPoints.insert(aliasPoints.end(), a.begin(), a.end());
		}
	}
	return aliasPoints;
}

val_inst_vec* CustomAlias::getPossiblePointee(Value* pointer, Instruction* I) {
	// reverse iterate and find stores that store to pointer
	if (LoadInst* ld = dyn_cast<LoadInst>(pointer)) {
		val_inst_vec* pointees = getPossiblePointee(ld->getOperand(0), ld);
		for (val_inst_vec::iterator VI = pointees->begin(); VI != pointees->end(); ++VI) {
			return getPossiblePointee((VI)->first, (VI)->second);
		}
	}
	else if (AllocaInst* al = dyn_cast<AllocaInst>(pointer)) {
		BackwardSearcher* BS = new BackwardSearcher();
		BS->calculatePossiblePointee(pointer, I);
		return BS->getPossiblePointees();
	}

	return NULL;
}
