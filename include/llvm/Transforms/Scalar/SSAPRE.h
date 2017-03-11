//===-------- SSAPRE.h - SSA PARTIAL REDUNDANCY ELIMINATION -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the interface for LLVM's SSA Partial Redundancy Elimination
// pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_SSAPRE_H
#define LLVM_TRANSFORMS_SCALAR_SSAPRE_H

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/ArrayRecycler.h"

namespace llvm {

namespace ssapre LLVM_LIBRARY_VISIBILITY {

class SSAPRELegacy;


//===----------------------------------------------------------------------===//
// Pass Expressions
//===----------------------------------------------------------------------===//

enum ExpressionType {
  ET_Base,
  ET_Ignored,
  ET_Unknown,
  ET_BasicStart,
  ET_Basic,
  ET_Phi,
  ET_Factor, // Phi for expressions, Φ in paper
  // TODO later
  // ET_Call,
  // ET_AggregateValue,
  // ET_Load,
  // ET_Store,
  ET_BasicEnd

};

inline std::string ExpressionTypeToString(ExpressionType ET) {
  switch (ET) {
  case ET_Ignored: return "ExpressionTypeIgnored";
  case ET_Unknown: return "ExpressionTypeUnknown";
  case ET_Basic:   return "ExpressionTypeBasic";
  case ET_Phi:     return "ExpressionTypePhi";
  case ET_Factor:  return "ExpressionTypeFactor";
  default:         return "ExpressionType???";
  }
}

class Expression {
private:
  ExpressionType EType;
  unsigned Opcode;

public:
  Expression(ExpressionType ET = ET_Base, unsigned O = ~2U)
      : EType(ET), Opcode(O) {}
  // Expression(const Expression &) = delete;
  // Expression &operator=(const Expression &) = delete;
  virtual ~Expression();

  unsigned getOpcode() const { return Opcode; }
  void setOpcode(unsigned opcode) { Opcode = opcode; }
  ExpressionType getExpressionType() const { return EType; }

  // ??? What are those?
  static unsigned getEmptyKey() { return ~0U; }
  static unsigned getTombstoneKey() { return ~1U; }

  bool operator==(const Expression &Other) const {
    if (getOpcode() != Other.getOpcode())
      return false;
    if (getOpcode() == getEmptyKey() || getOpcode() == getTombstoneKey())
      return true;
    // Compare the expression type for anything but load and store.
    // For load and store we set the opcode to zero.
    // This is needed for load coercion.
    // TODO figure out the reason for this
    // if (getExpressionType() != ET_Load && getExpressionType() != ET_Store &&
    //     getExpressionType() != Other.getExpressionType())
    //   return false;

    return equals(Other);
  }

  virtual bool equals(const Expression &Other) const { return true; }

  virtual hash_code getHashValue() const {
    return hash_combine(getExpressionType(), getOpcode());
  }

  //
  // Debugging support
  //
  virtual void printInternal(raw_ostream &OS) const {
    OS << ExpressionTypeToString(getExpressionType()) << ", ";
    OS << "OPC: " << getOpcode() << ", ";
  }

  void print(raw_ostream &OS) const {
    OS << "{ ";
    printInternal(OS);
    OS << "}";
  }

  void dump() const { print(dbgs()); }
}; // class Expression

class IgnoredExpression : public Expression {
protected:
  Instruction *Inst;

public:
  IgnoredExpression(Instruction *I) : IgnoredExpression(I, ET_Ignored) {}
  IgnoredExpression(Instruction *I, ExpressionType ET) : Expression(ET), Inst(I) {}
  IgnoredExpression() = delete;
  IgnoredExpression(const IgnoredExpression &) = delete;
  IgnoredExpression &operator=(const IgnoredExpression &) = delete;
  ~IgnoredExpression() override;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Ignored;
  }

  Instruction *getInstruction() const { return Inst; }
  void setInstruction(Instruction *I) { Inst = I; }

  bool equals(const Expression &Other) const override {
    const auto &OU = cast<IgnoredExpression>(Other);
    return Expression::equals(Other) && Inst == OU.Inst;
  }

  hash_code getHashValue() const override {
    return hash_combine(getExpressionType(), Inst);
  }

  //
  // Debugging support
  //
  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "I = " << *Inst;
  }
}; // class IgnoredExpression

class UnknownExpression final : public IgnoredExpression {
public:
  UnknownExpression(Instruction *I) : IgnoredExpression(I, ET_Unknown) {}
  UnknownExpression() = delete;
  UnknownExpression(const UnknownExpression &) = delete;
  UnknownExpression &operator=(const UnknownExpression &) = delete;
  ~UnknownExpression() override;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Unknown;
  }
};

class BasicExpression : public Expression {
private:
  typedef ArrayRecycler<Value *> RecyclerType;
  typedef RecyclerType::Capacity RecyclerCapacity;
  SmallVector<Value *, 2> Operands;
  Type *ValueType;

public:
  BasicExpression()
      : BasicExpression({}, ET_Basic) {}
  BasicExpression(SmallVector<Value *, 2> O)
      : BasicExpression(O, ET_Basic) {}
  BasicExpression(SmallVector<Value *, 2> O, ExpressionType ET)
      : Expression(ET), Operands(O) {}
  // BasicExpression(const BasicExpression &) = delete;
  // BasicExpression &operator=(const BasicExpression &) = delete;
  ~BasicExpression() override;

  static bool classof(const Expression *EB) {
    ExpressionType ET = EB->getExpressionType();
    return ET > ET_BasicStart && ET < ET_BasicEnd;
  }

  void swapOperands(unsigned First, unsigned Second) {
    std::swap(Operands[First], Operands[Second]);
  }

  Value *getOperand(unsigned N) const {
    return Operands[N];
  }

  SmallVector<Value *, 2>& getOperands() {
    return Operands;
  }

  void addOperand(Value *V) {
    Operands.push_back(V);
  }

  void setOperand(unsigned N, Value *V) {
    assert(N < Operands.size() && "Operand out of range");
    Operands[N] = V;
  }

  unsigned getNumOperands() const { return Operands.size(); }

  void setType(Type *T) { ValueType = T; }
  Type *getType() const { return ValueType; }

  bool equals(const Expression &Other) const override {
    if (getOpcode() != Other.getOpcode())
      return false;

    const auto &OE = cast<BasicExpression>(Other);
    return getType() == OE.getType() && Operands == OE.Operands;
  }

  hash_code getHashValue() const override {
    return hash_combine(getExpressionType(), getOpcode(), ValueType,
        hash_combine_range(Operands.begin(), Operands.end()));
  }

  //
  // Debugging support
  //
  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "OPS: { ";
    for (unsigned i = 0, e = getNumOperands(); i != e; ++i) {
      OS << "[" << i << "] = ";
      Operands[i]->printAsOperand(OS);
      if (i + 1 != e)
        OS << ", ";
    }
    OS << " }";
  }
}; // class BasicExpression

class PHIExpression final : public BasicExpression {
private:
  BasicBlock *BB;

public:
  PHIExpression(SmallVector<Value *, 2> O, BasicBlock *BB)
      : BasicExpression(O, ET_Phi), BB(BB) {}
  PHIExpression() = default;
  // PHIExpression(const PHIExpression &) = delete;
  // PHIExpression &operator=(const PHIExpression &) = delete;
  ~PHIExpression() override;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Phi;
  }

  bool equals(const Expression &Other) const override {
    if (!this->BasicExpression::equals(Other))
      return false;
    const PHIExpression &OE = cast<PHIExpression>(Other);
    return BB == OE.BB;
  }

  hash_code getHashValue() const override {
    return hash_combine(this->BasicExpression::getHashValue(), BB);
  }

  //
  // Debugging support
  //
  void printInternal(raw_ostream &OS) const override {
    this->BasicExpression::printInternal(OS);
    OS << "BB = " << BB->getName();
  }
}; // class PHIExpression

class FactorExpression final : public Expression {
private:
  BasicBlock *BB;
  std::vector<Expression *> Expressions;

public:
  FactorExpression(BasicBlock *BB, std::vector<Expression *> E)
      : Expression(ET_Factor), BB(BB), Expressions(E) {}
  FactorExpression() = delete;
  FactorExpression(const FactorExpression &) = delete;
  FactorExpression &operator=(const FactorExpression &) = delete;
  ~FactorExpression() override;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Factor;
  }

  bool equals(const Expression &Other) const override {
    if (!this->Expression::equals(Other))
      return false;
    const FactorExpression &OE = cast<FactorExpression>(Other);
    return BB == OE.BB;
  }

  hash_code getHashValue() const override {
    return hash_combine(this->Expression::getHashValue(), BB);
  }

  //
  // Debugging support
  //
  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "BB = " << BB->getName();
  }
}; // class FactorExpression
} // end namespace ssapre

using namespace ssapre;

template <> struct DenseMapInfo<const Expression *> {
  static const Expression *getEmptyKey() {
    auto Val = static_cast<uintptr_t>(-1);
    Val <<= PointerLikeTypeTraits<const Expression *>::NumLowBitsAvailable;
    return reinterpret_cast<const Expression *>(Val);
  }
  static const Expression *getTombstoneKey() {
    auto Val = static_cast<uintptr_t>(~1U);
    Val <<= PointerLikeTypeTraits<const Expression *>::NumLowBitsAvailable;
    return reinterpret_cast<const Expression *>(Val);
  }
  static unsigned getHashValue(const Expression *V) {
    return static_cast<unsigned>(V->getHashValue());
  }
  static bool isEqual(const Expression *LHS, const Expression *RHS) {
    if (LHS == RHS)
      return true;
    if (LHS == getTombstoneKey() || RHS == getTombstoneKey() ||
        LHS == getEmptyKey() || RHS == getEmptyKey())
      return false;
    return LHS->equals(*RHS);
  }
};

/// Performs SSA PRE pass.
class SSAPRE : public PassInfoMixin<SSAPRE> {
  DominatorTree *DT;
  const DataLayout *DL;
  const TargetLibraryInfo *TLI;
  AssumptionCache *AC;

  // Number of function arguments, used by ranking
  unsigned int NumFuncArgs;

  // DFS info.
  // This contains a mapping from Instructions to DFS numbers.
  // The numbering starts at 1. An instruction with DFS number zero
  // means that the instruction is dead.
  DenseMap<const Value *, unsigned> InstrDFS;

  // Expression-to-Definitions map
  DenseMap<const Expression *, SmallPtrSet<const Value *, 5>> ExpressionToInsts;

public:
  PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM);

private:
  friend ssapre::SSAPRELegacy;

  std::pair<unsigned, unsigned> AssignDFSNumbers(BasicBlock *B, unsigned Start);

  // This function provides global ranking of operations so that we can place them
  // in a canonical order.  Note that rank alone is not necessarily enough for a
  // complete ordering, as constants all have the same rank.  However, generally,
  // we will simplify an operation with all constants so that it doesn't matter
  // what order they appear in.
  unsigned int GetRank(const Value *V) const;

  // This is a function that says whether two commutative operations should
  // have their order swapped when canonicalizing.
  bool ShouldSwapOperands(const Value *A, const Value *B) const;

  bool FillInBasicExpressionInfo(Instruction &I, BasicExpression *E);

  // Take a Value returned by simplification of Expression E/Instruction
  // I, and see if it resulted in a simpler expression. If so, return
  // that expression.
  // TODO: Once finished, this should not take an Instruction, we only
  // use it for printing.
  Expression * CheckSimplificationResults(Expression *E,
                                          Instruction &I, Value *V);
  Expression * CreateIgnoredExpression(Instruction &I);
  Expression * CreateUnknownExpression(Instruction &I);
  Expression * CreateBasicExpression(Instruction &I);
  Expression * CreatePHIExpression(Instruction &I);
  Expression * CreateExpression(Instruction &I);
  PreservedAnalyses runImpl(Function &F, AssumptionCache &_AC,
                            TargetLibraryInfo &_TLI, DominatorTree &_DT);
};
} // end namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_SSAPRE_H
