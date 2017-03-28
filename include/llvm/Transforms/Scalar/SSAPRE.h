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
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/ArrayRecycler.h"
#include <stack>

namespace llvm {

namespace ssapre LLVM_LIBRARY_VISIBILITY {

class SSAPRELegacy;


//===----------------------------------------------------------------------===//
// Pass Expressions
//===----------------------------------------------------------------------===//

enum ExpressionType {
  ET_Base,
  ET_Buttom,
  ET_Ignored,
  ET_Unknown,
  ET_Constant,
  ET_Variable,
  ET_Factor, // Phi for expressions, Φ in paper
  ET_BasicStart,
  ET_Basic,
  ET_Phi,
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
  int Version;

  Instruction *Proto;

  bool Save;
  bool Reload;

public:
  Expression(ExpressionType ET = ET_Base, unsigned O = ~2U, bool S = true)
      : EType(ET), Opcode(O), Version(-1),
        Proto(nullptr),
        Save(S), Reload(false) {}
  // Expression(const Expression &) = delete;
  // Expression &operator=(const Expression &) = delete;
  virtual ~Expression();

  unsigned getOpcode() const { return Opcode; }
  void setOpcode(unsigned opcode) { Opcode = opcode; }
  ExpressionType getExpressionType() const { return EType; }

  int getVersion() const { return Version; }
  void setVersion(int V) { Version = V; }

  Instruction * getProto() const { return Proto; }
  void setProto(Instruction *I) { Proto = I; }

  int getSave() const { return Save; }
  void setSave(int S) { Save = S; }

  int getReload() const { return Reload; }
  void setReload(int R) { Reload = R; }

  static unsigned getEmptyKey() { return ~0U; }
  static unsigned getTombstoneKey() { return ~1U; }

  bool operator==(const Expression &O) const {
    if (getOpcode() != O.getOpcode())
      return false;
    if (getOpcode() == getEmptyKey() || getOpcode() == getTombstoneKey())
      return true;
    // Compare the expression type for anything but load and store.
    // For load and store we set the opcode to zero.
    // This is needed for load coercion.
    // TODO figure out the reason for this
    // if (getExpressionType() != ET_Load && getExpressionType() != ET_Store &&
    //     getExpressionType() != O.getExpressionType())
    //   return false;

    return equals(O);
  }

  virtual bool equals(const Expression &O) const {
    if (EType == O.EType && Opcode == O.Opcode && Version == O.Version) {
      assert(Save == O.Save && Reload == O.Reload &&
          "Expressions are not fully equal");
      return true;
    }
    return false;
  }

  virtual void printInternal(raw_ostream &OS) const {
    OS << ExpressionTypeToString(getExpressionType());
    OS << ", V: " << Version;
    OS << ", S: " << (Save ? "T" : "F");
    OS << ", R: " << (Reload ? "T" : "F");
    OS << ", OPC: " << getOpcode() << ", ";
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
  IgnoredExpression(IgnoredExpression &) = delete;
  IgnoredExpression &operator=(const IgnoredExpression &) = delete;
  ~IgnoredExpression() override;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Ignored;
  }

  Instruction *getInstruction() const { return Inst; }
  void setInstruction(Instruction *I) { Inst = I; }

  bool equals(const Expression &O) const override {
    if (auto OU = dyn_cast<IgnoredExpression>(&O))
      return Expression::equals(O) && Inst == OU->Inst;
    return false;
  }

  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "I = " << *Inst;
  }
}; // class IgnoredExpression

class UnknownExpression final : public IgnoredExpression {
public:
  UnknownExpression(Instruction *I) : IgnoredExpression(I, ET_Unknown) {}
  UnknownExpression() = delete;
  UnknownExpression(UnknownExpression &) = delete;
  UnknownExpression &operator=(const UnknownExpression &) = delete;
  ~UnknownExpression() override;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Unknown;
  }
};

class VariableExpression final : public Expression {
private:
  Value &VariableValue;

public:
  VariableExpression(Value &V) : Expression(ET_Variable), VariableValue(V) {}
  VariableExpression() = delete;
  VariableExpression(const VariableExpression &) = delete;
  VariableExpression &operator=(const VariableExpression &) = delete;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Variable;
  }

  bool equals(const Expression &Other) const override {
    const VariableExpression &OC = cast<VariableExpression>(Other);
    return &VariableValue == &OC.VariableValue;
  }

  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "A: " << VariableValue;
  }
};

class ConstantExpression final : public Expression {
private:
  Constant &ConstantValue;

public:
  ConstantExpression(Constant &C)
      : Expression(ET_Constant), ConstantValue(C) {}
  ConstantExpression() = delete;
  ConstantExpression(const ConstantExpression &) = delete;
  ConstantExpression &operator=(const ConstantExpression &) = delete;

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Constant;
  }

  bool equals(const Expression &Other) const override {
    const ConstantExpression &OC = cast<ConstantExpression>(Other);
    return &ConstantValue == &OC.ConstantValue;
  }

  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "C:" << ConstantValue;
  }
};

class BasicExpression : public Expression {
private:
  // typedef ArrayRecycler<Value *> RecyclerType;
  // typedef RecyclerType::Capacity RecyclerCapacity;
  SmallVector<Value *, 2> Operands; // TODO use Expressions here
  Type *ValueType;

public:
  BasicExpression(ExpressionType ET = ET_Basic)
      : Expression(ET), ValueType(nullptr) {}
  BasicExpression(const BasicExpression &) = delete;
  BasicExpression &operator=(const BasicExpression &) = delete;
  ~BasicExpression() override;

  static bool classof(const Expression *EB) {
    ExpressionType ET = EB->getExpressionType();
    return ET > ET_BasicStart && ET < ET_BasicEnd;
  }

  void addOperand(Value *V) {
    Operands.push_back(V);
  }
  Value *getOperand(unsigned N) const {
    return Operands[N];
  }
  void setOperand(unsigned N, Value *V) {
    assert(N < Operands.size() && "Operand out of range");
    Operands[N] = V;
  }
  void swapOperands(unsigned First, unsigned Second) {
    std::swap(Operands[First], Operands[Second]);
  }
  const SmallVector<Value *, 2>& getOperands() const {
    return Operands;
  }

  unsigned getNumOperands() const { return Operands.size(); }

  void setType(Type *T) { ValueType = T; }
  Type *getType() const { return ValueType; }

  bool equals(const Expression &O) const override {
    if (!Expression::equals(O))
      return false;

    if (auto OE = dyn_cast<BasicExpression>(&O)) {
      return getType() == OE->getType() && Operands == OE->Operands;
    }
    return false;
  }

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
  Expression *PE; // common PE of the expressions it functions on
  BasicBlock *BB;

public:
  PHIExpression(BasicBlock *BB)
    : BasicExpression(ET_Phi), PE(PHIExpression::getPExprNotSet()), BB(BB) {}
  PHIExpression() = delete;
  PHIExpression(const PHIExpression &) = delete;
  PHIExpression &operator=(const PHIExpression &) = delete;
  ~PHIExpression() override;

  bool isCommonPExprSet() const {
    return PE != PHIExpression::getPExprNotSet();
  }
  bool hasCommonPExpr() const {
    return isCommonPExprSet() && PE != PHIExpression::getPExprMismatch();
  }
  void setCommonPExpr(Expression *E) { assert(E); PE = E; }
  Expression * getCommonPExpr() const { return PE; }

  static Expression * getPExprNotSet()   { return (Expression *)~0; }
  static Expression * getPExprMismatch() { return (Expression *)~1; }

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Phi;
  }

  bool equals(const Expression &O) const override {
    if (!this->BasicExpression::equals(O))
      return false;
    if (auto OE = dyn_cast<PHIExpression>(&O)) {
      return BB == OE->BB;
    }
    return false;
  }

  void printInternal(raw_ostream &OS) const override {
    this->BasicExpression::printInternal(OS);
    OS << "BB: ";
    BB->printAsOperand(dbgs());
  }
}; // class PHIExpression

class FactorExpression final : public Expression {
private:
  const Expression &PE;
  const BasicBlock &BB;
  const PHIExpression *PHI;
  SmallVector<const BasicBlock *, 8> Pred;
  SmallVector<Expression *, 8> Versions;

  // If True expression is Anticipated on every path leading from this Factor
  bool DownSafe;
  // True if an Operand is a Real expressin and not Factor or Expression Operand
  // definition(⊥)
  SmallVector<bool, 8> HasRealUse;

  bool CanBeAvail;
  bool Later;

public:
  FactorExpression(const Expression &PE, const BasicBlock &BB,
                   SmallVector<const BasicBlock *, 8> P)
      : Expression(ET_Factor), PE(PE), BB(BB), PHI(nullptr), Pred(P),
                   Versions(P.size(), nullptr),
                   DownSafe(true), HasRealUse(P.size(), false),
                   CanBeAvail(true), Later(true) { }
  FactorExpression() = delete;
  FactorExpression(const FactorExpression &) = delete;
  FactorExpression &operator=(const FactorExpression &) = delete;
  ~FactorExpression() override;

  void setLinkedPHI(const PHIExpression *P) { PHI = P; }
  const PHIExpression * getLinkedPHI() const { return PHI; }

  const Expression& getPExpr() const { return PE; }

  size_t getPredIndex(BasicBlock * B) const {
    for (size_t i = 0; i < Pred.size(); ++i) {
      if (Pred[i] == B) {
        return i;
      }
    }
    return -1;
  }

  size_t getVExprNum() { return Versions.size(); }
  void setVExpr(unsigned P, Expression * V) { Versions[P] = V; }
  Expression * getVExpr(unsigned P) { return Versions[P]; }
  size_t getVExprIndex(Expression &V) {
    for(size_t i = 0, l = Versions.size(); i < l; ++i) {
      if (Versions[i] == &V)
        return i;
    }
    return -1;
  }

  bool hasVExpr(Expression &V) {
    return getVExprIndex(V) != -1UL;
  }

  SmallVector<Expression *, 8> getVExprs() { return Versions; };

  bool getDownSafe() const { return DownSafe; }
  void setDownSafe(bool DS) { DownSafe = DS; }

  bool getCanBeAvail() const { return CanBeAvail; }
  void setCanBeAvail(bool CBA) { CanBeAvail = CBA; }

  bool getLater() const { return Later; }
  void setLater(bool L) { Later = L; }

  bool getWillBeAvail() const { return CanBeAvail && !Later; }

  void setHasRealUse(unsigned P, bool HRU) { HasRealUse[P] = HRU; }
  bool getHasRealUse(unsigned P) const { return HasRealUse[P]; }

  static bool classof(const Expression *EB) {
    return EB->getExpressionType() == ET_Factor;
  }

  bool equals(const Expression &O) const override {
    if (!this->Expression::equals(O))
      return false;
    if (auto OE = dyn_cast<FactorExpression>(&O)) {
      return &BB == &OE->BB;
    }
    return false;
  }

  void printInternal(raw_ostream &OS) const override {
    this->Expression::printInternal(OS);
    OS << "BB: ";
    BB.printAsOperand(OS, false);
    OS << ", LNK: " << (PHI ? "T" : "F");
    OS << ", V: <";
    for (unsigned i = 0, l = Versions.size(); i < l; ++i) {
      if (Versions[i]) {
        OS << Versions[i]->getVersion();
      } else {
        OS << "⊥";
      }
      if (i + 1 != l) OS << ",";
    }
    OS << ">";
    OS << ", DS: " << (DownSafe ? "T" : "F");
    OS << ", HRU: <";
    for (unsigned i = 0, l = HasRealUse.size(); i < l; ++i) {
      OS << (HasRealUse[i] ? "T" : "F");
      if (i + 1 != l) OS << ",";
    }
    OS << ">";
    OS << ", CBA: " << (CanBeAvail ? "T" : "F");
    OS << ", L: " << (Later ? "T" : "F");
    OS << ", WBA: " << (getWillBeAvail() ? "T" : "F");
  }
}; // class FactorExpression

class FactorRenamingContext {
public:
  unsigned Counter;
  std::stack<int> Stack;
};

} // end namespace ssapre

using namespace ssapre;

/// Performs SSA PRE pass.
class SSAPRE : public PassInfoMixin<SSAPRE> {
  const DataLayout *DL;
  const TargetLibraryInfo *TLI;
  AssumptionCache *AC;
  DominatorTree *DT;
  ReversePostOrderTraversal<Function *> *RPOT;

  SmallPtrSet<const BasicBlock *, 32> JoinBlocks;

  // Values' stuff
  DenseMap<Expression *, const Value *> ExpToValue;
  DenseMap<const Value *, Expression *> ValueToExp;

  // Arguments' stuff
  unsigned int NumFuncArgs;
  DenseMap<VariableExpression *, Value *> VAExpToValue;
  DenseMap<const Value *, VariableExpression *> ValueToVAExp;

  // Constants' stuff
  DenseMap<ConstantExpression *, Value *> COExpToValue;
  DenseMap<const Value *, ConstantExpression *> ValueToCOExp;

  // DFS info.
  // This contains a mapping from Instructions to DFS numbers.
  // The numbering starts at 1. An instruction with DFS number zero
  // means that the instruction is dead.
  typedef DenseMap<const Value *, unsigned> InstrToOrderType;
  InstrToOrderType InstrDFS;
  InstrToOrderType InstrSDFS;

  // This contains the mapping DFS numbers to instructions.
  typedef SmallVector<const Value *, 32> OrderedInstrType;
  OrderedInstrType DFSToInstr;

  // Instruction-to-Expression map
  DenseMap<const Instruction *, Expression *> InstToVExpr;
  DenseMap<const Expression *, Instruction *> VExprToInst;

  // ProtoExpression-to-Instructions map
  DenseMap<const Expression *, SmallPtrSet<const Instruction *, 5>> PExprToInsts;

  DenseMap<const Expression *, DenseMap<unsigned, SmallPtrSet<Expression *, 5>>> PExprToVersions;

  // ProtoExpression-to-BasicBlock map
  DenseMap<const Expression *, SmallPtrSet<BasicBlock *, 5>> PExprToBlocks;

  // BasicBlock-to-FactorList map
  DenseMap<const BasicBlock *, SmallPtrSet<FactorExpression *, 5>> BlockToFactors;
  DenseMap<const FactorExpression *, const BasicBlock *> FactorToBlock;

  // Map PHI to Factor if PHI joins two expressions of the same proto
  DenseMap<FactorExpression *, PHIExpression *> FExprToPHIExpr;
  DenseMap<PHIExpression *, FactorExpression *> PHIExprToFExpr;

  // ProtoExpression-to-VersionedExpressions
  DenseMap<const Expression *, SmallPtrSet<Expression *, 5>> PExprToVExprs;

  // VersionedExpression-to-ProtoVersioned
  DenseMap<const Expression *, const Expression *> VExprToPExpr;

  SmallPtrSet<FactorExpression *, 32> FExprs;

  DenseMap<const Expression *, DenseMap<int, Expression *>> AvailDef;

  DenseMap<const BasicBlock *, SmallPtrSet<Instruction *, 5>> BlockToInserts;

  // This map contains 1-to-1 correspondence between Expression Occurrence and
  // its Definition. Upon initialization Keys will be equal to Values, once
  // an Expression assumes existing Version it must define its Definition, so
  // that during kill time we could replace its use with a proper definition.
  DenseMap<Expression *, Expression *> Substitutions;
  SmallPtrSet<Instruction *, 8> KillList;

public:
  PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM);

private:
  friend ssapre::SSAPRELegacy;

  std::pair<unsigned, unsigned> AssignDFSNumbers(BasicBlock *B, unsigned Start,
                                                 InstrToOrderType *M,
                                                 OrderedInstrType *V);

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

  bool Dominates(const Expression *Def, const Expression *Use);

  // Check whether Expression operands' definitions dominate the Factor
  bool OperandsDominate(Expression *Exp, const FactorExpression *F);

  bool FactorHasRealUse(const FactorExpression *F);

  // Take a Value returned by simplification of Expression E/Instruction
  // I, and see if it resulted in a simpler expression. If so, return
  // that expression.
  // TODO: Once finished, this should not take an Instruction, we only
  // use it for printing.
  Expression * CheckSimplificationResults(Expression *E,
                                          Instruction &I,
                                          Value *V);
  ConstantExpression * CreateConstantExpression(Constant &C);
  VariableExpression * CreateVariableExpression(Value &V);
  Expression * CreateIgnoredExpression(Instruction &I);
  Expression * CreateUnknownExpression(Instruction &I);
  Expression * CreateBasicExpression(Instruction &I);
  Expression * CreatePHIExpression(PHINode &I);
  FactorExpression * CreateFactorExpression(const Expression &E,
                                            const BasicBlock &B);
  Expression * CreateExpression(Instruction &I);

  bool IgnoreExpression(const Expression &E);

  // It is possible that a "materialized" Factor already exists in the code
  // if form of a PHI expression that joins two expressions of the same proto and
  // we need to account for that.
  // FIXME remove this
  void SetCommonProto(PHIExpression &PHI);

  void PrintDebug(const std::string &Caption);

  void Init(Function &F);
  void Fini();

  void FactorInsertion();

  void Rename();

  void ResetDownSafety(FactorExpression &F, unsigned O);
  void DownSafety();

  void ComputeCanBeAvail();
  void ResetCanBeAvail(FactorExpression &F);
  void ComputeLater();
  void ResetLater(FactorExpression &F);
  void WillBeAvail();

  void FinalizeVisit(BasicBlock &B);
  void Finalize();

  bool CodeMotion();

  PreservedAnalyses runImpl(Function &F, AssumptionCache &_AC,
                            TargetLibraryInfo &_TLI, DominatorTree &_DT);
};
} // end namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_SSAPRE_H
