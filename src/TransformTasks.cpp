#include "include/TransformTasks.h"

Instruction* TransformTasks::insertPreCommit(Value* oldVal, Value* newVal, Instruction* insertBefore) {
	BitCastInst* oldbc = new BitCastInst(oldVal, Type::getInt8PtrTy(m->getContext()), "", insertBefore);
	BitCastInst* newbc = new BitCastInst(newVal, Type::getInt8PtrTy(m->getContext()), "", insertBefore);	

	// This part is a hacky way of calculating sizeof()
	std::vector<Value*> arrayRef;
	arrayRef.push_back(ConstantInt::get(Type::getInt16Ty(m->getContext()), 1, false));
	Value* size = GetElementPtrInst::CreateInBounds(Constant::getNullValue(oldVal->getType()), ArrayRef<Value*>(arrayRef), "", insertBefore);
	Value* sizei = CastInst::Create(CastInst::getCastOpcode(size, false, Type::getInt16Ty(m->getContext()),false), size, Type::getInt16Ty(m->getContext()), "", insertBefore);

	std::vector<Value*> args;
	args.push_back(newbc);
	args.push_back(oldbc);
	args.push_back(sizei);
	CallInst* call = CallInst::Create(write_to_gbuf, ArrayRef<Value*>(args), "", insertBefore);
	return oldbc;
}

/*
 * Inserts dynamic commit code after array write
 * 1) Compare isDirty and numBoots
 * 2) if not same, commit
 */
void TransformTasks::insertDynamicCommit(Instruction* storeBegin, Instruction* storeEnd, Value* orig, Value* priv) {
	// get index of the array getting written
	std::vector<Value*> arrayRef;
	//auto gep = cast<GEPOperator>(storeBegin);
	GEPOperator* gep;
	if (!(gep = dyn_cast<GEPOperator>(storeBegin))) {
		// hacky temporary bug fix
		gep = cast<GEPOperator>(storeBegin->getOperand(1));
	}
	for (User::op_iterator OI = gep->idx_begin(); OI != gep->idx_end(); ++OI){
		arrayRef.push_back(OI->get());
	}	

	// get corresponding isDirty
	GetElementPtrInst* isDirtyValPtr = GetElementPtrInst::CreateInBounds(m->getNamedGlobal(orig->getName().str()+"_isDirty"), arrayRef, "", storeEnd);
	LoadInst* isDirtyVal = new LoadInst(isDirtyValPtr, "", storeEnd);

	// get numBoots
	LoadInst* numBoots = new LoadInst(m->getNamedGlobal("_numBoots"), "", true, storeEnd);	

	// isDirty == numBoots ?
	ICmpInst* cond = new ICmpInst(storeEnd, CmpInst::Predicate::ICMP_NE, isDirtyVal, numBoots);	

	// when isDirty is 0 : write to gbuf, set is dirty
	GetElementPtrInst* nonPrivatizedValPtr = GetElementPtrInst::CreateInBounds(orig, arrayRef, "", storeEnd);
	Instruction* writeToGbuf = insertPreCommit(nonPrivatizedValPtr, gep, storeEnd); 
	StoreInst* storeNumDirtyGv = new StoreInst(numBoots, isDirtyValPtr, storeEnd); 

	//when isDirty is not 0 : do nothing

	// add branch
	BasicBlock* curBb = storeEnd->getParent();
	BasicBlock* ifEnd = SplitBlock(curBb, storeEnd);
	BasicBlock* ifTrue = SplitBlock(curBb, writeToGbuf);
	BranchInst* branch = BranchInst::Create(ifTrue, ifEnd, cond);
	ReplaceInstWithInst(curBb->getTerminator(), branch);
}


/*
 * Inserts dynamic privatization code before array read
 * 1) Compare isDirty and numBoots
 * 2) if not same, privatize
 */
void TransformTasks::insertDynamicPriv(Instruction* I, Value* orig, Value* priv) {
	// get index of the array getting read
	std::vector<Value*> arrayRef;
	GEPOperator* gep;
	if (!(gep = dyn_cast<GEPOperator>(I))) {
		// hacky temporary bug fix
		gep = cast<GEPOperator>(I->getOperand(0));
	}
//	auto gep = cast<GEPOperator>(I);
	for (User::op_iterator OI = gep->idx_begin(); OI != gep->idx_end(); ++OI){
		arrayRef.push_back(OI->get());
	}	

	// load correspoinding isDirty Array
	GetElementPtrInst* isDirtyValPtr = GetElementPtrInst::CreateInBounds(m->getNamedGlobal(orig->getName().str()+"_isDirty"), arrayRef, "", I);
	LoadInst* isDirtyVal = new LoadInst(isDirtyValPtr, "", I);	

	// load numBoots val
	LoadInst* numBoots = new LoadInst(m->getNamedGlobal("_numBoots"), "", true, I);	

	// check if isDirty == numBoots?				
	ICmpInst* cond = new ICmpInst(I, CmpInst::Predicate::ICMP_NE, isDirtyVal, numBoots);	

	// privatization code
	GetElementPtrInst* nonPrivatizedValPtr = GetElementPtrInst::CreateInBounds(orig, arrayRef, "", I);
	LoadInst* nonPrivatizedVal = new LoadInst(nonPrivatizedValPtr, "", I);	
	GetElementPtrInst* privatizedValPtr = GetElementPtrInst::CreateInBounds(priv, arrayRef, "", I);
	StoreInst* str = new StoreInst(nonPrivatizedVal, privatizedValPtr, I);

	// insert branch
	BasicBlock* curBb = I->getParent();
	BasicBlock* ifFalse = SplitBlock(curBb, I);
	BasicBlock* ifTrue = SplitBlock(curBb, nonPrivatizedValPtr);
	BranchInst* branch = BranchInst::Create(ifTrue, ifFalse, cond);
	ReplaceInstWithInst(curBb->getTerminator(), branch);
}

inst_inst_vec TransformTasks::getArrWritePoint(Value* orig, Value* priv, Function* F) {
	AliasAnalysis* AAA = &pass->getAnalysis<AAResultsWrapperPass>(*(F)).getAAResults();
	inst_inst_vec writePoint;
	for (auto &B : *F) {
		for (auto &I : B) {
			if (isa<StoreInst>(&I)) {
				CustomAlias* CA = new CustomAlias(AAA, &I);
				inst_vec a = CA->alias(I.getOperand(1), cast<GlobalVariable>(priv), &I);
				if (a.size() != 0) {
					assert(a.size() == 1);
					writePoint.push_back(std::make_pair(a.at(0), &I));
				}
			}
			else if(CallInst* ci = dyn_cast<CallInst>(&I)) {
				if (isMemcpy(ci)) {
					// memcpy is a write
					CustomAlias* CA = new CustomAlias(AAA, &I);
					inst_vec a = CA->alias(I.getOperand(0), cast<GlobalVariable>(priv), &I);
					if (a.size() != 0) {
						assert(a.size() == 1);
						writePoint.push_back(std::make_pair(a.at(0), &I));
					}
				}
			}
		}
	}
	return writePoint;
}

inst_vec TransformTasks::getArrReadPoint(Value* orig, Value* priv, Function* F) {
	AliasAnalysis* AAA = &pass->getAnalysis<AAResultsWrapperPass>(*(F)).getAAResults();
	inst_vec readPoint;
	for (auto &B : *F) {
		for (auto &I : B) {
			if (isa<LoadInst>(&I)) {
				CustomAlias* CA = new CustomAlias(AAA, &I);
				inst_vec a = CA->alias(I.getOperand(0), cast<GlobalVariable>(priv), &I);
				readPoint.insert(readPoint.end(), a.begin(), a.end());
			}
			else if(CallInst* ci = dyn_cast<CallInst>(&I)) {
				if (isMemcpy(ci)) {
					// memcpy is a write
					CustomAlias* CA = new CustomAlias(AAA, &I);
					inst_vec a = CA->alias(I.getOperand(1), cast<GlobalVariable>(priv), &I);
					readPoint.insert(readPoint.end(), a.begin(), a.end());
				}
			}
		}
	}
	return readPoint;
}

void TransformTasks::privatize(Instruction* firstInst, Value* v) {
	Value* orig = new LoadInst(v, "", false, firstInst);
	Value* priv = m->getNamedValue(v->getName().str()+"_bak");
	StoreInst* st = new StoreInst(orig, priv, firstInst);

}

void TransformTasks::runTransformation(func_vals_map WARinFunc) {
	for (auto &F : *m) {
		if (!F.empty()) {
			// task transformation
			val_vec WARs = WARinFunc[&F];

			// change every usage of WAR to priv buffer
			replaceToPriv(&F, WARs);

			Instruction* firstInst = &F.front().front();
			for (val_vec::iterator VI = WARs.begin(); VI != WARs.end(); ++VI) {
				if (!isArray(*VI)) {
					// non-array privatization and commit only in tasks
					if (isTask(&F)) {
						// 1) For non-array
						// insert privatization
						privatize(firstInst, *VI);

						// insert pre-commit
						for (auto &B : F) {
							for (auto &I : B) {
								if (isTransitionTo(&I)) {
									insertPreCommit(*VI, 
											m->getNamedValue((*VI)->getName().str()+"_bak"),
											&I);
								}
							}
						}
					}
				}
				else {
					// 2) For array
					// insert isDirty operation
					// (You may want to do backward search again)

					// 2.1) Before any read, insert some ops
					// 2.2) After every write, insert some ops
					//					checkAfterArrWrite(*VI);
					GlobalValue* gv_bak = m->getNamedValue((*VI)->getName().str()+"_bak");

					inst_vec readPoint = getArrReadPoint(*VI, gv_bak, &F);
					inst_inst_vec writePoint = getArrWritePoint(*VI, gv_bak, &F);

					for (inst_vec::iterator II = readPoint.begin();
							II != readPoint.end(); ++II) {
						insertDynamicPriv(*II, *VI, gv_bak);
					}

					for (inst_inst_vec::iterator II = writePoint.begin();
							II != writePoint.end(); ++II) {
						// pass the next instruction because 
						// every insertion works as "insertBefore"
						BasicBlock::iterator BI = BasicBlock::iterator((II)->second);
						++BI;
						// We need to pass two instruction, 
						// beginning of store (loading address for array)
						// and the end of store (the actual store)
						insertDynamicCommit((II)->first, BI, *VI, gv_bak);
					}
				}
			}
		}
	}
}

/*
 * Replace all the usage of WAR vals to its privatized copy
 */
void TransformTasks::replaceToPriv(Function* F, val_vec WARs) {
	for (auto &B : *F) {
		for (auto &I : B) {
			for (unsigned i = 0; i < I.getNumOperands(); ++i) {
				Value* v = I.getOperand(i);
				// replaceUsesOfWrite() does not work on gep or bitcast (is it bug?)
				// so we take a workaround
				if(GEPOperator* gep = dyn_cast<GEPOperator>(v)){
					Value* target = gep->getPointerOperand();
					val_vec::iterator VI = std::find(WARs.begin(), WARs.end(), target);
					if (VI != WARs.end()) {
						// target is WAR. need substitution
						val_vec arrayRef;
						for (User::op_iterator OI = gep->idx_begin();
								OI != gep->idx_end(); ++OI){
							arrayRef.push_back(OI->get());
						}
						GetElementPtrInst* newGep = GetElementPtrInst::CreateInBounds(
								F->getParent()->getNamedValue((*VI)->getName().str()+"_bak"), 
								ArrayRef<Value*>(arrayRef), "", &I);
						I.replaceUsesOfWith(v, newGep);
					}
				}
				// replaceUsesOfWrite() does not work on gep or bitcast (is it bug?)
				// so we take a workaround
				else if(BitCastOperator* bc = dyn_cast<BitCastOperator>(v)){
					Value* target = bc->getOperand(0);
					val_vec::iterator VI = std::find(WARs.begin(), WARs.end(), target);
					if (VI != WARs.end()) {
						// target is WAR. need substitution
						BitCastInst* newBc = new BitCastInst(
								F->getParent()->getNamedValue((*VI)->getName().str()+"_bak"), 
								v->getType(), "", &I);
						I.replaceUsesOfWith(v , newBc);
					}
				}
				// else just use replaceUsesOfWrite
				else{
					val_vec::iterator VI = std::find(WARs.begin(), WARs.end(), v);
					if (VI != WARs.end()) {
						// target is WAR. need substitution
						I.replaceUsesOfWith(v, 
								F->getParent()->getNamedValue((*VI)->getName().str()+"_bak"));
					}
				}
			}
		}
	}
}
