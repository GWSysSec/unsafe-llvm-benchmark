#ifndef LLVMNOALIASCHECKER_H_
#define LLVMNOALIASCHECKER_H_

#include "WPA/NoAliasChecker.h"
#include "WPA/Andersen.h"
#include "SVF-LLVM/LLVMUtil.h"

namespace SVF
{

class LLVMNoAliasChecker : public NoAliasChecker
{
public:
    LLVMNoAliasChecker(PointerAnalysis* p) : pta(p) {}
    ~LLVMNoAliasChecker() override = default;

    AliasResult check(NodeID id1, NodeID id2) override;

private:
    PointerAnalysis* pta;
    const Value* getNoAliasBase(NodeID id);
};

} // namespace SVF

#endif /* LLVMNOALIASCHECKER_H_ */
