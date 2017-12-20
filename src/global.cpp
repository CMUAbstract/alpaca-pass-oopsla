#include "include/global.h"

uint64_t getSize(Value* val){
	if(val->getType()->getContainedType(0)->getTypeID() 
			== Type::ArrayTyID){
		return cast<ArrayType>(val->getType()->getContainedType(0))->getNumElements();
	}
	else if(val->getType()->getContainedType(0)->getTypeID() 
			== Type::StructTyID){
		return dyn_cast<StructType>(val->getType()->getContainedType(0))->getNumElements();
	}
	else {
		return 1;
	}
}

bool isTask(Function* F) {
	return F->getName().str().find("task_") != std::string::npos;
}

bool isArray(Value* v) {
	return (v->getType()->getContainedType(0)->getTypeID() == Type::ArrayTyID);
}

bool isMemcpy(Instruction* I){
	if (CallInst* ci = dyn_cast<CallInst>(I)){
		Function* F = ci->getCalledFunction();
		if (F != NULL){ //func == NULL means it is from gcc built lib function. we never need to apply pass on such
			std::string funcName = F->getName().str();
			if(funcName.find("llvm.memcpy") != std::string::npos){ //this is memcpy. it can be read or write
				return true;
			}
		}
	}
	return false;
}

bool isTransitionTo(Function* F) {
	if (F != NULL){ //func == NULL means it is from gcc built lib function. we never need to apply pass on such
		std::string funcName = F->getName().str();
		if(funcName.find("transition_to") != std::string::npos){ //this is transition_to. it can be read or write
			return true;
		}
	}
	return false;
}


bool isTransitionTo(Instruction* I) {
	if (CallInst* ci = dyn_cast<CallInst>(I)) {
		Function* F = ci->getCalledFunction();
		return isTransitionTo(F);
	}
	return false;
}
