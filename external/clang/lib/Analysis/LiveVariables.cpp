#include "clang/Analysis/Analyses/LiveVariables.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/AnalysisContext.h"
#include "clang/AST/StmtVisitor.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/DenseMap.h"

#include <deque>
#include <algorithm>
#include <vector>

using namespace clang;

namespace {

// FIXME: This is copy-pasted from ThreadSafety.c.  I wanted a patch that
// contained working code before refactoring the implementation of both
// files.
class CFGBlockSet {
  llvm::BitVector VisitedBlockIDs;
  
public:
  // po_iterator requires this iterator, but the only interface needed is the
  // value_type typedef.
  struct iterator {
    typedef const CFGBlock *value_type;
  };
  
  CFGBlockSet() {}
  CFGBlockSet(const CFG *G) : VisitedBlockIDs(G->getNumBlockIDs(), false) {}
  
  /// \brief Set the bit associated with a particular CFGBlock.
  /// This is the important method for the SetType template parameter.
  bool insert(const CFGBlock *Block) {
    // Note that insert() is called by po_iterator, which doesn't check to make
    // sure that Block is non-null.  Moreover, the CFGBlock iterator will
    // occasionally hand out null pointers for pruned edges, so we catch those
    // here.
    if (Block == 0)
      return false;  // if an edge is trivially false.
    if (VisitedBlockIDs.test(Block->getBlockID()))
      return false;
    VisitedBlockIDs.set(Block->getBlockID());
    return true;
  }
  
  /// \brief Check if the bit for a CFGBlock has been already set.
  /// This method is for tracking visited blocks in the main threadsafety loop.
  /// Block must not be null.
  bool alreadySet(const CFGBlock *Block) {
    return VisitedBlockIDs.test(Block->getBlockID());
  }
};

/// \brief We create a helper class which we use to iterate through CFGBlocks in
/// the topological order.
class TopologicallySortedCFG {
  typedef llvm::po_iterator<const CFG*, CFGBlockSet, true>  po_iterator;
  
  std::vector<const CFGBlock*> Blocks;
  
  typedef llvm::DenseMap<const CFGBlock *, unsigned> BlockOrderTy;
  BlockOrderTy BlockOrder;
  
  
public:
  typedef std::vector<const CFGBlock*>::reverse_iterator iterator;
  
  TopologicallySortedCFG(const CFG *CFGraph) {
    Blocks.reserve(CFGraph->getNumBlockIDs());
    CFGBlockSet BSet(CFGraph);
    
    for (po_iterator I = po_iterator::begin(CFGraph, BSet),
         E = po_iterator::end(CFGraph, BSet); I != E; ++I) {
      BlockOrder[*I] = Blocks.size() + 1;
      Blocks.push_back(*I);      
    }
  }
  
  iterator begin() {
    return Blocks.rbegin();
  }
  
  iterator end() {
    return Blocks.rend();
  }
  
  bool empty() {
    return begin() == end();
  }
  
  struct BlockOrderCompare;
  friend struct BlockOrderCompare;

  struct BlockOrderCompare {
    const TopologicallySortedCFG &TSC;
  public:
    BlockOrderCompare(const TopologicallySortedCFG &tsc) : TSC(tsc) {}
    
    bool operator()(const CFGBlock *b1, const CFGBlock *b2) const {
      TopologicallySortedCFG::BlockOrderTy::const_iterator b1It = TSC.BlockOrder.find(b1);
      TopologicallySortedCFG::BlockOrderTy::const_iterator b2It = TSC.BlockOrder.find(b2);
      
      unsigned b1V = (b1It == TSC.BlockOrder.end()) ? 0 : b1It->second;
      unsigned b2V = (b2It == TSC.BlockOrder.end()) ? 0 : b2It->second;
      return b1V > b2V;
    }  
  };
  
  BlockOrderCompare getComparator() const {
    return BlockOrderCompare(*this);
  }
};

class DataflowWorklist {
  SmallVector<const CFGBlock *, 20> worklist;
  llvm::BitVector enqueuedBlocks;
  TopologicallySortedCFG TSC;
public:
  DataflowWorklist(const CFG &cfg)
    : enqueuedBlocks(cfg.getNumBlockIDs()),
      TSC(&cfg) {}
  
  void enqueueBlock(const CFGBlock *block);
  void enqueueSuccessors(const CFGBlock *block);
  void enqueuePredecessors(const CFGBlock *block);

  const CFGBlock *dequeue();

  void sortWorklist();
};

}

void DataflowWorklist::enqueueBlock(const clang::CFGBlock *block) {
  if (block && !enqueuedBlocks[block->getBlockID()]) {
    enqueuedBlocks[block->getBlockID()] = true;
    worklist.push_back(block);
  }
}
  
void DataflowWorklist::enqueueSuccessors(const clang::CFGBlock *block) {
  const unsigned OldWorklistSize = worklist.size();
  for (CFGBlock::const_succ_iterator I = block->succ_begin(),
       E = block->succ_end(); I != E; ++I) {
    enqueueBlock(*I);
  }

  if (OldWorklistSize == 0 || OldWorklistSize == worklist.size())
    return;

  sortWorklist();
}

void DataflowWorklist::enqueuePredecessors(const clang::CFGBlock *block) {
  const unsigned OldWorklistSize = worklist.size();
  for (CFGBlock::const_pred_iterator I = block->pred_begin(),
       E = block->pred_end(); I != E; ++I) {
    enqueueBlock(*I);
  }
  
  if (OldWorklistSize == 0 || OldWorklistSize == worklist.size())
    return;

  sortWorklist();
}

void DataflowWorklist::sortWorklist() {
  std::sort(worklist.begin(), worklist.end(), TSC.getComparator());
}


const CFGBlock *DataflowWorklist::dequeue() {
  if (worklist.empty())
    return 0;
  const CFGBlock *b = worklist.back();
  worklist.pop_back();
  enqueuedBlocks[b->getBlockID()] = false;
  return b;
}

namespace {
class LiveVariablesImpl {
public:  
  AnalysisContext &analysisContext;
  std::vector<LiveVariables::LivenessValues> cfgBlockValues;
  llvm::ImmutableSet<const Stmt *>::Factory SSetFact;
  llvm::ImmutableSet<const VarDecl *>::Factory DSetFact;
  llvm::DenseMap<const CFGBlock *, LiveVariables::LivenessValues> blocksEndToLiveness;
  llvm::DenseMap<const CFGBlock *, LiveVariables::LivenessValues> blocksBeginToLiveness;
  llvm::DenseMap<const Stmt *, LiveVariables::LivenessValues> stmtsToLiveness;
  llvm::DenseMap<const DeclRefExpr *, unsigned> inAssignment;
  const bool killAtAssign;
  
  LiveVariables::LivenessValues
  merge(LiveVariables::LivenessValues valsA,
        LiveVariables::LivenessValues valsB);
      
  LiveVariables::LivenessValues runOnBlock(const CFGBlock *block,
                                           LiveVariables::LivenessValues val,
                                           LiveVariables::Observer *obs = 0);

  void dumpBlockLiveness(const SourceManager& M);

  LiveVariablesImpl(AnalysisContext &ac, bool KillAtAssign)
    : analysisContext(ac),
      SSetFact(false), // Do not canonicalize ImmutableSets by default.
      DSetFact(false), // This is a *major* performance win.
      killAtAssign(KillAtAssign) {}
};
}

static LiveVariablesImpl &getImpl(void *x) {
  return *((LiveVariablesImpl *) x);
}

//===----------------------------------------------------------------------===//
// Operations and queries on LivenessValues.
//===----------------------------------------------------------------------===//

bool LiveVariables::LivenessValues::isLive(const Stmt *S) const {
  return liveStmts.contains(S);
}

bool LiveVariables::LivenessValues::isLive(const VarDecl *D) const {
  return liveDecls.contains(D);
}

namespace {
  template <typename SET>
  SET mergeSets(SET A, SET B) {
    if (A.isEmpty())
      return B;
    
    for (typename SET::iterator it = B.begin(), ei = B.end(); it != ei; ++it) {
      A = A.add(*it);
    }
    return A;
  }
}

LiveVariables::LivenessValues
LiveVariablesImpl::merge(LiveVariables::LivenessValues valsA,
                         LiveVariables::LivenessValues valsB) {  
  
  llvm::ImmutableSetRef<const Stmt *>
    SSetRefA(valsA.liveStmts.getRootWithoutRetain(), SSetFact.getTreeFactory()),
    SSetRefB(valsB.liveStmts.getRootWithoutRetain(), SSetFact.getTreeFactory());
                                                
  
  llvm::ImmutableSetRef<const VarDecl *>
    DSetRefA(valsA.liveDecls.getRootWithoutRetain(), DSetFact.getTreeFactory()),
    DSetRefB(valsB.liveDecls.getRootWithoutRetain(), DSetFact.getTreeFactory());
  

  SSetRefA = mergeSets(SSetRefA, SSetRefB);
  DSetRefA = mergeSets(DSetRefA, DSetRefB);
  
  // asImmutableSet() canonicalizes the tree, allowing us to do an easy
  // comparison afterwards.
  return LiveVariables::LivenessValues(SSetRefA.asImmutableSet(),
                                       DSetRefA.asImmutableSet());  
}

bool LiveVariables::LivenessValues::equals(const LivenessValues &V) const {
  return liveStmts == V.liveStmts && liveDecls == V.liveDecls;
}

//===----------------------------------------------------------------------===//
// Query methods.
//===----------------------------------------------------------------------===//

static bool isAlwaysAlive(const VarDecl *D) {
  return D->hasGlobalStorage();
}

bool LiveVariables::isLive(const CFGBlock *B, const VarDecl *D) {
  return isAlwaysAlive(D) || getImpl(impl).blocksEndToLiveness[B].isLive(D);
}

bool LiveVariables::isLive(const Stmt *S, const VarDecl *D) {
  return isAlwaysAlive(D) || getImpl(impl).stmtsToLiveness[S].isLive(D);
}

bool LiveVariables::isLive(const Stmt *Loc, const Stmt *S) {
  return getImpl(impl).stmtsToLiveness[Loc].isLive(S);
}

//===----------------------------------------------------------------------===//
// Dataflow computation.
//===----------------------------------------------------------------------===//

namespace {
class TransferFunctions : public StmtVisitor<TransferFunctions> {
  LiveVariablesImpl &LV;
  LiveVariables::LivenessValues &val;
  LiveVariables::Observer *observer;
  const CFGBlock *currentBlock;
public:
  TransferFunctions(LiveVariablesImpl &im,
                    LiveVariables::LivenessValues &Val,
                    LiveVariables::Observer *Observer,
                    const CFGBlock *CurrentBlock)
  : LV(im), val(Val), observer(Observer), currentBlock(CurrentBlock) {}

  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitBlockExpr(BlockExpr *BE);
  void VisitDeclRefExpr(DeclRefExpr *DR);  
  void VisitDeclStmt(DeclStmt *DS);
  void VisitObjCForCollectionStmt(ObjCForCollectionStmt *OS);
  void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE);
  void VisitUnaryOperator(UnaryOperator *UO);
  void Visit(Stmt *S);
};
}

static const VariableArrayType *FindVA(QualType Ty) {
  const Type *ty = Ty.getTypePtr();
  while (const ArrayType *VT = dyn_cast<ArrayType>(ty)) {
    if (const VariableArrayType *VAT = dyn_cast<VariableArrayType>(VT))
      if (VAT->getSizeExpr())
        return VAT;
    
    ty = VT->getElementType().getTypePtr();
  }
  
  return 0;
}

void TransferFunctions::Visit(Stmt *S) {
  if (observer)
    observer->observeStmt(S, currentBlock, val);
  
  StmtVisitor<TransferFunctions>::Visit(S);
  
  if (isa<Expr>(S)) {
    val.liveStmts = LV.SSetFact.remove(val.liveStmts, S);
  }

  // Mark all children expressions live.
  
  switch (S->getStmtClass()) {
    default:
      break;
    case Stmt::StmtExprClass: {
      // For statement expressions, look through the compound statement.
      S = cast<StmtExpr>(S)->getSubStmt();
      break;
    }
    case Stmt::CXXMemberCallExprClass: {
      // Include the implicit "this" pointer as being live.
      CXXMemberCallExpr *CE = cast<CXXMemberCallExpr>(S);
      if (Expr *ImplicitObj = CE->getImplicitObjectArgument()) {
        ImplicitObj = ImplicitObj->IgnoreParens();        
        val.liveStmts = LV.SSetFact.add(val.liveStmts, ImplicitObj);
      }
      break;
    }
    case Stmt::DeclStmtClass: {
      const DeclStmt *DS = cast<DeclStmt>(S);
      if (const VarDecl *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
        for (const VariableArrayType* VA = FindVA(VD->getType());
             VA != 0; VA = FindVA(VA->getElementType())) {
          val.liveStmts = LV.SSetFact.add(val.liveStmts,
                                          VA->getSizeExpr()->IgnoreParens());
        }
      }
      break;
    }
    // FIXME: These cases eventually shouldn't be needed.
    case Stmt::ExprWithCleanupsClass: {
      S = cast<ExprWithCleanups>(S)->getSubExpr();
      break;
    }
    case Stmt::CXXBindTemporaryExprClass: {
      S = cast<CXXBindTemporaryExpr>(S)->getSubExpr();
      break;
    }
    case Stmt::UnaryExprOrTypeTraitExprClass: {
      // No need to unconditionally visit subexpressions.
      return;
    }
  }
  
  for (Stmt::child_iterator it = S->child_begin(), ei = S->child_end();
       it != ei; ++it) {
    if (Stmt *child = *it) {
      if (Expr *Ex = dyn_cast<Expr>(child))
        child = Ex->IgnoreParens();
               
      val.liveStmts = LV.SSetFact.add(val.liveStmts, child);
    }
  }
}

void TransferFunctions::VisitBinaryOperator(BinaryOperator *B) {
  if (B->isAssignmentOp()) {
    if (!LV.killAtAssign)
      return;
    
    // Assigning to a variable?
    Expr *LHS = B->getLHS()->IgnoreParens();
    
    if (DeclRefExpr *DR = dyn_cast<DeclRefExpr>(LHS))
      if (const VarDecl *VD = dyn_cast<VarDecl>(DR->getDecl())) {
        // Assignments to references don't kill the ref's address
        if (VD->getType()->isReferenceType())
          return;

        if (!isAlwaysAlive(VD)) {
          // The variable is now dead.
          val.liveDecls = LV.DSetFact.remove(val.liveDecls, VD);
        }

        if (observer)
          observer->observerKill(DR);
      }
  }
}

void TransferFunctions::VisitBlockExpr(BlockExpr *BE) {
  AnalysisContext::referenced_decls_iterator I, E;
  llvm::tie(I, E) =
    LV.analysisContext.getReferencedBlockVars(BE->getBlockDecl());
  for ( ; I != E ; ++I) {
    const VarDecl *VD = *I;
    if (isAlwaysAlive(VD))
      continue;
    val.liveDecls = LV.DSetFact.add(val.liveDecls, VD);
  }
}

void TransferFunctions::VisitDeclRefExpr(DeclRefExpr *DR) {
  if (const VarDecl *D = dyn_cast<VarDecl>(DR->getDecl()))
    if (!isAlwaysAlive(D) && LV.inAssignment.find(DR) == LV.inAssignment.end())
      val.liveDecls = LV.DSetFact.add(val.liveDecls, D);
}

void TransferFunctions::VisitDeclStmt(DeclStmt *DS) {
  for (DeclStmt::decl_iterator DI=DS->decl_begin(), DE = DS->decl_end();
       DI != DE; ++DI)
    if (VarDecl *VD = dyn_cast<VarDecl>(*DI)) {
      if (!isAlwaysAlive(VD))
        val.liveDecls = LV.DSetFact.remove(val.liveDecls, VD);
    }
}

void TransferFunctions::VisitObjCForCollectionStmt(ObjCForCollectionStmt *OS) {
  // Kill the iteration variable.
  DeclRefExpr *DR = 0;
  const VarDecl *VD = 0;

  Stmt *element = OS->getElement();
  if (DeclStmt *DS = dyn_cast<DeclStmt>(element)) {
    VD = cast<VarDecl>(DS->getSingleDecl());
  }
  else if ((DR = dyn_cast<DeclRefExpr>(cast<Expr>(element)->IgnoreParens()))) {
    VD = cast<VarDecl>(DR->getDecl());
  }
  
  if (VD) {
    val.liveDecls = LV.DSetFact.remove(val.liveDecls, VD);
    if (observer && DR)
      observer->observerKill(DR);
  }
}

void TransferFunctions::
VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE)
{
  // While sizeof(var) doesn't technically extend the liveness of 'var', it
  // does extent the liveness of metadata if 'var' is a VariableArrayType.
  // We handle that special case here.
  if (UE->getKind() != UETT_SizeOf || UE->isArgumentType())
    return;

  const Expr *subEx = UE->getArgumentExpr();
  if (subEx->getType()->isVariableArrayType()) {
    assert(subEx->isLValue());
    val.liveStmts = LV.SSetFact.add(val.liveStmts, subEx->IgnoreParens());
  }
}

void TransferFunctions::VisitUnaryOperator(UnaryOperator *UO) {
  // Treat ++/-- as a kill.
  // Note we don't actually have to do anything if we don't have an observer,
  // since a ++/-- acts as both a kill and a "use".
  if (!observer)
    return;
  
  switch (UO->getOpcode()) {
  default:
    return;
  case UO_PostInc:
  case UO_PostDec:    
  case UO_PreInc:
  case UO_PreDec:
    break;
  }
  
  if (DeclRefExpr *DR = dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParens()))
    if (isa<VarDecl>(DR->getDecl())) {
      // Treat ++/-- as a kill.
      observer->observerKill(DR);
    }
}

LiveVariables::LivenessValues
LiveVariablesImpl::runOnBlock(const CFGBlock *block,
                              LiveVariables::LivenessValues val,
                              LiveVariables::Observer *obs) {

  TransferFunctions TF(*this, val, obs, block);
  
  // Visit the terminator (if any).
  if (const Stmt *term = block->getTerminator())
    TF.Visit(const_cast<Stmt*>(term));
  
  // Apply the transfer function for all Stmts in the block.
  for (CFGBlock::const_reverse_iterator it = block->rbegin(),
       ei = block->rend(); it != ei; ++it) {
    const CFGElement &elem = *it;
    if (!isa<CFGStmt>(elem))
      continue;
    
    const Stmt *S = cast<CFGStmt>(elem).getStmt();
    TF.Visit(const_cast<Stmt*>(S));
    stmtsToLiveness[S] = val;
  }
  return val;
}

void LiveVariables::runOnAllBlocks(LiveVariables::Observer &obs) {
  const CFG *cfg = getImpl(impl).analysisContext.getCFG();
  for (CFG::const_iterator it = cfg->begin(), ei = cfg->end(); it != ei; ++it)
    getImpl(impl).runOnBlock(*it, getImpl(impl).blocksEndToLiveness[*it], &obs);    
}

LiveVariables::LiveVariables(void *im) : impl(im) {} 

LiveVariables::~LiveVariables() {
  delete (LiveVariablesImpl*) impl;
}

LiveVariables *
LiveVariables::computeLiveness(AnalysisContext &AC,
                                 bool killAtAssign) {

  // No CFG?  Bail out.
  CFG *cfg = AC.getCFG();
  if (!cfg)
    return 0;

  LiveVariablesImpl *LV = new LiveVariablesImpl(AC, killAtAssign);

  // Construct the dataflow worklist.  Enqueue the exit block as the
  // start of the analysis.
  DataflowWorklist worklist(*cfg);
  llvm::BitVector everAnalyzedBlock(cfg->getNumBlockIDs());

  // FIXME: we should enqueue using post order.
  for (CFG::const_iterator it = cfg->begin(), ei = cfg->end(); it != ei; ++it) {
    const CFGBlock *block = *it;
    worklist.enqueueBlock(block);
    
    // FIXME: Scan for DeclRefExprs using in the LHS of an assignment.
    // We need to do this because we lack context in the reverse analysis
    // to determine if a DeclRefExpr appears in such a context, and thus
    // doesn't constitute a "use".
    if (killAtAssign)
      for (CFGBlock::const_iterator bi = block->begin(), be = block->end();
           bi != be; ++bi) {
        if (const CFGStmt *cs = bi->getAs<CFGStmt>()) {
          if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(cs->getStmt())) {
            if (BO->getOpcode() == BO_Assign) {
              if (const DeclRefExpr *DR =
                    dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParens())) {
                LV->inAssignment[DR] = 1;
              }
            }
          }
        }
      }
  }
  
  worklist.sortWorklist();
  
  while (const CFGBlock *block = worklist.dequeue()) {
    // Determine if the block's end value has changed.  If not, we
    // have nothing left to do for this block.
    LivenessValues &prevVal = LV->blocksEndToLiveness[block];
    
    // Merge the values of all successor blocks.
    LivenessValues val;
    for (CFGBlock::const_succ_iterator it = block->succ_begin(),
                                       ei = block->succ_end(); it != ei; ++it) {
      if (const CFGBlock *succ = *it) {     
        val = LV->merge(val, LV->blocksBeginToLiveness[succ]);
      }
    }
    
    if (!everAnalyzedBlock[block->getBlockID()])
      everAnalyzedBlock[block->getBlockID()] = true;
    else if (prevVal.equals(val))
      continue;

    prevVal = val;
    
    // Update the dataflow value for the start of this block.
    LV->blocksBeginToLiveness[block] = LV->runOnBlock(block, val);
    
    // Enqueue the value to the predecessors.
    worklist.enqueuePredecessors(block);
  }
  
  return new LiveVariables(LV);
}

static bool compare_entries(const CFGBlock *A, const CFGBlock *B) {
  return A->getBlockID() < B->getBlockID();
}

static bool compare_vd_entries(const Decl *A, const Decl *B) {
  SourceLocation ALoc = A->getLocStart();
  SourceLocation BLoc = B->getLocStart();
  return ALoc.getRawEncoding() < BLoc.getRawEncoding();
}

void LiveVariables::dumpBlockLiveness(const SourceManager &M) {
  getImpl(impl).dumpBlockLiveness(M);
}

void LiveVariablesImpl::dumpBlockLiveness(const SourceManager &M) {
  std::vector<const CFGBlock *> vec;
  for (llvm::DenseMap<const CFGBlock *, LiveVariables::LivenessValues>::iterator
       it = blocksEndToLiveness.begin(), ei = blocksEndToLiveness.end();
       it != ei; ++it) {
    vec.push_back(it->first);    
  }
  std::sort(vec.begin(), vec.end(), compare_entries);

  std::vector<const VarDecl*> declVec;

  for (std::vector<const CFGBlock *>::iterator
        it = vec.begin(), ei = vec.end(); it != ei; ++it) {
    llvm::errs() << "\n[ B" << (*it)->getBlockID()
                 << " (live variables at block exit) ]\n";
    
    LiveVariables::LivenessValues vals = blocksEndToLiveness[*it];
    declVec.clear();
    
    for (llvm::ImmutableSet<const VarDecl *>::iterator si =
          vals.liveDecls.begin(),
          se = vals.liveDecls.end(); si != se; ++si) {
      declVec.push_back(*si);      
    }
    
    std::sort(declVec.begin(), declVec.end(), compare_vd_entries);
    
    for (std::vector<const VarDecl*>::iterator di = declVec.begin(),
         de = declVec.end(); di != de; ++di) {
      llvm::errs() << " " << (*di)->getDeclName().getAsString()
                   << " <";
      (*di)->getLocation().dump(M);
      llvm::errs() << ">\n";
    }
  }
  llvm::errs() << "\n";  
}

const void *LiveVariables::getTag() { static int x; return &x; }
const void *RelaxedLiveVariables::getTag() { static int x; return &x; }