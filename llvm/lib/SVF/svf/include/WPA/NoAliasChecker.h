#ifndef NOALIASCHECKER_H_
#define NOALIASCHECKER_H_

#include "MemoryModel/PointerAnalysis.h"

namespace SVF
{

/*!
 * NoAliasChecker: Abstract Interface for noalias checks.
 * Implementation will be in SVF-LLVM to access LLVM attributes.
 */
class NoAliasChecker
{
public:
    virtual ~NoAliasChecker() = default;

    /// Check if two nodes are no-alias.
    virtual AliasResult check(NodeID id1, NodeID id2) = 0;
};

} // namespace SVF

#endif /* NOALIASCHECKER_H_ */
