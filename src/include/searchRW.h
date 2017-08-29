#ifndef __SEARCH__
#define __SEARCH__

#include "AlpacaPass.h"
#include "check.h"
#include "replace.h"

extern void markAsRW(Value* val);
extern void search_funcCall(non_task* nt); 
extern void digDownRead(Value* val, int pointerLayer);
extern void digDownWrite(Value* val, int pointerLayer);
extern void digDownDeeper(Value* val, int pointerLayer, bool isRead);
extern void digDownRead(Value* val, int pointerLayer);
extern void digDownWrite(Value* val, int pointerLayer);
extern void checkRW(Instruction* op);
extern void isGlobalWrite(Value* val);
extern void isGlobalRead(Value* val);
extern void checkRWInFunc(Function& F);
extern void searchWAR(Function& F);

#endif
