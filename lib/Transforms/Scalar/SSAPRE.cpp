//===---- SSAPRELegacy.cpp - SSA PARTIAL REDUNDANCY ELIMINATION--------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/SSAPRE.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BreakCriticalEdges.h"
#include "llvm/Analysis/IteratedDominanceFrontier.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/CFG.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::ssapre;

#define DEBUG_TYPE "ssapre"

STATISTIC(SSAPREInstrSaved,    "Number of instructions saved");
STATISTIC(SSAPREInstrReloaded, "Number of instructions reloaded");
STATISTIC(SSAPREInstrInserted, "Number of instructions inserted");
STATISTIC(SSAPREInstrDeleted,  "Number of instructions deleted");
STATISTIC(SSAPREBlocksAdded,   "Number of blocks deleted");

// Anchor methods.
namespace llvm {
namespace ssapre {
Expression::~Expression() = default;
IgnoredExpression::~IgnoredExpression() = default;
UnknownExpression::~UnknownExpression() = default;
BasicExpression::~BasicExpression() = default;
PHIExpression::~PHIExpression() = default;
FactorExpression::~FactorExpression() = default;
unsigned Expression::LastID = 0;
}
}

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

// This is used as ⊥ version
Expression BExpr(ET_Buttom, ~2U, false);

std::pair<unsigned, unsigned> SSAPRE::
AssignDFSNumbers(BasicBlock *B, unsigned Start,
                 InstrToOrderType *M, OrderedInstrType *V) {
  unsigned End = Start;
  // if (MemoryAccess *MemPhi = MSSA->getMemoryAccess(B)) {
  //   InstrDFS[MemPhi] = End++;
  //   DFSToInstr.emplace_back(MemPhi);
  // }

  for (auto &I : *B) {
    if (M) (*M)[&I] = End++;
    if (V) V->emplace_back(&I);
  }

  // All of the range functions taken half-open ranges (open on the end side).
  // So we do not subtract one from count, because at this point it is one
  // greater than the last instruction.
  return std::make_pair(Start, End);
}

unsigned int SSAPRE::
GetRank(const Value *V) const {
  // Prefer undef to anything else
  if (isa<UndefValue>(V))
    return 0;
  if (isa<Constant>(V))
    return 1;
  else if (auto *A = dyn_cast<Argument>(V))
    return 2 + A->getArgNo();

  // Need to shift the instruction DFS by number of arguments + 3 to account for
  // the constant and argument ranking above.
  unsigned Result = InstrDFS.lookup(V);
  if (Result > 0)
    return 3 + NumFuncArgs + Result;
  // Unreachable or something else, just return a really large number.
  return ~0;
}

bool SSAPRE::
ShouldSwapOperands(const Value *A, const Value *B) const {
  // Because we only care about a total ordering, and don't rewrite expressions
  // in this order, we order by rank, which will give a strict weak ordering to
  // everything but constants, and then we order by pointer address.
  return std::make_pair(GetRank(A), A) > std::make_pair(GetRank(B), B);
}

bool SSAPRE::
FillInBasicExpressionInfo(Instruction &I, BasicExpression *E) {
  bool AllConstant = true;
  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    E->setType(GEP->getSourceElementType());
  } else {
    E->setType(I.getType());
  }

  E->setOpcode(I.getOpcode());

  for (auto &O : I.operands()) {
    AllConstant &= isa<Constant>(O);
    E->addOperand(O);
  }

  return AllConstant;
}

Expression *SSAPRE::
CheckSimplificationResults(Expression *E, Instruction &I, Value *V) {
  if (!V)
    return nullptr;

  if (auto *C = dyn_cast<Constant>(V)) {
    DEBUG(dbgs() << "Simplified " << I << " to "
                 << " constant " << *C << "\n");
    assert(isa<BasicExpression>(E) &&
           "We should always have had a basic expression here");

    // cast<BasicExpression>(E)->deallocateOperands(ArgRecycler);
    // ExpressionAllocator.Deallocate(E);
    return CreateIgnoredExpression(I);
  } else if (isa<Argument>(V) || isa<GlobalVariable>(V)) {
    DEBUG(dbgs() << "Simplified " << I << " to "
                 << " variable " << *V << "\n");
    // cast<BasicExpression>(E)->deallocateOperands(ArgRecycler);
    // ExpressionAllocator.Deallocate(E);
    return CreateIgnoredExpression(I);
  }

  return nullptr;
}

Expression * SSAPRE::
CreateIgnoredExpression(Instruction &I) {
  // auto *E = new (ExpressionAllocator) UnknownExpression(I);
  auto *E = new IgnoredExpression(&I);
  E->setOpcode(I.getOpcode());
  return E;
}

Expression * SSAPRE::
CreateUnknownExpression(Instruction &I) {
  // auto *E = new (ExpressionAllocator) UnknownExpression(I);
  auto *E = new UnknownExpression(&I);
  E->setOpcode(I.getOpcode());
  return E;
}


Expression * SSAPRE::
CreateBasicExpression(Instruction &I) {
  // auto *E = new (ExpressionAllocator) BasicExpression(I->getNumOperands());
  auto *E = new BasicExpression();

  bool AllConstant = FillInBasicExpressionInfo(I, E);

  if (I.isCommutative()) {
    // Ensure that commutative instructions that only differ by a permutation
    // of their operands get the same expression map by sorting the operand value
    // numbers.  Since all commutative instructions have two operands it is more
    // efficient to sort by hand rather than using, say, std::sort.
    assert(I.getNumOperands() == 2 && "Unsupported commutative instruction!");
    if (ShouldSwapOperands(E->getOperand(0), E->getOperand(1)))
      E->swapOperands(0, 1);
  }

  // Perform simplificaiton
  // We do not actually require simpler instructions but rather require them be
  // in a canonical form. Mainly we are interested in instructions that we
  // ignore, such as constants and variables.
  // TODO: Right now we only check to see if we get a constant result.
  // We may get a less than constant, but still better, result for
  // some operations.
  // IE
  //  add 0, x -> x
  //  and x, x -> x
  // We should handle this by simply rewriting the expression.
  if (auto *CI = dyn_cast<CmpInst>(&I)) {
    // Sort the operand value numbers so x<y and y>x get the same value
    // number.
    CmpInst::Predicate Predicate = CI->getPredicate();
    if (ShouldSwapOperands(E->getOperand(0), E->getOperand(1))) {
      E->swapOperands(0, 1);
      Predicate = CmpInst::getSwappedPredicate(Predicate);
    }
    E->setOpcode((CI->getOpcode() << 8) | Predicate);
    // TODO: 25% of our time is spent in SimplifyCmpInst with pointer operands
    assert(I.getOperand(0)->getType() == I.getOperand(1)->getType() &&
           "Wrong types on cmp instruction");
    assert((E->getOperand(0)->getType() == I.getOperand(0)->getType() &&
            E->getOperand(1)->getType() == I.getOperand(1)->getType()));
    Value *V = SimplifyCmpInst(Predicate, E->getOperand(0), E->getOperand(1),
                               *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (isa<SelectInst>(I)) {
    if (isa<Constant>(E->getOperand(0)) ||
        E->getOperand(0) == E->getOperand(1)) {
      assert(E->getOperand(1)->getType() == I.getOperand(1)->getType() &&
             E->getOperand(2)->getType() == I.getOperand(2)->getType());
      Value *V = SimplifySelectInst(E->getOperand(0), E->getOperand(1),
                                    E->getOperand(2), *DL, TLI, DT, AC);
      if (auto *SE = CheckSimplificationResults(E, I, V))
        return SE;
    }
  } else if (I.isBinaryOp()) {
    Value *V = SimplifyBinOp(E->getOpcode(), E->getOperand(0), E->getOperand(1),
                             *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (auto *BI = dyn_cast<BitCastInst>(&I)) {
    Value *V = SimplifyInstruction(BI, *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (isa<GetElementPtrInst>(I)) {
    Value *V = SimplifyGEPInst(E->getType(), E->getOperands(), *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (AllConstant) {
    // We don't bother trying to simplify unless all of the operands
    // were constant.
    // TODO: There are a lot of Simplify*'s we could call here, if we
    // wanted to.  The original motivating case for this code was a
    // zext i1 false to i8, which we don't have an interface to
    // simplify (IE there is no SimplifyZExt).

    SmallVector<Constant *, 8> C;
    for (Value *Arg : E->getOperands())
      C.emplace_back(cast<Constant>(Arg));

    if (Value *V = ConstantFoldInstOperands(&I, C, *DL, TLI))
      if (auto *SE = CheckSimplificationResults(E, I, V))
        return SE;
  }

  return E;
}

Expression *SSAPRE::
CreatePHIExpression(Instruction &I) {
  auto *E = new PHIExpression();
  FillInBasicExpressionInfo(I, E);
  // Very simple method, we do not try check for undef etc
  return E;
}

FactorExpression *SSAPRE::
CreateFactorExpression(const Expression &E, const BasicBlock &B) {
  return new FactorExpression(E, B, {pred_begin(&B), pred_end(&B)});
}

Expression *
SSAPRE::CreateExpression(Instruction &I) {
  Expression * E = nullptr;
  switch (I.getOpcode()) {
  case Instruction::ExtractValue:
  case Instruction::InsertValue:
    // E = performSymbolicAggrValueEvaluation(I);
    break;
  case Instruction::PHI:
    E = CreatePHIExpression(I);
    break;
  case Instruction::Call:
    // E = performSymbolicCallEvaluation(I);
    break;
  case Instruction::Store:
    // E = performSymbolicStoreEvaluation(I);
    break;
  case Instruction::Load:
    // E = performSymbolicLoadEvaluation(I);
    break;
  case Instruction::BitCast: {
    E = CreateBasicExpression(I);
  } break;
  case Instruction::ICmp:
  case Instruction::FCmp: {
    // E = performSymbolicCmpEvaluation(I);
  } break;
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::Select:
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
  case Instruction::GetElementPtr:
    E = CreateBasicExpression(I);
    break;
  default:
    E = CreateUnknownExpression(I);
  }

  if (!E)
    E = CreateUnknownExpression(I);

  return E;
}

void SSAPRE::
PrintDebug(std::string Caption) {
  dbgs() << "\n" << Caption << ":";
  dbgs() << "--------------------------------------";
  dbgs() << "\nExpressionsToInts\n";
  for (auto &P : PExprToInsts) {
    dbgs() << "(" << P.getSecond().size() << ") ";
    P.getFirst()->printInternal(dbgs());
    if (!P.getFirst()->getSave())
      continue;
    dbgs() << ":";
    for (const auto &I : P.getSecond()) {
      auto &VE = InstToVExpr[I];
      if (!VE->getSave())
        dbgs() << "\n(deleted)";
      else
        dbgs() << "\n" << *I;
    }
    dbgs() << "\n";
  }

  dbgs() << "\nORDERS DFS/SDFS";
  for (auto &V : DFSToInstr) {
    auto I = dyn_cast<Instruction>(V);
    dbgs() << "\n" << InstrDFS[I];
    dbgs() << "\t" << InstrSDFS[I];
    if (KillList.count((Instruction *)I))
      dbgs() << "\t(deleted)";
    else
      dbgs() << "\t" << *I;
  }

  dbgs() << "\nBlockToFactors\n";
  for (auto &P : BlockToFactors) {
    dbgs() << "(" << P.getSecond().size() << ") ";
    P.getFirst()->printAsOperand(dbgs(), false);
    dbgs() << ":";
    for (const auto &F : P.getSecond()) {
      dbgs() << "\n";
      F->printInternal(dbgs());
    }
    dbgs() << "\n";
  }

  dbgs() << "\nBlockToInserts\n";
    for (auto &P : BlockToInserts) {
      dbgs() << "(" << P.getSecond().size() << ") ";
      P.getFirst()->printAsOperand(dbgs(), false);
      dbgs() << ":";
      for (const auto &F : P.getSecond()) {
        dbgs() << "\n";
        F->printInternal(dbgs());
      }
      dbgs() << "\n";
    }

  dbgs() << "---------------------------------------------\n";
}

void SSAPRE::
ResetDownSafety(FactorExpression &FE, unsigned ON) {
  auto E = FE.getVExpr(ON);
  if (FE.getHasRealUse(ON) || !FactorExpression::classof(E)) {
    return;
  }

  auto *F = dyn_cast<FactorExpression>(E);
  if (!F->getDownSafe())
    return;

  F->setDownSafe(false);
  for (size_t i = 0, l = F->getVExprNum(); i < l; ++i) {
    ResetDownSafety(*F, i);
  }
}

void SSAPRE::
DownSafety() {
  for (auto F : FExprs) {
    if (F->getDownSafe()) continue;
    for (size_t i = 0, l = F->getVExprNum(); i < l; ++i) {
      ResetDownSafety(*F, i);
    }
  }
}

void SSAPRE::
ComputeCanBeAvail() {
  for (auto F : FExprs) {
    if (!F->getDownSafe()
      && F->getCanBeAvail()
      && F->getVExprIndex(&BExpr) != -1UL) {
      ResetCanBeAvail(*F);
    }
  }
}

void SSAPRE::
ResetCanBeAvail(FactorExpression &G) {
  G.setCanBeAvail(false);
  for (auto F : FExprs) {
    auto I = F->getVExprIndex(&G);
    if (I == -1UL) continue;
    if (!F->getHasRealUse(I)) {
      F->setVExpr(I, &BExpr);
      if (!F->getDownSafe() && F->getCanBeAvail()) {
        ResetCanBeAvail(*F);
      }
    }
  }
}

void SSAPRE::
ComputeLater() {
  for (auto F : FExprs) {
    F->setLater(F->getCanBeAvail());
  }
  for (auto F : FExprs) {
    if (F->getLater()) {
      for (size_t i = 0, l = F->getVExprs().size(); i < l; ++i) {
        if (F->getHasRealUse(i) && F->getVExpr(i) != &BExpr) {
          ResetLater(*F);
          break;
        }
      }
    }
  }
}

void SSAPRE::
ResetLater(FactorExpression &G) {
  G.setLater(false);
  for (auto F : FExprs) {
    auto I = F->getVExprIndex(&G);
    if (I == -1UL) continue;
    if (F->getLater()) ResetLater(*F);
  }
}

void SSAPRE::
WillBeAvail() {
  ComputeCanBeAvail();
  ComputeLater();
}

void SSAPRE::
FinalizeVisit(BasicBlock &B) {
  for (auto F : BlockToFactors[&B]) {
    F->setSave(false);
    F->setReload(false);
    auto V = F->getVersion();
    if (F->getWillBeAvail()) {
      auto &PE = F->getPExpr();
      if (!AvailDef.count(&PE)) {
          AvailDef.insert({&PE, DenseMap<int,Expression*>()});
          AvailDef[&PE][V] = F;
      } else if (!AvailDef[&PE].count(V)) {
          AvailDef[&PE].insert({V, F});
      } else {
        AvailDef[&PE][V] = F;
      }
    }
  }

  for (auto &I : B) {
    auto &VE = InstToVExpr[&I];
    auto &PE = VExprToPExpr[VE];
    if (I.isTerminator() ||
        IgnoredExpression::classof(VE) ||
        UnknownExpression::classof(VE))
      continue;

    VE->setSave(false);
    VE->setReload(false);
    auto V = VE->getVersion();

    if (AvailDef.count(PE)) {
      auto ADE = AvailDef[PE];
      // FIXME Check whether dominance is not strict
      if (!ADE.count(V) || DT->dominates(VExprToInst[ADE[V]], VExprToInst[VE])) {
        ADE[V] = VE;
      } else if (BasicExpression::classof(ADE[V])) {
        ADE[V]->setSave(true);
        VE->setReload(true);
      }
    }

    if (I.isTerminator()) {
      auto *T = dyn_cast<TerminatorInst>(&I);
      for (auto S : T->successors()) {
          for (auto F : BlockToFactors[S]) {
            if (F->getWillBeAvail()) {
              auto &PE = F->getPExpr();
              unsigned PI = F->getPredIndex(&B);
              auto O = F->getVExpr(PI);
              // Satisfies insert if either:
              //   - Version(O) is ⊥
              //   - HRU(O) is False and O is Factor and WBA(O) is False
              if (O == &BExpr ||
                  (!F->getHasRealUse(PI) &&
                   FactorExpression::classof(O) &&
                   !dyn_cast<FactorExpression>(O)->getWillBeAvail())) {
                // Insert the Expression at the of B
                if (!BlockToInserts.count(&B)) {
                  BlockToInserts.insert({&B, {&PE}});
                } else {
                  BlockToInserts[&B].insert(&PE);
                }
              } else {
                auto OV = O->getVersion();
                if (AvailDef.count(&PE)) {
                  auto &ADPE = AvailDef[&PE];
                  if (BasicExpression::classof(ADPE[OV])) {
                    ADPE[OV]->setSave(true);
                  }
                }
              }
            }
          }
      }
      break;
    }
  }
}

bool SSAPRE::
CodeMotion() {
  bool Changed = false;

  DenseMap<const Expression *, unsigned> PExprToCounter;
  for (auto &P : PExprToInsts) PExprToCounter.insert({P.getFirst(), 1});

  DenseMap<const Expression *, std::stack<std::pair<unsigned, Expression *>>>
    PExprToVExprStack;

  for (auto B : *RPOT) {
    // Since factors live outside basick blocks we set theirs DFS as the first
    // instruction's in the block
    auto FSDFS = InstrSDFS[&B->front()];

    for (auto FE : BlockToFactors[B]) {
      // Set Factor version
      FE->setVersion(PExprToCounter[&FE->getPExpr()]++);

      // Push VExpr onto stack Expr stack
      if (!PExprToVExprStack.count(FE)) {
        PExprToVExprStack.insert({FE, {}});
      }
      PExprToVExprStack[FE].push({FSDFS, FE});
    }

    for (auto &I : *B) {
      auto &VE = InstToVExpr[&I];
      auto &PE = VExprToPExpr[VE];

      // For each terminator we need to visit every cfg successor of this block
      // to update its Factor expressions
      if (I.isTerminator()) {
        auto *T = dyn_cast<TerminatorInst>(&I);
        for (auto S : T->successors()) {
          for (auto F : BlockToFactors[S]) {
            auto &VES = PExprToVExprStack[&F->getPExpr()];
            size_t PI = F->getPredIndex(B);
            assert(PI != -1UL && "Should not be the case");
            F->setVExpr(PI, VES.empty() ? &BExpr : VES.top().second);
          }
        }

        break;
      }

      // Do nothing for ignored expressions
      if (IgnoredExpression::classof(VE) || UnknownExpression::classof(VE))
        continue;

      auto SDFS = InstrSDFS[&I];
      auto &VES = PExprToVExprStack[PE];

      // Backtrace every PExprs' stack if we jumped up the tree
      for (auto &P : PExprToVExprStack) {
        auto &VES = P.getSecond();
        while (!VES.empty() && VES.top().first > SDFS) {
          VES.pop();
        }
      }

      if (VE->getSave()) {
        // Leave it be
        VES.push({SDFS, VE});
      } else if (VE->getReload()) {
        auto *VESTop = VES.empty() ? nullptr : VES.top().second;
        assert(VESTop && "This must not be null");
        assert(VESTop->getSave() && "This Value must be saved");
        auto &RI = VExprToInst[VESTop];
        I.replaceAllUsesWith(RI);
        // SHIT ugly af
        // Update Factors
        for (auto F : FExprs) {
          auto VEI = F->getVExprIndex(VE);
          if (VEI != -1UL) F->setVExpr(VEI, VESTop);
        }
        Changed = true;
      } else {
        assert(I.hasNUses(0) && "This instructin must not have any uses");
        KillList.insert(&I);
        Changed = true;
      }
    }
  }

  dbgs() << "KILL'EM ALL";
  for (auto I : KillList) {
    dbgs() << "\nKILL ";
    I->printAsOperand(dbgs());
    I->eraseFromParent();
  }

  // Insert PHIs for each available
  for (auto &P : BlockToFactors) {
    auto &B = P.getFirst();

    // Check parameters of potential PHIs, they are either:
    //  - Factor
    //  - Saved Expression
    for (auto F : P.getSecond()) {
      bool hasFactors = false;
      bool hasSaved = false;
      bool hasDeleted = false;
      for (auto &O : F->getVExprs()) {
        if (FactorExpression::classof(O)) {
          hasFactors = true;
          continue;
        }
        hasSaved   |= O->getSave();
        hasDeleted |= !O->getSave();
      }

      // Insert a PHI only if its operands are live
      if (hasFactors || hasSaved) {
        assert(!hasDeleted && "Must not be the case");
        IRBuilder<> Builder((Instruction *)B->getTerminator());
        auto BE = dyn_cast<BasicExpression>(&F->getPExpr());
        auto PHI = Builder.CreatePHI(BE->getType(), F->getVExprNum());
        for (unsigned i = 0, l = F->getVExprNum(); i < l; ++i) {
          auto VES = PExprToVExprStack[&F->getPExpr()];
          assert(!VES.empty() && "VES must not be empty");
          auto VESTop = VES.top().second;
          PHI->setIncomingValue(i, VExprToInst[VESTop]);
        }
        Changed = true;
      }
    }
  }
  return Changed;
}

PreservedAnalyses SSAPRE::
runImpl(Function &F,
        AssumptionCache &_AC,
        TargetLibraryInfo &_TLI, DominatorTree &_DT) {
  bool Changed = false;

  TLI = &_TLI;
  DL = &F.getParent()->getDataLayout();
  AC = &_AC;
  DT = &_DT;

  NumFuncArgs = F.arg_size();

  unsigned ICount = 1;
  // DFSToInstr.emplace_back(nullptr);

  // This is used during renaming step
  DenseMap<const Expression *, FactorRenamingContext> PExprToRC;

  RPOT = new ReversePostOrderTraversal<Function *>(&F);

  DenseMap<const DomTreeNode *, unsigned> RPOOrdering;
  unsigned Counter = 0;
  for (auto &B : *RPOT) {
    auto *Node = DT->getNode(B);
    assert(Node && "RPO and Dominator tree should have same reachability");

    // Assign each block RPO index
    RPOOrdering[Node] = ++Counter;

    // Collect all the expressions
    for (auto &I : *B) {
      // We map every instruction except terminators
      if (I.isTerminator()) continue;

      // Create ProtoExpresison, this expression will not be versioned and used
      // to bind Versioned Expressions of the same kind/class.
      auto PE = CreateExpression(I);
      // This is the real versioned expression
      auto VE = CreateExpression(I);

      assert(PE && VE && "Oh No!");

      InstToVExpr.insert({&I, VE});
      VExprToInst.insert({VE, &I});
      VExprToPExpr[VE] = PE;
      if (!PExprToVExprs.count(PE)) {
        PExprToVExprs.insert({PE, {VE}});
      } else {
        PExprToVExprs[PE].insert(VE);
      }

      // Map Proto-to-Reals and Proto-to-Blocks
      if (!PExprToInsts.count(PE)) {
        PExprToRC.insert({PE, {}});
        PExprToInsts.insert({PE, {&I}});
        PExprToBlocks.insert({PE, {B}});
      } else {
        PExprToInsts[PE].insert(&I);
        PExprToBlocks[PE].insert(B);
      }
    }
  }

  // Sort dominator tree children arrays into RPO.
  for (auto &B : *RPOT) {
    auto *Node = DT->getNode(B);
    if (Node->getChildren().size() > 1) {
      std::sort(Node->begin(), Node->end(),
                [&RPOOrdering](const DomTreeNode *A, const DomTreeNode *B) {
                  return RPOOrdering[A] < RPOOrdering[B];
                });
    }
  }

  // Assign each instruction a DFS order number. This will be the main order
  // we traverse DT in.
  auto DFI = df_begin(DT->getRootNode());
  for (auto DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    BasicBlock *B = DFI->getBlock();
    const auto &BlockRange = AssignDFSNumbers(B, ICount, &InstrDFS, &DFSToInstr);
    // BlockInstRange.insert({B, BlockRange});
    ICount += BlockRange.second - BlockRange.first;
  }

  // Now we need to create Reverse Sorted Dominator Tree, where siblings sorted
  // in the opposite to RPO order. This order will give us a clue when during
  // the normal traversal we go up the tree. For example:
  //
  //   CFG:    DT:
  //
  //    a       a     RPO(CFG): { a, c, b, d, e } // normal cfg rpo
  //   / \    / | \   DFS(DT):  { a, b, d, e, c } // before reorder
  //  b   c  b  d  c  DFS(DT):  { a, c, b, d, e } // after reorder
  //   \ /      |
  //    d       e     SDFS(DT): { a, d, e, b, c } // after reverse reorder
  //    |             SDFSO(DFS(DT),SDFS(DT)): { 1, 5, 4, 2, 3 }
  //    e                                          <  >  >  <
  //
  // So this SDFSO which maps our RPOish DFS(DT) onto SDFS order gives us points
  // where we must backtrace our context(stack or whatever we keep updated).
  // These are the places where the next SDFSO is less than the previous one.
  //
  for (auto &B : *RPOT) {
    auto *Node = DT->getNode(B);
    if (Node->getChildren().size() > 1) {
      std::sort(Node->begin(), Node->end(),
                [&RPOOrdering](const DomTreeNode *A, const DomTreeNode *B) {
                  // NOTE here we are using the reversed operator
                  return RPOOrdering[A] > RPOOrdering[B];
                });
    }
  }

  // Calculate Instruction-to-SDFS map
  ICount = 1;
  DFI = df_begin(DT->getRootNode());
  for (auto DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    BasicBlock *B = DFI->getBlock();
    const auto &BlockRange = AssignDFSNumbers(B, ICount, &InstrSDFS, nullptr);
    // BlockInstRange.insert({B, BlockRange});
    ICount += BlockRange.second - BlockRange.first;
  }

  // STEP 1: F-Insertion
  // Factors are inserted in two cases:
  //   - for each block in expressions IDF
  //   - for each phi of expression operand, which indicates expression alteration
  for (auto &P : PExprToInsts) {
    auto &PE = P.getFirst();
    if (IgnoredExpression::classof(PE) || UnknownExpression::classof(PE))
      continue;

    SmallVector<BasicBlock *, 32> IDF;
    ForwardIDFCalculator IDFs(*DT);
    IDFs.setDefiningBlocks(PExprToBlocks[PE]);
    // IDFs.setLiveInBlocks(BlocksWithDeadTerminators);
    IDFs.calculate(IDF);

    for (const auto &B : IDF) {
      auto F = CreateFactorExpression(*PE, *B);
      FExprs.insert(F);
      if (!BlockToFactors.count(B)) {
        BlockToFactors.insert({B, {F}});
      } else {
        BlockToFactors[B].insert({F});
      }
    }

    if (const auto *BE = dyn_cast<const BasicExpression>(PE)) {
      for (auto &O : BE->getOperands()) {
        if (const auto *PHI = dyn_cast<const PHINode>(O)) {
          // TODO
          // At this point we do not traverse phi-ud graph for expression's
          // operands since expressions by itself do not identify a phi-ud graph
          // as a single variable that changes over time
          auto B = PHI->getParent();
          auto F = CreateFactorExpression(*PE, *B);
          if (!BlockToFactors.count(B)) {
            BlockToFactors.insert({B, {F}});
          } else {
            BlockToFactors[B].insert({F});
          }
        }
      }
    }
  }

  DEBUG(PrintDebug("STEP 1"));

  // STEP 2: Rename
  // We assign SSA versions to each of 3 kinds of expressions:
  //   - Real expression
  //   - Factor expression
  //   - Factor operands, these generally versioned as Bottom
  DenseMap<const Expression *, std::stack<std::pair<unsigned, Expression *>>>
    PExprToVExprStack;
  for (auto B : *RPOT) {
    // Since factors live outside basick blocks we set theirs DFS as the first
    // instruction's in the block
    auto FSDFS = InstrSDFS[&B->front()];

    for (auto FE : BlockToFactors[B]) {
      auto &RC = PExprToRC[&FE->getPExpr()];

      // Set Factor version
      FE->setVersion(RC.Counter++);

      // Push VExpr onto stack Expr stack
      if (!PExprToVExprStack.count(FE)) {
        PExprToVExprStack.insert({FE, {}});
      }
      PExprToVExprStack[FE].push({FSDFS, FE});
    }

    for (auto &I : *B) {
      auto &VE = InstToVExpr[&I];
      auto &PE = VExprToPExpr[VE];

      // For each terminator we need to visit every cfg successor of this block
      // to update its Factor expressions
      if (I.isTerminator()) {
        auto *T = dyn_cast<TerminatorInst>(&I);
        for (auto S : T->successors()) {
          for (auto F : BlockToFactors[S]) {
            auto &VES = PExprToVExprStack[&F->getPExpr()];
            size_t PI = F->getPredIndex(B);
            assert(PI != -1UL && "Should not be the case");
            auto &VESTop = VES.top().second;
            F->setVExpr(PI, VES.empty() ? &BExpr : VESTop);

            // STEP 3 Init: HasRealUse
            // We set HasRealUse to True for an Factors' operands if they
            // reference a real instruction/expression, and not some another
            // Factor or Factor's operand definition; the last is TBD
            F->setHasRealUse(PI, BasicExpression::classof(VESTop));
          }
        }

        // FIXME Check if this is correct
        // STEP 3 Init: DownSafe
        // We set Factor's DownSafe safe to False if it is the last Expression's
        // occurence before program exit.
        if (T->getNumSuccessors() == 0) {
          for (auto &P : PExprToVExprStack) {
            if (auto *F = dyn_cast<FactorExpression>(P.getSecond().top().second)) {
              F->setDownSafe(false);
            }
          }
        }

        break;
      }

      // Do nothing for ignored expressions
      if (IgnoredExpression::classof(VE) || UnknownExpression::classof(VE))
        continue;

      auto SDFS = InstrSDFS[&I];
      auto &RC = PExprToRC[PE];
      auto &VES = PExprToVExprStack[PE];

      // Backtrace every PExprs' stack if we jumped up the tree
      for (auto &P : PExprToVExprStack) {
        auto &VES = P.getSecond();
        while (!VES.empty() && VES.top().first > SDFS) {
          VES.pop();
        }
      }

      // TODO
      // This is a simplified version for operand comparison, normally we
      // would check current operands on their respected stacks with operands
      // for the VExpr on its stack, if they match we assign the same version,
      // otherwise there was a def for VExpr operand and we need to assign a new
      // version. This will be required when operand versioning is implemented.
      //
      // For now this will suffice, the only case we reuse a version if we've
      // seen this expression before, since in SSA there is a singe def for
      // an operand.
      //
      // This limits algorithm effectiveness, because we do not track operands'
      // versions we cannot prove that certain separate expressions are in fact
      // the same expressions of different versions. TBD, anyway.
      //
      // Another thing related to not tracking operand versions, because of that
      // there always will be a single definition of VExpr's operand and the
      // VExpr itself will follow it in the traversal, thus, for now, we do not
      // have to assign ⊥ version to the VExpr whenever we see its operand
      // defined.
      auto *VESTop = VES.empty() ? nullptr : VES.top().second;
      if (VESTop && VExprToPExpr[VESTop] == PE) {
        // If the top of stack contains take its version
        VE->setVersion(VESTop->getVersion());
      } else {
        // Otherwise assign new version
        VE->setVersion(RC.Counter++);

        // STEP 3 Init: DownSafe
        // If the top of the stack contains a Factor expression we clear its
        // DownSafe flag because its result is not used and not anticipated
        if (VESTop && FactorExpression::classof(VESTop)) {
          auto *F = dyn_cast<FactorExpression>(VESTop);
          F->setDownSafe(false);
        }
      }

      VES.push({SDFS, VE});
    }
  }

  DEBUG(PrintDebug("STEP 2"));

  // STEP 3 Calculating DownSafety
  DownSafety();

  DEBUG(PrintDebug("STEP 3"));

  // STEP 4 Calculating WillBeAvail
  WillBeAvail();

  DEBUG(PrintDebug("STEP 4"));

  // STEP 5 Finalize
  for (auto B : *RPOT) {
    FinalizeVisit(*B);
  }

  DEBUG(PrintDebug("STEP 5"));

  // STEP 6 Code Motion
  Changed = CodeMotion();

  DEBUG(PrintDebug("STEP 6"));

  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}

PreservedAnalyses SSAPRE::run(Function &F, AnalysisManager<Function> &AM) {
  return runImpl(F,
      AM.getResult<AssumptionAnalysis>(F),
      AM.getResult<TargetLibraryAnalysis>(F),
      AM.getResult<DominatorTreeAnalysis>(F));
}


//===----------------------------------------------------------------------===//
// Pass Legacy
//
// Do I need to keep it?
//===----------------------------------------------------------------------===//

class llvm::ssapre::SSAPRELegacy : public FunctionPass {
  DominatorTree *DT;

public:
  static char ID; // Pass identification, replacement for typeid.
  SSAPRE Impl;
  SSAPRELegacy() : FunctionPass(ID) {
    initializeSSAPRELegacyPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "SSAPRE"; }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto PA = Impl.runImpl(F, AC, TLI, DT);
    return !PA.areAllPreserved();
  }

private:
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
  }
};

char SSAPRELegacy::ID = 0;

// createSSAPREPass - The public interface to this file.
FunctionPass *llvm::createSSAPREPass() { return new SSAPRELegacy(); }

INITIALIZE_PASS_BEGIN(SSAPRELegacy,
                      "ssapre",
                      "SSA Partial Redundancy Elimination",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(BreakCriticalEdges)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(SSAPRELegacy,
                    "ssapre",
                    "SSA Partial Redundancy Elimination",
                    false, false)
