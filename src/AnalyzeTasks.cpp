#include "include/AnalyzeTasks.h"

void AnalyzeTasks::runNonTaskAnalysis(Module& M) {
	for (auto &F : M) {
		if (!F.empty()) {
			if (!isTask(&F)) {
				if (WARinNonTask.find(&F) == WARinNonTask.end()) {
					WARinNonTask[&F] = new non_task();
				}
				for (auto &B : F) {
					for (auto &I : B) {
						// if a non-task function ever uses any global, consider as WAR
						for(int i = 0; i < I.getNumOperands(); ++i){
							// check if every inst contains val in gv_list
							Value* operand;
							if (GEPOperator* gep = dyn_cast<GEPOperator>(I.getOperand(i))) {
								operand = gep->getPointerOperand();
							}
							else if (BitCastOperator* bc = dyn_cast<BitCastOperator>(I.getOperand(i))) {
								operand = bc->getOperand(0);
							}
							else{
								operand = I.getOperand(i);
							}

							for(gv_vec::iterator GI = gv_list.begin(); GI != gv_list.end(); ++GI){
								// if GV is being used, write it down.
								if (operand == *GI) {
									// first check if it is already written 
									if (std::find(WARinNonTask[&F]->usedGlobal.begin(), 
												WARinNonTask[&F]->usedGlobal.end(), operand) 
											== WARinNonTask[&F]->usedGlobal.end() ) {
										WARinNonTask[&F]->usedGlobal.push_back(operand);
									}	
								}
							}
						}

						// if it calls another function, write down the call structure
						if (CallInst* ci = dyn_cast<CallInst>(&I)){
							Function* calledF = ci->getCalledFunction();
							if (calledF != NULL) {
								if (!isTask(calledF)) { // sanity check. It cannot be task anyway
									if (WARinNonTask.find(calledF) == WARinNonTask.end()) {
										// first time this function is seen
										if (!calledF->empty()) {
											WARinNonTask[calledF] = new non_task();
											WARinNonTask[&F]->callee.push_back(WARinNonTask[calledF]);
										}
									}
									else{
										assert(!calledF->empty());
										WARinNonTask[&F]->callee.push_back(WARinNonTask.find(calledF)->second);
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void AnalyzeTasks::runTaskAnalysis(Module &M) {
	// first, run non task analysis
	runNonTaskAnalysis(M);
	// then, run task analysis
	for (auto &F : M) {
		visitedFunc.clear();
		if (!F.empty()) {
			// run only on tasks
			if (isTask(&F)) {
				errs() << F.getName() << "\n";
				val_insts_map Writelist;
				val_vec WARlist;
				// search for WAR

				// search for Write to __global__
				for (auto &B : F) {
					for (auto &I : B) {
						if (isa<StoreInst>(&I)) {
							findWriteToGlobal(I.getOperand(1), &I, &Writelist);
						}
						else if(CallInst* ci = dyn_cast<CallInst>(&I)) {
							if (isMemcpy(ci)) {
								// memcpy is a write
								findWriteToGlobal(ci->getArgOperand(0), &I, &Writelist);
							}
						}
					}
				}

				// check if there is read to the value
				for (auto &B : F) {
					for (auto &I : B) {
						if (isa<LoadInst>(&I)) {
							findReadPrecedingWrite(I.getOperand(0), &I, &Writelist, &WARlist);
						}
						else if(CallInst* ci = dyn_cast<CallInst>(&I)) {
							if (isMemcpy(ci)) {
								// memcpy is a write
								findReadPrecedingWrite(I.getOperand(1), &I, &Writelist, &WARlist);
							}
						}
					}
				}

				// special cases: func call inside tasks
				for (auto &B : F) {
					for (auto &I : B) {
						if (CallInst* ci = dyn_cast<CallInst>(&I)) {
							// memcpy is handled earlier as a special case
							if (!isMemcpy(ci) && !isTransitionTo(ci)) {
								Function* calledF = ci->getCalledFunction();
								if (calledF != NULL) {
									std::map<Function*, non_task*>::iterator MI = WARinNonTask.find(calledF);
									// if known func uses GV, it is WAR
									if (MI != WARinNonTask.end()) {
										// if internal function
										addWARinNonTask(MI->second, &WARlist);
									}
									else {
										// if unknown function takes a pointer to a GV, it is WAR
										// it is added to WAR
										for (unsigned i = 0; i < ci->getNumArgOperands(); ++i) {
											AliasAnalysis* AAA =
												&pass->getAnalysis<AAResultsWrapperPass>(F).getAAResults();

											for(gv_vec::iterator GI = gv_list.begin(); GI != gv_list.end(); ++GI){
												bool isAlias = false;
												AliasResult result = AAA->alias(ci->getArgOperand(i), *GI);
												if (result == AliasResult::MustAlias) {
													isAlias = true;
												}	
												// in case of May Alias, do our own search
												else if(result == AliasResult::MayAlias) {
													CustomAlias* CA = new CustomAlias(AAA, ci);
													if ((CA->alias(ci->getArgOperand(i), *GI, ci)).size() != 0) {
														isAlias = true;
													}
												}

												if (isAlias) {
													// add to WAR
													if (std::find(WARlist.begin(), WARlist.end(), 
																ci->getArgOperand(i)) == WARlist.end()) {
														WARlist.push_back(ci->getArgOperand(i));
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}

				// calculate commit size
				uint64_t commitSize = calcCommitSize(WARlist);
				if (commitSize > maxCommitSize) {
					maxCommitSize = commitSize;
				}
				// save the result
				WARinFunc[&F] = WARlist;
			}
			else {
				// for non-tasks
				WARinFunc[&F] = WARinNonTask[&F]->usedGlobal;
			}
		}
	}
}

uint64_t AnalyzeTasks::calcCommitSize(val_vec WARlist) {
	uint64_t commitSize = 0;
	for (val_vec::iterator VI = WARlist.begin(); 
			VI != WARlist.end(); ++VI) {
//		errs() << "calc commit size! " << (*VI)->getName() << "\n";
		commitSize += getSize(*VI);
	}
	return commitSize;
}

void AnalyzeTasks::addWARinNonTask(non_task* curTask, val_vec* WARlist) {
	visitedFunc.push_back(curTask);
	for (val_vec::iterator VI = curTask->usedGlobal.begin(); 
			VI != curTask->usedGlobal.end(); ++VI) {
		// add to WARlist
		if (std::find(WARlist->begin(), WARlist->end(), *VI) == WARlist->end()) {
			WARlist->push_back(*VI);
		}
	}
	for (std::vector<non_task*>::iterator NI = curTask->callee.begin();
			NI != curTask->callee.end(); ++NI) {
		if (std::find(visitedFunc.begin(), visitedFunc.end(), *NI)
				== visitedFunc.end()) {
			addWARinNonTask(*NI, WARlist);
		}
	}
}

void AnalyzeTasks::findWriteToGlobal(Value* v, 
		Instruction* I, val_insts_map* Writelist) {
	AliasAnalysis* AAA = 
		&pass->getAnalysis<AAResultsWrapperPass>(*(I->getParent()->getParent())).getAAResults();

	for(gv_vec::iterator GI = gv_list.begin(); GI != gv_list.end(); ++GI){
		AliasResult result = AAA->alias(v,*GI);

		bool isAlias = false;
		// if Must Alias
		if (result == AliasResult::MustAlias) {
			isAlias = true;
		}	
		// in case of May Alias, do our own search
		else if(result == AliasResult::MayAlias) {
			CustomAlias* CA = new CustomAlias(AAA, I);
			if ((CA->alias(v, *GI, I)).size() != 0) {
				isAlias = true;
			}
		}

		if (isAlias) {
			val_insts_map::iterator IT = Writelist->find(*GI);
			if (IT != Writelist->end()) {
				// map for GV already exists; append
				inst_vec write_insts = IT->second;
				assert(write_insts.size() != 0);
				write_insts.push_back(I);
				(*Writelist)[*GI] = write_insts;
			}
			else {
				// first time writing to GV
				inst_vec write_insts;
				write_insts.push_back(I);
				(*Writelist)[*GI] = write_insts;
			}
		}
	}
}

void AnalyzeTasks::findReadPrecedingWrite(Value* v, Instruction* I, val_insts_map* Writelist, val_vec* WARlist) {
	AliasAnalysis* AAA = &pass->getAnalysis<AAResultsWrapperPass>(*(I->getParent()->getParent())).getAAResults();

	for(gv_vec::iterator GI = gv_list.begin(); GI != gv_list.end(); ++GI){
		AliasResult result = AAA->alias(v,*GI);

		bool isAlias = false;
		// if Must Alias
		if (result == AliasResult::MustAlias) {
			isAlias = true;
		}	
		// in case of May Alias, do our own search
		else if(result == AliasResult::MayAlias) {
			CustomAlias* CA = new CustomAlias(AAA, I);
			if ((CA->alias(v, *GI, I)).size() != 0) {
				isAlias = true;
			}
		}

		if (isAlias) {
			val_insts_map::iterator IT = Writelist->find(*GI);
			if (IT != Writelist->end()) {
				// this function wrote to GI. Check if Write is after Read
				inst_vec write_insts = IT->second;
				assert(write_insts.size() != 0);
				for (inst_vec::iterator IIT = write_insts.begin(); IIT != write_insts.end(); ++IIT) {
					BackwardSearcher* BS = new BackwardSearcher();
					if (BS->isPreceding(I, (*IIT))) {
						// if read precede write
						// insert GI to WARlist
						if (std::find(WARlist->begin(), WARlist->end(), *GI) == WARlist->end()) {
							errs() << "This is WAR " << (*GI)->getName() << "\n";
							WARlist->push_back(*GI);
						}
					}
				}
			}
			else {
				// This func never wrote to GI.
			}
		}
	}
}
