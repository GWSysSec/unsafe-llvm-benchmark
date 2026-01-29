#include "SVF-LLVM/LLVMNoAliasChecker.h"
#include "SVF-LLVM/LLVMModule.h"

using namespace SVF;
using namespace SVF::LLVMUtil;

LLVMModuleSet* llvmModuleSet()
{
    return LLVMModuleSet::getLLVMModuleSet();
}

AliasResult LLVMNoAliasChecker::check(NodeID id1, NodeID id2)
{
    const Value* base1 = getNoAliasBase(id1);
    const Value* base2 = getNoAliasBase(id2);

    if (base1 && base2 && base1 != base2)
    {
        if (const Argument* arg1 = SVFUtil::dyn_cast<Argument>(base1))
        {
            if (const Argument* arg2 = SVFUtil::dyn_cast<Argument>(base2))
            {
                if (arg1->getParent() == arg2->getParent())
                {
                    if (LLVMUtil::isNoAlias(arg1) && LLVMUtil::isNoAlias(arg2))
                    {
                        return AliasResult::NoAlias;
                    }
                }
            }
        }
    }
    return AliasResult::MayAlias;
}

const Value* LLVMNoAliasChecker::getNoAliasBase(NodeID id)
{
    SVFIR* pag = pta->getPAG();
    if (!pag->hasGNode(id)) return nullptr;
    
    PAGNode* node = pag->getGNode(id);
    if (!llvmModuleSet()->hasLLVMValue(node)) return nullptr;
    const Value* val = llvmModuleSet()->getLLVMValue(node);
    
    if (!val) return nullptr;

    const Value* base = val->stripPointerCasts();
    
    // Handle GEPs
    while (const GetElementPtrInst* gep = SVFUtil::dyn_cast<GetElementPtrInst>(base))
    {
        base = gep->getPointerOperand()->stripPointerCasts();
    }

    if (const Argument* arg = SVFUtil::dyn_cast<Argument>(base))
    {
        return arg;
    }

    return nullptr;
}
