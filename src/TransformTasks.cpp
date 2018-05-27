#include "include/TransformTasks.h"

Instruction* TransformTasks::insertLogBackup(Value* oldVal, Value* newVal, Instruction* insertBefore) {
	BitCastInst* oldbc = new BitCastInst(oldVal, Type::getInt8PtrTy(m->getContext()), "", insertBefore);
	BitCastInst* newbc = new BitCastInst(newVal, Type::getInt8PtrTy(m->getContext()), "", insertBefore);	

	// This part is a hacky way of calculating sizeof()
	std::vector<Value*> arrayRef;
	arrayRef.push_back(ConstantInt::get(Type::getInt16Ty(m->getContext()), 1, false));
	Value* size = GetElementPtrInst::CreateInBounds(Constant::getNullValue(oldVal->getType()), ArrayRef<Value*>(arrayRef), "", insertBefore);
	Value* sizei = CastInst::Create(CastInst::getCastOpcode(size, false, Type::getInt16Ty(m->getContext()),false), size, Type::getInt16Ty(m->getContext()), "", insertBefore);

	std::vector<Value*> args;
	args.push_back(oldbc);
	args.push_back(newbc);
	args.push_back(sizei);
	CallInst* call = CallInst::Create(log_backup, ArrayRef<Value*>(args), "", insertBefore);
	return oldbc;
}

void TransformTasks::setGPIO(Instruction* I) {
#if OVERHEAD == 1
	LoadInst* ldr = new LoadInst(Type::getInt8Ty(m->getContext()), m->getNamedGlobal("PBOUT_L"), "gpioreg", true, 1, I);
	ZExtInst* zxt = new ZExtInst(ldr, Type::getInt16Ty(m->getContext()), "zext", I);
	ConstantInt* i32 = ConstantInt::get(Type::getInt16Ty(m->getContext()), 32, false);
	BinaryOperator* bi = BinaryOperator::Create(Instruction::Or, zxt, i32, "or", I);
	TruncInst* ti = new TruncInst(bi, Type::getInt8Ty(m->getContext()), "trunc", I);
	StoreInst* str = new StoreInst(ti, m->getNamedGlobal("PBOUT_L"), true, 1, I);
#endif
}

void TransformTasks::unsetGPIO(Instruction* I) {
#if OVERHEAD == 1
	LoadInst* ldr = new LoadInst(Type::getInt8Ty(m->getContext()), m->getNamedGlobal("PBOUT_L"), "gpioreg", true, 1, I);
	ZExtInst* zxt = new ZExtInst(ldr, Type::getInt16Ty(m->getContext()), "zext", I);
	ConstantInt* m33 = ConstantInt::get(Type::getInt16Ty(m->getContext()), -33, false);
	BinaryOperator* bi = BinaryOperator::Create(Instruction::And, zxt, m33, "and", I);
	TruncInst* ti = new TruncInst(bi, Type::getInt8Ty(m->getContext()), "trunc", I);
	StoreInst* str = new StoreInst(ti, m->getNamedGlobal("PBOUT_L"), true, 1, I);
#endif
}

/*
 * Inserts dynamic privatization code before array read
 * 1) Compare isDirty and numBoots
 * 2) if not same, privatize
 */
void TransformTasks::insertDynamicBackup(Instruction* I, Value* orig, Value* priv) {
	// get index of the array getting read
	std::vector<Value*> arrayRef;
	GEPOperator* gep;
	if (!(gep = dyn_cast<GEPOperator>(I))) {
		// hacky temporary bug fix
		gep = cast<GEPOperator>(I->getOperand(1));
	}
//	auto gep = cast<GEPOperator>(I);
	for (User::op_iterator OI = gep->idx_begin(); OI != gep->idx_end(); ++OI){
		arrayRef.push_back(OI->get());
	}	
	
	// for overhead measurement
	setGPIO(I);

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

	// insert backup log
	insertLogBackup(nonPrivatizedValPtr, privatizedValPtr, I); 
	// set isDirty
	StoreInst* storeNumDirtyGv = new StoreInst(numBoots, isDirtyValPtr, I); 

	// insert branch
	BasicBlock* curBb = I->getParent();
	BasicBlock* ifFalse = SplitBlock(curBb, I);
	BasicBlock* ifTrue = SplitBlock(curBb, nonPrivatizedValPtr);
	BranchInst* branch = BranchInst::Create(ifTrue, ifFalse, cond);
	ReplaceInstWithInst(curBb->getTerminator(), branch);
	unsetGPIO(&ifFalse->front());
}

inst_inst_vec TransformTasks::getArrWritePoint(Value* val, Function* F) {
	AliasAnalysis* AAA = &pass->getAnalysis<AAResultsWrapperPass>(*(F)).getAAResults();
	inst_inst_vec writePoint;
	for (auto &B : *F) {
		for (auto &I : B) {
			if (isa<StoreInst>(&I)) {
				CustomAlias* CA = new CustomAlias(AAA, &I);
				inst_vec a = CA->alias(I.getOperand(1), cast<GlobalVariable>(val), &I);
				if (a.size() != 0) {
					assert(a.size() == 1);
					writePoint.push_back(std::make_pair(a.at(0), &I));
				}
			}
			else if(CallInst* ci = dyn_cast<CallInst>(&I)) {
				if (isMemcpy(ci)) {
					// memcpy is a write
					CustomAlias* CA = new CustomAlias(AAA, &I);
					inst_vec a = CA->alias(I.getOperand(0), cast<GlobalVariable>(val), &I);
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
/*
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
}*/

void TransformTasks::backup(Instruction* firstInst, Value* v) {
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
			// replaceToPriv(&F, WARs);
			Instruction* firstInst = &F.front().front();

			// For overhead measurement only
			bool scalarUndoLogged = false;

			for (val_vec::iterator VI = WARs.begin(); VI != WARs.end(); ++VI) {
				if (!isArray(*VI)) {
					// non-array privatization and commit only in tasks
					if (isTask(&F)) {
						// 1) For non-array
						// insert backup
						if (!scalarUndoLogged) {
							// When it is the first time to undo log
							setGPIO(firstInst);
							scalarUndoLogged = 1;
						}
						backup(firstInst, *VI);

						// log the backup
						insertLogBackup(*VI, 
								m->getNamedValue((*VI)->getName().str()+"_bak"),
								firstInst);
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

					//inst_vec readPoint = getArrReadPoint(*VI, gv_bak, &F);
					inst_inst_vec writePoint = getArrWritePoint(*VI, &F);

					//for (inst_vec::iterator II = readPoint.begin();
					//		II != readPoint.end(); ++II) {
					//	insertDynamicPriv(*II, *VI, gv_bak);
					//}

					for (inst_inst_vec::iterator II = writePoint.begin();
							II != writePoint.end(); ++II) {
						// pass the next instruction because 
						// every insertion works as "insertBefore"
						//BasicBlock::iterator BI = BasicBlock::iterator((II)->second);
						//++BI;
						// We need to pass two instruction, 
						// beginning of store (loading address for array)
						// and the end of store (the actual store)
						//insertDynamicCommit((II)->first, BI, *VI, gv_bak);
						insertDynamicBackup(II->second, *VI, gv_bak);
					}
				}
			}
			if (scalarUndoLogged) {
				// only for overhead measurement
				unsetGPIO(firstInst);
			}
		}
	}
}

/*
 * Replace all the usage of WAR vals to its privatized copy
 */
/*
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
}*/
