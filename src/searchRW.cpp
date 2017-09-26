#include "include/searchRW.h"

/** @brief Code insert point for arrays. (before both privatization and pre-commit, isDirty check code here)*/
Instruction* insertPoint;
/** @brief Code insert point for arrays. (On write, actual pre-commit code here)*/
Instruction* insertPointForWrite;
/** @brief Code replace point. */
Instruction* replacePoint;
/** @brief Current function. */
Function* curFunc;
/** @brief true if we split block to insert array privatization. */
bool isBlockSplitted;

/** @brief Mark the val as WAR
 *  @param val     The value in question
 *  */
void markAsRW(Value* val){
	std::map<std::string, Value*>::iterator it = valPointer.find(val->getName().str().c_str());
	// check if it is already marked as WAR before
	if(it == valPointer.end()){
		valPointer[val->getName().str().c_str()] = val;	
		dirtyBufSize +=	getSize(val);
	}
}

/** @brief Dig down the function call and mark everything being used as WAR
 *  @param nt     The function in question
 *  */
void search_funcCall(non_task* nt){
	std::vector<Value*>::iterator it2;
	// mark everything used inside the function as WAR
	for(it2 = nt->used_globalVars.begin(); it2 != nt->used_globalVars.end(); ++it2){
		markAsRW(*it2);
	}
	std::vector<non_task*>::iterator it3;
	// dig down iteratively if it calls another function
	for(it3 = nt->callees.begin(); it3 != nt->callees.end(); ++it3){
		if(nt == *it3){
			//if it is recursive call, ignore
		}
		else{
			search_funcCall(*it3);
		}
	}
}

/** @brief Check if current reading val is _global_
 *  @param val     The value in question
 *  */
void isGlobalRead(Value* val){
	/****************************/
	// All the read from NV has to be from load
	// 1. if the load is from global_var, it is read for sure
	// 2. if the load is from instruction, it might be gep or bitcast. follow it
	// 3. if the load is from user-defined var, it is just data copy from v to v.
	// Note: this might seem like a bug but it is not:
	// _global_p = &_global_a;
	// *_global_p = 10;
	// Then _global_p is actually being read!
	/****************************/

	// in case of local var, do nothing. It is never _global_
	if(isa<AllocaInst>(val)){
	}
	// else, dig down the rabbit hole!
	else{
		digDownRead(val, getPointerLayer(val));	
	}
}

/** @brief Check if current writing val is _global_
 *  @param val     The value in question
 *  */
void isGlobalWrite(Value* val){
	/***********************************/
	// how it works:
	// for every store, check if the dest is reg (ex- %0) or user-defined var (%var), or global var.
	// user defined var is AllocaInst; regs are other Inst, global var is neither.
	// if it is global var, check is it is _global_ (nv var)
	// if user defined var, end. Never writing to nv var.
	// if it is reg, it might hold a pointer to user defined var. So we need to dig down the rabbit hole
	// until we meet the store that sets the reg, and see if it is indeed pointer to global_var.
	/***********************************/

	// if it is local var, do nothing
	if(isa<AllocaInst>(val)){
	}
	// else dig down the rabbit hole
	else{
		digDownWrite(val, getPointerLayer(val));	
	}
}

/** @brief iterate through all preceding inst to find who sets this pointer
 *  @param val     The value (potentially pointer) in question
 *  @param pointerLayer     The depth of pointer in question
 *  @param isRead	    Is we are looking for read, 1. Else, 0. 
 *  */
void digDownDeeper(Value* val, int pointerLayer, bool isRead){
	// hack to reverse_iterate since reverse_iterator does not work well
	BasicBlock::iterator i = curOp;
	BasicBlock::iterator curOp_local = curOp;
	curBb = curOp->getParent(); // will this fix everything?
	firstOp = curBb->begin();
	BasicBlock::iterator firstOp_local = firstOp;
	BasicBlock* curBb_local = curBb;

//	errs() << "curBb: " << *curBb << "\n";
	// reverse iterate over instruction
	while(firstOp != i){
//		errs() << "firstOp: " << *firstOp << "\n";
		--i;
//		errs() << "curOp: " << *i << "\n";
		Instruction* I = dyn_cast<Instruction>(i);
		// find store inst that potentially sets the pointer
		if(isa<StoreInst>(I) && I->getOperand(1) == val){
			curOp = I;
			// check if the value that the pointer is pointing is _global_
			if(isRead){
				insertPoint = I;
				digDownRead(I->getOperand(0), pointerLayer); 
			}
			else{
				replacePoint = I;
				digDownWrite(I->getOperand(0), pointerLayer);
			}
			// rollback curOp, firstOp, curBb on return
			curOp = curOp_local;
			firstOp = firstOp_local;
			curBb = curBb_local;
		}		
	}
	//if we reach the first Op and still can't find it, jump to previous block
	BasicBlock* B = curBb;
	// iterate through all the pred block
	for (auto it = pred_begin(B), et = pred_end(B); it != et; ++it)
	{
		curBb = *it;
		// visit only non-visited block
		if(!(std::find(visitedBb.begin(), visitedBb.end(), curBb) != visitedBb.end())){
			visitedBb.push_back(curBb);
			firstOp = curBb->begin();
			curOp = &(curBb->back());
			digDownDeeper(val, pointerLayer, isRead);
			// rollback curOp, firstOp, curBb on return
			curOp = curOp_local;
			firstOp = firstOp_local;
			curBb = curBb_local;
		}
	}
}

/** @brief iterate through to find if this is reading _global_
 *  @param val     The value in question
 *  @param pointerLayer     The depth of pointer in question (if it is pointer)
 *  */
void digDownRead(Value* val, int pointerLayer){
	/*********************************************/
	// see if this val holds pointer to _global_
	// to save pointer to a reg, it must use store.
	// and the value might gets passed around using load or gep or another store..
	// Our goal is to follow the load, gep, stores till we reach the source of the reg.
	// *ASSUMTION: nv_var never holds pointer for another nv var. nv vars are pointer only when it wants to hold address itself,
	// say like address of the next task.
	// It doesn't make sense for an nv var to act as an pointer.->In theory this seems like it should also work.
	// but doesn't for some reason. The name captured are blank.
	/*********************************************/
	Value* val2 = isGlobal(val, pointerLayer);
	// if val is _global_
	if(val2 != NULL){
		// if replace mode
		if(isReplace) {
			std::string varName;
			varName = val2->getName().str();
			std::map<std::string, Value*>::iterator it = valPointer.find(varName.c_str());
			// if the _global_ is WAR
			if(it != valPointer.end()){
				replacePoint = insertPoint;
				// replace operand to _bak
				replaceOperand(val, replacePoint);
				// if it is array, than privatization code must be inserted
				if(val2->getType()->getContainedType(0)->getTypeID() == Type::ArrayTyID){
					// it should always fall here! 
					if(isa<GEPOperator>(val) || isa<GEPOperator>(replacePoint)){
						// get pointer to element
						Value* gep;
						if(isa<GEPOperator>(val))
							gep = val;
						if(isa<GEPOperator>(replacePoint))
							gep = replacePoint;
						auto* val3 = dyn_cast<GEPOperator>(gep);

						// get index of element
						std::vector<Value*> arrayRef;
						for (llvm::User::op_iterator it2 = val3->idx_begin(); it2 != val3->idx_end(); it2++){
							arrayRef.push_back(it2->get());
						}	
						
						// get corresponding isDirty index
						GetElementPtrInst* isDirtyValPtr = llvm::GetElementPtrInst::CreateInBounds(curBb->getModule()->getNamedGlobal(val2->getName().str()+"_isDirty"),arrayRef, "",insertPoint);
						LoadInst* isDirtyVal = new LoadInst(isDirtyValPtr, "", insertPoint);	
						
						// load numBoots val
						LoadInst* numBoots = new LoadInst(curBb->getModule()->getNamedGlobal("_numBoots"), "", true, insertPoint);	
						
						// check if isDirty == numBoots?				
						ICmpInst* cond = new ICmpInst(insertPoint, llvm::CmpInst::Predicate::ICMP_NE, isDirtyVal, numBoots);	

						// privatization code
						GetElementPtrInst* nonPrivatizedValPtr = llvm::GetElementPtrInst::CreateInBounds(val2 ,arrayRef, "",insertPoint);
						LoadInst* nonPrivatizedVal = new LoadInst(nonPrivatizedValPtr, "", insertPoint);	
						GetElementPtrInst* privatizedValPtr = llvm::GetElementPtrInst::CreateInBounds(curBb->getModule()->getNamedGlobal(val2->getName().str()+"_bak"),arrayRef, "",insertPoint);
						StoreInst* str = new StoreInst(nonPrivatizedVal, privatizedValPtr, insertPoint);

						// insert branch
						curBbSaved = SplitBlock(curBb, insertPoint);
						BasicBlock* ifTrue = SplitBlock(curBb, nonPrivatizedValPtr);
						BranchInst* branch = BranchInst::Create(ifTrue, curBbSaved, cond);
						ReplaceInstWithInst(curBb->getTerminator(), branch);

						curBb = curBbSaved;
						isBlockSplitted = true;
					}
					else {
						errs() << "unknown unhandled corner case!\n";
					}
				}
			}
		}
		else {
			//if search mode, just mark it as read.
			std::string varName = val2->getName().str();
			//isR[varName.c_str()] = 1; //value being read
			if (readPerInst.find(curOpSaved) == readPerInst.end()) { //if this is the first time for this instruction
				std::vector<std::string> readVars;
				readVars.push_back(varName.c_str());
				readPerInst[curOpSaved] = readVars;
			}
			else { //if there is already a vector, append
				std::vector<std::string> readVars = readPerInst[curOpSaved];
				if (std::find(readVars.begin(), readVars.end(), varName.c_str()) == readVars.end()) {
					readVars.push_back(varName.c_str());
					readPerInst[curOpSaved] = readVars;
				}
			}
		}
	}
	else if(isa<GetElementPtrInst>(val)){ // GEP can assign pointer
		curOp = dyn_cast<Instruction>(val);
		auto* op2 = dyn_cast<GEPOperator>(val);
		insertPoint = dyn_cast<Instruction>(op2);
		digDownRead(op2->getPointerOperand(), pointerLayer);
	}
	else if(isa<BitCastInst>(val)){ // this maybe? can assign pointer
		curOp = dyn_cast<Instruction>(val);
		auto* op2 = dyn_cast<BitCastOperator>(val);
		insertPoint = dyn_cast<Instruction>(op2);
		digDownRead(op2->getOperand(0), pointerLayer);
	}
	else if(isa<LoadInst>(val)){ // this can only send values
		curOp = dyn_cast<Instruction>(val);
		auto* op2 = dyn_cast<LoadInst>(val);
		insertPoint = op2;
		digDownRead(op2->getOperand(0), pointerLayer);
	}
	else if(isa<AllocaInst>(val)){ // this may be pointer 
		digDownDeeper(val, pointerLayer, 1);
	}
}

/** @brief iterate through to find if this is writing _global_
 *  @param val     The value in question
 *  @param pointerLayer     The depth of pointer in question (if it is pointer)
 *  */
void digDownWrite(Value* val, int pointerLayer){
	/*********************************************/
	// see if this val holds pointer to _global_
	// to save pointer to a reg, it must use store.
	// and the value might gets passed around using load or gep or another store..
	// Our goal is to follow the load, gep, stores till we reach the source of the reg.
	// *ASSUMTION: nv_var never holds pointer for another nv var. nv vars are pointer only when it wants to hold address itself,
	// say like address of the next task.
	// It doesn't make sense for an nv var to act as an pointer.->In theory this seems like it should also work.
	// but doesn't for some reason. The name captured are blank.
	/*********************************************/
	Value* val2 = isGlobal(val, pointerLayer);
	// check if it is global
	if(val2 != NULL){
		// if replace mode
		if(isReplace) {
			std::string varName;
			if(val2->getName().str().find("_bak") != std::string::npos){
				varName = val2->getName().str().substr(0, val2->getName().str().find("_bak"));
			}
			else {
				varName = val2->getName().str();
			}
			std::map<std::string, Value*>::iterator it = valPointer.find(varName.c_str());
			// if it is WAR
			if(it != valPointer.end()){
				Value* gep;
				if(val2->getName().str().find("_bak") != std::string::npos){
					if(isa<GEPOperator>(val))
						gep = val;
					if(isa<GEPOperator>(replacePoint))
						gep = replacePoint;
				}
				else {
					// if it is not already _bak, replace
					gep = replaceOperand(val, replacePoint);
				}
				// if array
				if(val2->getType()->getContainedType(0)->getTypeID() == Type::ArrayTyID){ 
					// it should always fall here
					if(isa<GEPOperator>(val) || isa<GEPOperator>(replacePoint)){ 
						auto* val3 = dyn_cast<GEPOperator>(gep);
						// get index
						std::vector<Value*> arrayRef;
						for (llvm::User::op_iterator it2 = val3->idx_begin(); it2 != val3->idx_end(); it2++){
							arrayRef.push_back(it2->get());
						}	
						// get corresponding isDirty
						GetElementPtrInst* isDirtyValPtr = llvm::GetElementPtrInst::CreateInBounds(curBbSaved->getModule()->getNamedGlobal(varName+"_isDirty"),arrayRef, "",insertPointForWrite);
						LoadInst* isDirtyVal = new LoadInst(isDirtyValPtr, "", insertPointForWrite);	
						// get numBoots
						LoadInst* numBoots = new LoadInst(curBb->getModule()->getNamedGlobal("_numBoots"), "", true, insertPoint);	
						// isDirty == numBoots ?
						ICmpInst* cond = new ICmpInst(insertPointForWrite, llvm::CmpInst::Predicate::ICMP_NE, isDirtyVal, numBoots);	

						// when isDirty is 0 : write to gbuf, set is dirty
						GetElementPtrInst* nonPrivatizedValPtr = llvm::GetElementPtrInst::CreateInBounds(curBbSaved->getModule()->getNamedGlobal(varName),arrayRef, "",replacePoint);
						Instruction* writeToGbuf = write_to_gbuf(nonPrivatizedValPtr, gep, insertPointForWrite, *curFunc); 
						StoreInst* storeNumDirtyGv = new StoreInst(numBoots, isDirtyValPtr, insertPointForWrite); 
						
						//when isDirty is not 0 : do nothing

						// add branch
						BasicBlock* ifEnd = SplitBlock(curBb, insertPointForWrite);
						BasicBlock* ifTrue = SplitBlock(curBb, writeToGbuf);
						BranchInst* branch = BranchInst::Create(ifTrue, ifEnd, cond);
						ReplaceInstWithInst(curBb->getTerminator(), branch);

						curBbSaved = ifEnd;
						curBb = curBbSaved;
						isBlockSplitted = true;
					}
					else {
						errs() << "It should not reach here!\n";
					}
				}
			}
		}
		else {
			// on search mode, if it is being read before, it is WAR
			std::string varName = val2->getName().str();
			if (writePerInst.find(curOpSaved) == writePerInst.end()) { //if this is the first time for this instruction
				std::vector<std::string> writeVars;
				writeVars.push_back(varName.c_str());
				writePerInst[curOpSaved] = writeVars;
			}
			else { //if there is already a vector, append
				std::vector<std::string> writeVars = writePerInst[curOpSaved];
				if (std::find(writeVars.begin(), writeVars.end(), varName.c_str()) == writeVars.end()) {
					writeVars.push_back(varName.c_str());
					writePerInst[curOpSaved] = writeVars;
				}
			}
			/*
			std::map<std::string, bool>::iterator it = isR.find(varName.c_str());
			if(it != isR.end()){ //read before. now write. this is WAR!!
				markAsRW(val2);
			}
			else{
				// do nothing
			}*/
		}
	}
	else if(isa<GetElementPtrInst>(val)){ // GEP can assign pointer
		curOp = dyn_cast<Instruction>(val);
		auto* op2 = dyn_cast<GEPOperator>(val);
		replacePoint = dyn_cast<Instruction>(op2);
		digDownWrite(op2->getPointerOperand(), pointerLayer);
	}
	else if(isa<LoadInst>(val)){ // this can only send values
		curOp = dyn_cast<Instruction>(val);
		auto* op2 = dyn_cast<LoadInst>(val);
		replacePoint = dyn_cast<Instruction>(op2);
		digDownWrite(op2->getOperand(0), pointerLayer);
	}
	else if(isa<BitCastInst>(val)){ // this maybe? can assign pointer
		curOp = dyn_cast<Instruction>(val);
		auto* op2 = dyn_cast<BitCastOperator>(val);
		replacePoint = dyn_cast<Instruction>(op2);
		digDownWrite(op2->getOperand(0), pointerLayer);
	}
	//if oerand is user_defined variable, it is different between read and write
	//for write, only if the var is pointer and if it holds GlobalVar's address it might matter
	else if(isa<AllocaInst>(val)){ 
		digDownDeeper(val, pointerLayer, 0);
	}
	else if (isa<GlobalValue>(val)){
		digDownDeeper(val, pointerLayer, 0);
	}
}

/** @brief Iterate through function and check every read and write.
 *  @param F    Function in question 
 *  */
void checkRWInFunc(Function& F){
	bool first;
	bool firstB = 0;
	curFunc = &F;
	BasicBlock::iterator firstOpInBlock;
	BasicBlock::iterator begin;
	isBlockSplitted = false;

	// iterate through block
	// this does some nasty things in case branch is inserted along iteration to
	// deal with insertion.
	// Maybe better way is never changing the code inside iteration, but anyway it works
	for(Function::iterator block = F.begin(), end = F.end(); block!=end; ++block){
		if (!firstB){
			firstB = 1;
		}
		first = 0;
		curBbSaved = block;
		if(isBlockSplitted && insertPointForWrite != NULL){
			begin = insertPointForWrite;
		}
		else{
			begin = block->begin();
		}
		isBlockSplitted = false;
		for (BasicBlock::iterator instruction = begin, endblock = block->end(); instruction != endblock;){
			if(!first){
				first = 1;
				firstOpInBlock = *instruction;
			}
			curBb = curBbSaved;
			curOp = *instruction;
			//firstOp = firstOpInBlock;
			firstOp = curBb->begin();
			curOpSaved = *instruction;
			//firstOpSaved = firstOpInBlock;
			firstOpSaved = firstOp;
			visitedBb.clear();
			BasicBlock::iterator temp = instruction;
			Instruction* curInst = instruction;
			if(curInst != (curBbSaved->getTerminator())){
				++temp;
				insertPointForWrite = temp;
			}
			else {
				insertPointForWrite = NULL; //on digDownWrite, we don't need to consider NULL case because always task ends with transition_to. Write op never ends the task
			}
			if (auto *op = dyn_cast<Instruction>(instruction)){
				checkRW(op);
			}
			instruction = insertPointForWrite;
			curInst = instruction;
			if(curInst == NULL){
				if (isBlockSplitted){
					block = curBb;
				}
				break;
			}
		}
	}
}

/** @brief check If cur instruction is involved in Read or Write.
 *  @param op    Instruction in question 
 *  */
void checkRW(Instruction* op){ //if isReplace = 0, search Read or Write. if isReplace = 1, replace accordingly.
	insertPoint = op;
	replacePoint = op;

	// first, find write
	if(isa<StoreInst>(op)){ //if instruction is store
		isGlobalWrite(op->getOperand(1));
	}
	else if(isMemcpy(op)){ // memcpy can be write
		auto* op2 = dyn_cast<CallInst>(op);
		Function* func = op2->getCalledFunction();
		isGlobalWrite(op2->getArgOperand(0));
	}
	else if(isa<CallInst>(op)){ //other functions that takes pointer is all considered as write: We need to be conservative.
		auto* op2 = dyn_cast<CallInst>(op);
		Function* f = op2->getCalledFunction();
		if(f != NULL){
			std::map<std::string, non_task*>::iterator it = non_tasks.find(f->getName().str().c_str());
			// if internal function, 
			if(it != non_tasks.end()){
				search_funcCall(it->second);
			}
			// if it is not printf. We use printf a LOT and it NEVER writes so make an exception out of it!
			else if(f->getName().str().find("printf") == std::string::npos){
				for (int i = 0; i < op2->getNumOperands(); ++i){
					isGlobalWrite(op2->getOperand(i));
				}
			}
		}
	}
	// Must init firstOp, curOp, curBb
	curOp = curOpSaved;
	curBb = curBbSaved;
	firstOp = firstOpSaved;

	//check read
	if(isa<LoadInst>(op)){
		isGlobalRead(op->getOperand(0));
	}
	else if(isMemcpy(op)){
		auto* op2 = dyn_cast<CallInst>(op);
		Function* func = op2->getCalledFunction();
		isGlobalRead(op2->getArgOperand(1));
	}
	else if(isa<CallInst>(op)){ 
		auto* op2 = dyn_cast<CallInst>(op);
		Function* f = op2->getCalledFunction();
		if(f != NULL){
			// printf is for debugging. Don't analyze it. (or should we??)
			if(f->getName().str().find("printf") == std::string::npos){
				for (int i = 0; i < op2->getNumOperands(); ++i){
					isGlobalRead(op2->getOperand(i));
				}
			}
		}
	}
#if 0
	// how come this wasn't here??
	else if(!(isa<GetElementPtrInst>(op) || isa<BitCastInst>(op))){
		auto* op2 = dyn_cast<Instruction>(op);
		for (unsigned i = 0; i < op2->getNumOperands(); ++i) {
			isGlobalRead(op2->getOperand(i));
			// Must init firstOp, curOp, curBb // NEED FIX!!
			curOp = curOpSaved;
			curBb = curBbSaved;
			firstOp = firstOpSaved;
		}
	}
#endif
}
std::vector<BasicBlock*> visited;
bool found = false;
void searchBackwards(Instruction* I, std::string writeVal) {
	// hack to reverse_iterate since reverse_iterator does not work well

	// only for those whose not proven to be WAR
	if (valPointer.find(writeVal) == valPointer.end()) {
		Instruction* startInst = I->getParent()->begin();
		BasicBlock::iterator iter = I;
		while(startInst != iter){
			--iter;
			Instruction* I2 = dyn_cast<Instruction>(iter);
			if (readPerInst.find(I2) != readPerInst.end()) { // when cur inst is reading val
				std::vector<std::string> readVals = readPerInst.find(I2)->second;
				if (std::find(readVals.begin(), readVals.end(), writeVal) != readVals.end()) {
					// found!
					markAsRW(I->getParent()->getParent()->getParent()->getNamedValue(*(std::find(readVals.begin(), readVals.end(), writeVal))));
					found = 1;
					break;
				}
			}
		}
		if (!found) {
			//if we reach the first Op and still can't find it, jump to previous block
			BasicBlock* B = I->getParent();
			// iterate through all the pred block
			for (auto it = pred_begin(B), et = pred_end(B); it != et; ++it) {
				// visit only non-visited block
				if(std::find(visited.begin(), visited.end(), *it) == visited.end()){
					visited.push_back(*it);
					searchBackwards(&((*it)->back()), writeVal);
					if (found) break;
				}
			}
		}
	}
}

void searchWAR(Function& F) {
	for (auto &B : F) {
		for (auto &I : B) {
			if (writePerInst.find(&I) != writePerInst.end()) { //if it is writing to _global_
				// for every write vals, check if there was previous read
				std::vector<std::string> writeVals = (writePerInst.find(&I)->second);
				for (std::vector<std::string>::iterator wit = writeVals.begin(), we = writeVals.end(); wit != we; ++wit) {
					visited.clear();
					found = false;
					searchBackwards(&I, *wit);
				}
			}
		}
	}
}
