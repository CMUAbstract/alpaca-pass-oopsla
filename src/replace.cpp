#include "include/replace.h"

/** @brief Replace val to _bak
 *  @param val     	value that needs replacing
 *  @param op		current instruction
 *  */
Instruction* replaceOperand(Value* val, Instruction* op){
	// replaceUsesOfWrite() does not work on gep or bitcast (is it bug?)
	// so we take a workaround
	if(isa<GEPOperator>(val)){
		auto* op2 = dyn_cast<GEPOperator>(val);
		Value* v2 = op2->getPointerOperand();
		std::map<std::string, Value*>::iterator it = newValPointer.find(v2->getName().str().c_str());
		if(it != newValPointer.end()){
			std::vector<Value*> arrayRef;
			for (llvm::User::op_iterator it2 = op2->idx_begin(); it2 != op2->idx_end(); it2++){
				arrayRef.push_back(it2->get());
			}
			GetElementPtrInst* gep = llvm::GetElementPtrInst::CreateInBounds(it->second, ArrayRef<Value*>(arrayRef), "", op);
			op->replaceUsesOfWith(val, gep);
			return gep;
		}
	}
	// replaceUsesOfWrite() does not work on gep or bitcast (is it bug?)
	// so we take a workaround
	else if(isa<BitCastOperator>(val)){
		auto* op2 = dyn_cast<BitCastOperator>(val);
		Value* v2 = op2->getOperand(0);
		std::map<std::string, Value*>::iterator it = newValPointer.find(v2->getName().str().c_str());
		if(it != newValPointer.end()){
			BitCastInst* bc = new BitCastInst(it->second, val->getType(), "",op);
			op->replaceUsesOfWith(val ,bc);
			return bc;
		}
	}
	// else just use replaceUsesOfWrite
	else{
		std::map<std::string, Value*>::iterator it = newValPointer.find(val->getName().str().c_str());
		if(it != newValPointer.end()){
			op->replaceUsesOfWith(val, it->second);
		}
	}
	return op;
}

/** @brief declare double buffer (_bak)
 *  @param val     	value that needs double buffer
 *  @param b		current bb
 *  @param F		current function
 *  */
void double_buffer(GlobalVariable* val, BasicBlock* b, Function &F){
	GlobalVariable* global_bak = new GlobalVariable(*(b->getModule()), val->getType()->getContainedType(0), false, val->getLinkage(), 0, val->getName()+"_bak", val);
	global_bak->copyAttributesFrom(val);
	global_bak->setSection(".nv_vars");
	global_bak->setInitializer(val->getInitializer());	
	// in case of arrays, also init isDirty
	if(val->getType()->getContainedType(0)->getTypeID() == Type::ArrayTyID){
		GlobalVariable* global_isDirty = new GlobalVariable(*(b->getModule()), ArrayType::get(Type::getInt16Ty(b->getContext()), val->getType()->getContainedType(0)->getArrayNumElements()), false, val->getLinkage(), 0, val->getName()+"_isDirty", val);
		global_isDirty->copyAttributesFrom(val);
		global_isDirty->setSection(".nv_vars");
		ConstantAggregateZero* zeroInit = ConstantAggregateZero::get(global_isDirty->getType()->getContainedType(0));
		global_isDirty->setInitializer(zeroInit);
		isDirtys.push_back(global_isDirty);
	}
}

/** @brief For measuring overhead only. Start timer. Note: timer reg should be saved in pointer timer. 
 *  @param endOp     	timer ends after this instruction
 *  @param startOp	timer starts before this instruction
 *  @param b		current bb
 *  */
void start_timer(Instruction* endOp, Instruction* startOp, BasicBlock* b){
	LoadInst* ld = new LoadInst(b->getModule()->getNamedValue("timer"), "", startOp);
	LoadInst* ld2 = new LoadInst(ld, "", true, startOp);
	Constant* two = ConstantInt::get(ld2->getType(), 32);
	BinaryOperator* orrr = BinaryOperator::Create(Instruction::Or, ld2, two, "", startOp);
	StoreInst* str = new StoreInst(orrr, ld, true, startOp);


	Constant* tt = ConstantInt::get(ld2->getType(), -33, true);
	BinaryOperator* andd = BinaryOperator::Create(Instruction::And, orrr, tt, "", endOp);
	StoreInst* str2 = new StoreInst(andd, ld, true, endOp);

}

/** @brief Insert privatization code. 
 *  @param op     	timer ends after this instruction
 *  @param b		current bb
 *  */
void privatize(Instruction* op, BasicBlock* b){
	//privatize global vars to _bak
	for (std::map<std::string, Value*>::iterator it = valPointer.begin(); it != valPointer.end(); it++){
		Value* vp = it->second;
		// don't privatize array beforehand. searchRW.cpp will do the work
		if(vp->getType()->getContainedType(0)->getTypeID() != Type::ArrayTyID){
			Value* vVal = new LoadInst(vp, "", false, op);
			Value* vbak = b->getModule()->getNamedValue(it->first+"_bak");
			StoreInst* st = new StoreInst(vVal, vbak, op);
		}
	}
}

/** @brief declare double buffer (_bak) (by calling double_buffer())
 *  @param b		current bb
 *  @param F		current function
 *  */
void declare_dbuf(BasicBlock* b, Function &F){
	for (std::map<std::string, Value*>::iterator it = valPointer.begin(); it != valPointer.end(); it++){
		Value* vbak = b->getModule()->getNamedValue(it->first+"_bak");
		// if not declared before
		if (vbak == NULL){
			double_buffer(b->getModule()->getGlobalVariable(it->first), b, F);
			vbak = b->getModule()->getNamedValue(it->first+"_bak");	
		}
		newValPointer[it->first] = vbak;
	}
}

/** @brief Insert precommit code
 *  @param op		current instruction
 *  @param F		current function
 *  */
void pre_commit(Instruction* op, Function &F){
	// for every WAR
	for (std::map<std::string, Value*>::iterator it = valPointer.begin(); it != valPointer.end(); it++){
		// only for non-arrays
		if(it->second->getType()->getContainedType(0)->getTypeID() != Type::ArrayTyID){
			std::map<std::string, Value*>::iterator it2 = newValPointer.find(it->first);
			if(it != newValPointer.end()){
				write_to_gbuf(it->second, it2->second, op, F);
			}
			else{
				errs() << "should never reach here!!\n"; 
			}
		}
	}
}

/** @brief Add write_to_gbuf function call
 *  @param oldVal	Original _global_ address
 *  @param newVal	_bak address
 *  @param op		current instruction
 *  @param F		current function
 *  */
Instruction* write_to_gbuf(Value* oldVal, Value* newVal, Instruction* op, Function &F){
	BitCastInst* oldbc = new BitCastInst(oldVal, llvm::Type::getInt8PtrTy(F.getContext()), "",op);
	BitCastInst* newbc = new BitCastInst(newVal, llvm::Type::getInt8PtrTy(F.getContext()), "",op);	

	// This part is a hacky way of calculating sizeof()
	std::vector<Value*> arrayRef;
	arrayRef.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(F.getContext()),1, false));
	Value* size = llvm::GetElementPtrInst::CreateInBounds(llvm::Constant::getNullValue(oldVal->getType()), ArrayRef<Value*>(arrayRef), "", op);
	Value* sizei = llvm::CastInst::Create(llvm::CastInst::getCastOpcode(size, false, llvm::Type::getInt16Ty(F.getContext()),false), size, llvm::Type::getInt16Ty(F.getContext()), "", op);

	std::vector<Value*> args;
	args.push_back(newbc);
	args.push_back(oldbc);
	args.push_back(sizei);
	CallInst* call = llvm::CallInst::Create(wtg, ArrayRef<Value*>(args), "", op);
	return oldbc;
}

/** @brief Add memset to 0 function call
 *  @param array	Array that needs clearing
 *  @param InsertBefore	Instruction that you want the call to be placed before
 *  @param F		current function
 *  */
Instruction* reset_array(Value* array, Instruction* InsertBefore, Function &F){
	BitCastInst* arraybc = new BitCastInst(array, llvm::Type::getInt8PtrTy(F.getContext()), "",InsertBefore);	

	// This part is a hacky way of calculating sizeof()
	std::vector<Value*> arrayRef;
	arrayRef.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(F.getContext()),1, false));
	Value* size = llvm::GetElementPtrInst::CreateInBounds(llvm::Constant::getNullValue(array->getType()), ArrayRef<Value*>(arrayRef), "", InsertBefore);
	Value* sizei = llvm::CastInst::Create(llvm::CastInst::getCastOpcode(size, false, llvm::Type::getInt16Ty(F.getContext()),false), size, llvm::Type::getInt16Ty(F.getContext()), "", InsertBefore);

	Value* zero = llvm::ConstantInt::get(llvm::Type::getInt16Ty(F.getContext()), 0);
	std::vector<Value*> args;
	args.push_back(arraybc);
	args.push_back(zero);
	args.push_back(sizei);
	CallInst* call = llvm::CallInst::Create(array_memset, ArrayRef<Value*>(args), "", InsertBefore);
	return arraybc;
}
