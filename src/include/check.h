#ifndef __CHECK__
#define __CHECK__

#include "AlpacaPass.h"
extern uint64_t getSize(Value* val);
extern bool isMemcpy(Value* val);
extern llvm::Type::TypeID getBaseTypeID(Value* val);
extern int getPointerLayer(Value* val);
extern Value* isGlobal(Value* val, int pointerLayer); //return varName

#endif
