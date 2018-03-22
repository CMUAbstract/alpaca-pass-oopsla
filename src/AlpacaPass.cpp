#include "include/AlpacaPass.h"
#include "include/searchRW.h"
#include "include/replace.h"
#include <algorithm>

#define ALLOCA 32
#define STORE 34
#define CALL 55

/** @brief Mark as read */
std::map<Instruction*, std::vector<std::string>> readPerInst;
/** @brief Mark as write */
std::map<Instruction*, std::vector<std::string>> writePerInst;



/** @brief Mark as read */
std::map<std::string,bool> isR;
/** @brief Map that holds _global_ vars */
std::map<std::string,Value*> valPointer;
/** @brief Map that holds _global__bak vars */
std::map<std::string,Value*> newValPointer;
/** @brief List of visited basic blocks */
std::vector<BasicBlock*> visitedBb;
/** @brief Function write_to_gbuf (pre-commit) */
Function* wtg;	
/** @brief Fast custom memset */
Function* array_memset;	
/** @brief Current instruction */
BasicBlock::iterator curOp;
/** @brief First instruction of bb --TODO: move it to searchRW.cpp */
BasicBlock::iterator firstOp;
/** @brief First bb --TODO: move it to searchRW.cpp */
BasicBlock* curBb;
/** @brief Backup curOp when searching recursively -- TODO: to searchRW.cpp */
BasicBlock::iterator curOpSaved;
/** @brief Backup fristOp when searching recursively -- TODO: to searchRW.cpp */
BasicBlock::iterator firstOpSaved;
/** @brief Backup curBb when searching recursively -- TODO: to searchRW.cpp */
BasicBlock* curBbSaved;
/** @brief List of functions that are not tasks */
std::map<std::string, non_task*> non_tasks;
//DataLayout* dl;
/** @brief num of war vars on current task */
uint64_t dirtyBufSize;
/** @brief max num of war vars in current app */
uint64_t maxDirtyBufSize;
/** @brief flag to searchRW.cpp -- is it replace mode or analysis mode?*/
bool isReplace;
/** @brief list of isDirty arrays */
std::vector<GlobalVariable*> isDirtys;

namespace {
	/**
	 * @brief Add clear_isDirty() function.
	 */
	void add_clear_isDirty(Module &M){
		// insert function named clear_isDirty
		Constant* c = M.getOrInsertFunction("clear_isDirty",llvm::Type::getVoidTy(getGlobalContext()),
				NULL);
		Function* clear_isDirty = cast<Function>(c);
		clear_isDirty->setCallingConv(CallingConv::C);

		// create basic block inside the function
		BasicBlock* block = BasicBlock::Create(getGlobalContext(), "entry", clear_isDirty);

		// insert memset for every isDirty
		for (std::vector<GlobalVariable*>::iterator it = isDirtys.begin(); it != isDirtys.end(); ++it){	
			// This part is a hacky way of calculating sizeof()
			BitCastInst* arraybc = new BitCastInst(*it, llvm::Type::getInt8PtrTy(getGlobalContext()), "",block);	
			std::vector<Value*> arrayRef;
			arrayRef.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(getGlobalContext()),1, false));
			Value* size = llvm::GetElementPtrInst::CreateInBounds(llvm::Constant::getNullValue((*it)->getType()), ArrayRef<Value*>(arrayRef), "", block);
			Value* sizei = llvm::CastInst::Create(llvm::CastInst::getCastOpcode(size, false, llvm::Type::getInt16Ty(getGlobalContext()),false), size, llvm::Type::getInt16Ty(getGlobalContext()), "", block);

			// insert custom fast memset (because mspgcc memset is slow)
			Value* zero = llvm::ConstantInt::get(llvm::Type::getInt16Ty(getGlobalContext()), 0);
			std::vector<Value*> args;
			args.push_back(arraybc);
			args.push_back(zero);
			args.push_back(sizei);
			CallInst* call = llvm::CallInst::Create(array_memset, ArrayRef<Value*>(args), "", block);
		}
		// insert return inst
		ReturnInst::Create(getGlobalContext(), block);
	}

	/**
	 * @brief Alpaca Pass
	 */
	struct AlpacaModulePass : public ModulePass {
		static char ID;
		AlpacaModulePass() : ModulePass(ID) {}

		bool isDeclared = false; // flag for declaring functions
		bool isAnalyzingTask = false; // first analyze non-task functions, and then tasks
		bool isSetDirtyBufSize = false; // flag to declare dirtylist (commit-list)

		/**
		 * @brief Body of pass
		 */
		virtual bool runOnModule(Module &M){
			// init maxDirtyBufSize
			maxDirtyBufSize = 0;
			// run multiple times for each function (declare funcs -> analysis/transform non-tasks -> analysis/transform tasks -> declare dirtylist)
			while(true){
				// for every function,
				for (auto &F : M){
					// init dirtyBufSize
					dirtyBufSize = 0;
					// is this is set, time to insert dirtylist!
					if(isSetDirtyBufSize){
						// declare source dirtylist
						StringRef* name0 = new StringRef("data_src");
						const Twine tname0 = llvm::Twine(*name0);
						GlobalVariable* data_global = new GlobalVariable(M, ArrayType::get(Type::getInt8PtrTy(F.getContext()), maxDirtyBufSize), false, GlobalValue::ExternalLinkage, 0, tname0, M.getNamedGlobal("data_base"));
						data_global->setAlignment(2);
						data_global->setSection(".nv_vars");
						std::vector<Constant*> arr;
						Constant* initData = ConstantArray::get(ArrayType::get(Type::getInt8PtrTy(F.getContext()), maxDirtyBufSize), ArrayRef<Constant*>(arr));
						data_global->setInitializer(initData);	

						// declare dest dirtylist
						StringRef* name1 = new StringRef("data_dest");
						const Twine tname1 = llvm::Twine(*name1);
						GlobalVariable* data_dest_global = new GlobalVariable(M, ArrayType::get(Type::getInt8PtrTy(F.getContext()), maxDirtyBufSize), false, GlobalValue::ExternalLinkage, 0, tname1, M.getNamedGlobal("data_base"));
						data_dest_global->setAlignment(2);
						std::vector<Constant*> arr1;
						Constant* init_data_dest = ConstantArray::get(ArrayType::get(Type::getInt8PtrTy(F.getContext()), maxDirtyBufSize), ArrayRef<Constant*>(arr1));
						data_dest_global->setInitializer(init_data_dest);	
						data_dest_global->setSection(".nv_vars");

						// declare size dirtylist
						StringRef* name2 = new StringRef("data_size");
						const Twine tname2 = llvm::Twine(*name2);
						GlobalVariable* data_size_global = new GlobalVariable(M, ArrayType::get(Type::getInt16Ty(F.getContext()), maxDirtyBufSize), false, GlobalValue::ExternalLinkage, 0, tname2, M.getNamedGlobal("data_base"));
						data_size_global->setAlignment(2);
						std::vector<Constant*> arr2;
						Constant* init_data_size = ConstantArray::get(ArrayType::get(Type::getInt16Ty(F.getContext()), maxDirtyBufSize), ArrayRef<Constant*>(arr2));
						data_size_global->setInitializer(init_data_size);	
						data_size_global->setSection(".nv_vars");

						// declare clear_isDirty()
						add_clear_isDirty(M);

						return true;
					}
					// is this is not set, time to declare functions!

					if(!isDeclared){
						// declare write_to_gbuf()
						Constant* c = F.getParent()->getOrInsertFunction("write_to_gbuf",llvm::Type::getVoidTy(F.getContext()),
								llvm::Type::getInt8PtrTy(F.getContext()),
								llvm::Type::getInt8PtrTy(F.getContext()),
								llvm::Type::getInt16Ty(F.getContext()), NULL);

						isDeclared = 1;
						wtg = cast<Function>(c);				

						// declare my_memset()
						Constant* c3 = F.getParent()->getOrInsertFunction("my_memset",llvm::Type::getVoidTy(F.getContext()),
								llvm::Type::getInt8PtrTy(F.getContext()),
								llvm::Type::getInt16Ty(F.getContext()),
								llvm::Type::getInt16Ty(F.getContext()),
								NULL);
						array_memset = cast<Function>(c3);				

						// declare num_dirty_gv (extern declaration, since it is defined in the library)
						StringRef* name2 = new StringRef("num_dirty_gv");
						const Twine tname = llvm::Twine(*name2);
						GlobalVariable* num_dirty_gv = new GlobalVariable(M, Type::getInt16Ty(F.getContext()), false, GlobalValue::CommonLinkage, 0, tname, M.getNamedGlobal("data_src_base"), GlobalValue::ThreadLocalMode::NotThreadLocal, 0, true);
						Constant* zeroVal = ConstantInt::get(Type::getInt16Ty(F.getContext()) , 0);
						num_dirty_gv->setInitializer(zeroVal);	

						// declare numBoots (extern declaration, since it is defined in the library)
						StringRef* name3 = new StringRef("_numBoots");
						const Twine tname2 = llvm::Twine(*name3);
						GlobalVariable* numBoots = new GlobalVariable(M, Type::getInt16Ty(F.getContext()), false, GlobalValue::CommonLinkage, 0, tname2, M.getNamedGlobal("data_src_base"), GlobalValue::ThreadLocalMode::NotThreadLocal, 0, true);
						numBoots->setInitializer(zeroVal);	
					}

					// init valPointer / newValPointer
					valPointer.clear();
					newValPointer.clear();
					// time to analyze task!
					if (isAnalyzingTask){
						bool done = 0;
						Value* local_b;
						isR.clear();
						readPerInst.clear();
						writePerInst.clear();

						// only for tasks, run multiple times (analyze -> transform)
						while(F.getName().str().find("task_") != std::string::npos){
							if (valPointer.size() != 0 && !done){ //when WAR. switch all _global_ read to local. insert privatization and pre-commit 
								isReplace = 1;
								bool first = 0;

								// declare _baks
								for (auto &B : F){
									for (auto &I : B){
										if (auto *op = dyn_cast<Instruction>(&I)){
											if (!first){ //at first instruction, insert privatization
												curOp = op;
												declare_dbuf(&B, F); //first only declare _baks if not declared
												first = 1;
											}
										}
									}
								}

								//errs() << "task: " << F.getName() << "\n";
								//errs() << "vars that needs to be replaced\n";

								for (std::map<std::string, Value*>::iterator vpit = valPointer.begin(), vpe = valPointer.end(); vpit != vpe; ++vpit) {
									//errs() << vpit->first << "\n";
								}
								checkRWInFunc(F); // replace _global_ to _baks
								first = 0;

								// pre-commit
								for (auto &B : F){
									for (auto &I : B){
										if (auto *op = dyn_cast<Instruction>(&I)){
											if (op->getOpcode() == CALL){
												for (Use &U : op->operands()) {
													Value *v = U.get();
													if (!strcmp(v->getName().str().c_str(), "transition_to")){
														pre_commit(op, F);
													}
												}
											}
										}
									}
								}

								// privatization
								for (auto &B : F){
									for (auto &I : B){
										if (auto *op = dyn_cast<Instruction>(&I)){
											if (!first){ //at first instruction, insert privatization
												privatize(op, &B); //here do the privatization.
												first = 1;
											}
										}
									}
								}
								done = 1;
							}
							else{ // Analyze here
								// do WAR analysis
								isReplace = 0;
								checkRWInFunc(F);
								searchWAR(F); // search for WAR based on readPerInst and writePerInst, and call markAsRW 
								maxDirtyBufSize = (dirtyBufSize > maxDirtyBufSize) ? dirtyBufSize : maxDirtyBufSize;
							}
							if (valPointer.size() == 0 || done) break; //if no WAR, no change
						}
					}
					else{ // non-tasks
						bool done = 0;

						// only analyze non-task functions (analyze -> transform)
						while((F.getName().str().find("task_") == std::string::npos) &&
              (F.getName().str().find("ISR") == std::string::npos)){
              if (valPointer.size() != 0 && !done){ //when RW. switch all global_ read to local. 
								isReplace = 1;
								bool first = 0;
								for (auto &B : F){
									for (auto &I : B){
										if (auto *op = dyn_cast<Instruction>(&I)){
											if (!first){ //at first instruction, insert privatization
												curOp = op;
												declare_dbuf(&B, F); //first only declare _baks if not declared
												first = 1;
											}
										}
									}
								}
								checkRWInFunc(F); //this is replace
								first = 0;
								done = 1;
							}
							else { // Analyze here
								//for non-functions, we do three things
								// 1. just switch every _global_ to its copy
								// 2. save every _global_ used to map. (It will be used by tasks later)
								// 3. save function call to other functions

								std::map<std::string, non_task*>::iterator it = non_tasks.find(F.getName().str().c_str());
								if(it == non_tasks.end()){ // if we fist met the function, add it to tree
									non_tasks[F.getName().str()] = new non_task();
								}

								for (auto &B : F){
									for (auto &I : B){
										if (auto *op = dyn_cast<Instruction>(&I)){
											for(int i = 0; i < op->getNumOperands(); ++i){
												// check if it contains _global_ var (This is simpler than analyzing task since we don't need WAR analysis.
												// So we just do it here instead of using checkRWInFunc()
												Value* val;; 
												if (isa<GEPOperator>(op->getOperand(i))){
													auto* op2 = dyn_cast<GEPOperator>(op->getOperand(i));
													val = op2->getPointerOperand();
												}
												else if (isa<BitCastOperator>(op->getOperand(i))){
													auto* op2 = dyn_cast<BitCastOperator>(op->getOperand(i));
													val = op2->getOperand(0);
												}	
												else{
													val = op->getOperand(i);
												}		
												std::string varName = val->getName().str();

												// if global is being used, write it down.
												if (varName.find("_global_") != std::string::npos){
													// first check if it is already written 
													if (std::find(non_tasks[F.getName().str()]->used_globalVars.begin(), non_tasks[F.getName().str()]->used_globalVars.end(), val) == non_tasks[F.getName().str()]->used_globalVars.end() ){
														non_tasks[F.getName().str()]->used_globalVars.push_back(val);
														valPointer[varName] = val;
														Value* vbak = M.getNamedValue(varName+"_bak");
														newValPointer[varName] = vbak;
													}	
												}

												// if it calls another function, write down the call structure
												if (isa<CallInst>(op)){
													auto* op2 = dyn_cast<CallInst>(op);
													Function* f = op2->getCalledFunction();
													if(f != NULL){
														if(f->getName().str().find("task_") == std::string::npos){ //this is not task.
															it = non_tasks.find(f->getName().str().c_str());
															if(it == non_tasks.end()){ //never declared before
																non_tasks[f->getName().str()] = new non_task();
																non_tasks[F.getName().str()]->callees.push_back(non_tasks[f->getName().str()]);
															}
															else{
																non_tasks[F.getName().str()]->callees.push_back(it->second);
															}
														}
													}
												}
											}
										}
									}
								}
							}
							if (valPointer.size() == 0 || done) break; //if not RW, no change
						}
					}
				}
				if(!isAnalyzingTask){
					isAnalyzingTask = true;
				}
				else if(!isSetDirtyBufSize) {
					isSetDirtyBufSize = true; //first analyze non-task functions, and then tasks
				}
				else{
					return true; // it should not reach here though.
				}
			}	
		}
		virtual void getAnalysisUsage(AnalysisUsage& AU) const {
			AU.setPreservesAll();
		}

	};
}

char AlpacaModulePass::ID = 0;

RegisterPass<AlpacaModulePass> X("alpaca", "Alpaca Pass");
