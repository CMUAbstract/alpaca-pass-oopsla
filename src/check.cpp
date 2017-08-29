#include "include/check.h"

/** @brief return number of element it value contains (array, struct > 1, else 1)
 *  @param val     value in question
 *  */
uint64_t getSize(Value* val){
	if(val->getType()->getContainedType(0)->getTypeID() == llvm::Type::ArrayTyID){
		return dyn_cast<ArrayType>(val->getType()->getContainedType(0))->getNumElements();
	}
	else if(val->getType()->getContainedType(0)->getTypeID() == llvm::Type::StructTyID){
		return dyn_cast<StructType>(val->getType()->getContainedType(0))->getNumElements();
	}
	else {
		return 1;
	}
}

/** @brief Check if the instruction is memcpy
 *  @param val     value in question
 *  */
bool isMemcpy(Value* val){
	if (!isa<CallInst>(val)){
		return false;
	}
	else{
		auto* op = dyn_cast<CallInst>(val);
		Function* func = op->getCalledFunction();
		if (func != NULL){ //func == NULL means it is from gcc built lib function. we never need to apply pass on such
			std::string funcName = func->getName().str();
			if(funcName.find("llvm.memcpy") != std::string::npos){ //this is memcpy. it can be read or write
				return true;
			}
		}

	}
	return false;

}

/** @brief Get what type of variable is the pointer pointing at the end
 *  @param val     value in question
 *  */
llvm::Type::TypeID getBaseTypeID(Value* val){
	llvm::Type* type = val->getType();
	while (type->getTypeID() == llvm::Type::PointerTyID){
		type = type->getContainedType(0);
	}
	return type->getTypeID();
}

/** @brief Get depth of pointer
 *  @param val     value in question
 *  */
int getPointerLayer(Value* val){
	llvm::Type* type = val->getType();
	int layer = 0;
	while (type->getTypeID() == llvm::Type::PointerTyID){
		type = type->getContainedType(0);
		layer++;
	}
	return layer;
}	

/** @brief Check if the val is _global_
 *  @param val     value in question
 *  @param pointerLayer  depth of pointer in question
 *  */
Value* isGlobal(Value* val, int pointerLayer){ //return varName
	/**
	 * If _global_pointer = 1; it is writing to _global_
	 * If *_global_pointer = 1; it is not.
	 * pointerLayer is to decide such thing
	 */
	std::string varName;
	varName = val->getName().str();
	//if this is global, end of search!
	if (varName.find("_global_") != std::string::npos && getPointerLayer(val) == pointerLayer){
		return val;
	}
	else if (isa<ConstantExpr>(val)){ //constantExpr.
		if (isa<GEPOperator>(val)){
			auto* op = dyn_cast<GEPOperator>(val);
			return isGlobal(op->getPointerOperand(), pointerLayer);
		}
		if (isa<BitCastOperator>(val)){
			auto* op = dyn_cast<BitCastOperator>(val);
			return isGlobal(op->getOperand(0), pointerLayer);
		}	
	}
	else return NULL;
}
