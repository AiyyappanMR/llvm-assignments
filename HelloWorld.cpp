//===-- HelloWorld.cpp - Example Transformations --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/HelloWorld.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"

using namespace llvm;

//At -O0 every C variable is alloca + load/store.
//"trackable" = only plain load/store use it (address never escapes), safe to track
bool nanIsTrackable(AllocaInst *AI)
{
  for(User *U : AI->users()) {
    if(auto *LI = dyn_cast<LoadInst>(U)) {
      if(!LI->isSimple())
        return false;
    } else if(auto *SI = dyn_cast<StoreInst>(U)) {
      if(!SI->isSimple() || SI->getPointerOperand() != AI)
        return false;
    } else {
      return false;
    }
  }
  return true;
}

AllocaInst *nanTrackedPtr(Value *Ptr)
{
  auto *AI = dyn_cast<AllocaInst>(Ptr);//is the pointer an alloca?
  if(AI && nanIsTrackable(AI))
    return AI;
  return NULL;
}

bool nanIsSimpleConst(Value *V)
{
  return isa<ConstantInt>(V) || isa<ConstantFP>(V);
}

//---------------- 1. algebraic identity: x+0, x-0, x*1, x/1 ----------------
bool nanAlgebricXopIdInternal(BinaryOperator *BI)
{
      auto op1 = BI->getOperand(0);//first operand
      auto op2 = BI->getOperand(1);//second operand

      auto op2c = dyn_cast<Constant>(op2);//typecast second operand to constant
      if(!op2c)
        return false;

      Value *finalci = NULL;//initialize to NULL

      switch(BI->getOpcode()) {//check op code of the operator
      default:
        break;
      case Instruction::Add:
      case Instruction::Sub:
        if(op2c->isNullValue()) {//if value of constant is Zero
          finalci = op1;//operand one
        }
        break;
      case Instruction::Mul:
      case Instruction::SDiv:
      case Instruction::UDiv:
        if(op2c->isOneValue()) {//if value of constant is One
          finalci = op1;//operand one
        }
        break;
      }

      if(finalci) {//if not NULL
        errs()<<"Replacing all uses of " << *BI<<" with its first operand\n";
        BI->replaceAllUsesWith(finalci);//replace all uses of BI with its operand one
        return true;
      }

      return false;
}

bool nanAlgebraicXopId(BasicBlock &BB)
{
  bool changed = false;
  bool result = false;//remembers if ANY instruction in the block changed
  for (auto I = BB.begin(), End = BB.end()/*initial*/; I != End/*termination*/; ) {
    changed = false;
    llvm::Instruction *Inst = &*I;//llvm::Instruction from Iterator
    I++;//increment the iterator I
    if(BinaryOperator *BI = dyn_cast<BinaryOperator>(Inst)) {//typecast instruction to Binary operation
      changed = nanAlgebricXopIdInternal(BI);//XopID check

      if(!changed && BI->isCommutative()) {//check IDopX for commutative operators
        BI->swapOperands();//LLVM swap operands
        changed = nanAlgebricXopIdInternal(BI);//check identity element after swap
        BI->swapOperands();//swap back
      }
      if(changed) {
        Inst->eraseFromParent();//identity found, delete the instruction
        result = true;
      }
    }
  }
  return result;
}

//---------------- 2. strength reduction: x*2^k -> x<<k, unsigned x/2^k -> x>>k ----------------
bool nanReduceStrength(BasicBlock &BB)
{
  bool changed = false;
  for (auto I = BB.begin(), End = BB.end()/*init*/; I != End/*termination-condition*/; ) {
    llvm::Instruction *Inst = &*I;//get Instruction from iterator
    I++;//increment the iterator I
    if(BinaryOperator *BI = dyn_cast<BinaryOperator>(Inst)) {//if instruction is a binary operator
      auto op1 = BI->getOperand(0);//operand one
      auto op2 = BI->getOperand(1);//operand two

      auto op2ci = dyn_cast<llvm::ConstantInt>(op2);//type cast operand two to a constant int
      if(!op2ci && BI->isCommutative()) {//constant on the left: 2*c
        BI->swapOperands();//LLVM API Function
        op2 = BI->getOperand(1);
        op1 = BI->getOperand(0);
        op2ci = dyn_cast<llvm::ConstantInt>(op2);
      }
      if(!op2ci) continue;

      auto op2api = op2ci->getValue();//getting value of constant int
      if(!op2api.isPowerOf2())//check whether the value is power of two
        continue;

      int logb2 = op2api.exactLogBase2();//log2 val
      if(logb2 <= 0) continue;//x*1 is not a shift, skip

      Value *finalci = NULL;//llvm::Value class

      switch(BI->getOpcode()) {
      default:
        break;
      case Instruction::Mul:
        /*create a new binary operator: shl*/
        finalci = BinaryOperator::Create(Instruction::Shl/*operator*/, op1/*operand1*/, ConstantInt::get(op1->getType(), logb2)/*operand2*/);
        break;
      case Instruction::UDiv:
        /*unsigned divide -> logical shift right. (SDiv skipped: -3/2=-1 but -3>>1=-2)*/
        finalci = BinaryOperator::Create(Instruction::LShr, op1, ConstantInt::get(op1->getType(), logb2));
        break;
      }

      if(finalci) {//if new instruction is created, insert it, delete old instruction
        Instruction *newinst = dyn_cast<Instruction>(finalci);
        newinst->insertBefore(I);//I points to next instruction after mul, new instruction inserted before I
        Inst->replaceAllUsesWith(finalci);//replace all uses of mul/div with new instruction
        errs()<<"Replacing "<< *Inst <<" with " <<*newinst<<"\n";
        Inst->eraseFromParent();//remove the mul/div from basic block
        changed = true;
      }
    }
  }
  return changed;
}

//---------------- 3. constant folding + constant propagation ----------------
//store 2 -> a ; load a  ==> use 2 directly.   4+2 ==> 6
Value *nanFoldBinaryOp(BinaryOperator *BI)
{
  auto op1ci = dyn_cast<llvm::ConstantInt>(BI->getOperand(0));//type cast operand one to a constant int
  auto op2ci = dyn_cast<llvm::ConstantInt>(BI->getOperand(1));//type cast operand two to a constant int
  if(!op1ci || !op2ci)//both operands must be constants
    return NULL;

  APInt a = op1ci->getValue();//value of operand one
  APInt b = op2ci->getValue();//value of operand two
  APInt res(a.getBitWidth(), 0);//result of folding

  switch(BI->getOpcode()) {//check op code of the operator
  default:
    return NULL;//not handled, don't fold
  case Instruction::Add: res = a + b; break;
  case Instruction::Sub: res = a - b; break;
  case Instruction::Mul: res = a * b; break;
  case Instruction::And: res = a & b; break;
  case Instruction::Or:  res = a | b; break;
  case Instruction::Xor: res = a ^ b; break;
  case Instruction::SDiv:
  case Instruction::SRem:
    if(b.isZero() || (a.isMinSignedValue() && b.isAllOnes()))//x/0 and INT_MIN/-1 are undefined, leave them
      return NULL;
    res = (BI->getOpcode() == Instruction::SDiv) ? a.sdiv(b) : a.srem(b);
    break;
  case Instruction::UDiv:
  case Instruction::URem:
    if(b.isZero())//x/0 is undefined, leave it
      return NULL;
    res = (BI->getOpcode() == Instruction::UDiv) ? a.udiv(b) : a.urem(b);
    break;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    if(b.uge(a.getBitWidth()))//shift by >= bit width is poison, leave it
      return NULL;
    if(BI->getOpcode() == Instruction::Shl) res = a.shl(b);
    else if(BI->getOpcode() == Instruction::LShr) res = a.lshr(b);
    else res = a.ashr(b);
    break;
  }

  return ConstantInt::get(BI->getType(), res);//new constant with the folded value
}

bool nanConstFoldProp(BasicBlock &BB)
{
  bool changed = false;
  DenseMap<AllocaInst *, Constant *> known;//variable -> constant it holds right now

  for (auto I = BB.begin(), End = BB.end()/*init*/; I != End/*termination-condition*/; ) {
    llvm::Instruction *Inst = &*I;//get Instruction from iterator
    I++;//increment the iterator I

    //constant folding: both operands are constants?
    if(BinaryOperator *BI = dyn_cast<BinaryOperator>(Inst)) {
      Value *finalci = nanFoldBinaryOp(BI);
      if(finalci) {//folded, replace uses with the constant and delete the instruction
        errs()<<"Folding "<< *Inst <<" to "<< *finalci <<"\n";
        Inst->replaceAllUsesWith(finalci);
        Inst->eraseFromParent();
        changed = true;
      }
      continue;
    }

    //constant propagation
    if(LoadInst *LI = dyn_cast<LoadInst>(Inst)) {//x = load a
      AllocaInst *AI = nanTrackedPtr(LI->getPointerOperand());
      if(!AI) continue;
      auto It = known.find(AI);
      if(It != known.end() && It->second->getType() == LI->getType()) {//we know a's value
        errs()<<"Propagating constant "<< *It->second <<" into "<< *LI <<"\n";
        LI->replaceAllUsesWith(It->second);
        LI->eraseFromParent();
        changed = true;
      }
    } else if(StoreInst *SI = dyn_cast<StoreInst>(Inst)) {//store v -> a
      AllocaInst *AI = nanTrackedPtr(SI->getPointerOperand());
      if(!AI) continue;
      if(nanIsSimpleConst(SI->getValueOperand()))
        known[AI] = cast<Constant>(SI->getValueOperand());//a now holds a constant
      else
        known.erase(AI);//a holds something unknown
    }
  }
  return changed;
}

//---------------- 4. copy propagation: c = d; e = c + b ==> e = d + b ----------------
bool nanCopyProp(BasicBlock &BB)
{
  bool changed = false;
  DenseMap<Value *, AllocaInst *> loadOf;//%x = load d  -> d
  DenseMap<AllocaInst *, AllocaInst *> copyOf;//c is currently a copy of d

  for (auto I = BB.begin(), End = BB.end(); I != End; ) {
    llvm::Instruction *Inst = &*I;
    I++;

    if(LoadInst *LI = dyn_cast<LoadInst>(Inst)) {
      AllocaInst *AI = nanTrackedPtr(LI->getPointerOperand());
      if(!AI) continue;
      auto It = copyOf.find(AI);
      if(It != copyOf.end()) {//AI is a copy of src, load src instead
        AllocaInst *src = It->second;
        /*create a new load instruction from src*/
        LoadInst *newload = new LoadInst(LI->getType(), src, "", false/*volatile*/, LI->getAlign());
        newload->insertBefore(I);//I points to next instruction after old load, new load inserted before I
        errs()<<"Copy propagation: "<< *LI <<" -> "<< *newload <<"\n";
        LI->replaceAllUsesWith(newload);
        LI->eraseFromParent();
        loadOf[newload] = src;
        changed = true;
      } else {
        loadOf[LI] = AI;
      }
    } else if(StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
      AllocaInst *dst = nanTrackedPtr(SI->getPointerOperand());
      if(!dst) continue;

      //dst is overwritten: forget everything that depended on its old value
      copyOf.erase(dst);
      SmallVector<AllocaInst *, 4> k1;
      for(auto &KV : copyOf) if(KV.second == dst) k1.push_back(KV.first);
      for(auto *K : k1) copyOf.erase(K);
      SmallVector<Value *, 4> k2;
      for(auto &KV : loadOf) if(KV.second == dst) k2.push_back(KV.first);
      for(auto *K : k2) loadOf.erase(K);

      auto It = loadOf.find(SI->getValueOperand());//store (load d) -> c : c is a copy of d
      if(It != loadOf.end() && It->second != dst)
        copyOf[dst] = It->second;
    }
  }
  return changed;
}

//---------------- 5. dead code elimination ----------------
//(a) unused instructions  (b) redundant store: a=3; a=3;
bool nanIsDeadInst(Instruction *Inst)
{
  if(!Inst->use_empty())//result is used somewhere
    return false;
  if(isa<BinaryOperator>(Inst) || isa<CmpInst>(Inst) || isa<CastInst>(Inst) || isa<AllocaInst>(Inst))
    return true;//no side effects, safe to delete
  if(LoadInst *LI = dyn_cast<LoadInst>(Inst))
    return LI->isSimple();//volatile/atomic loads must stay
  return false;//stores, calls, terminators etc. are kept
}

bool nanDCE(BasicBlock &BB)
{
  bool changed = false;
  DenseMap<AllocaInst *, Constant *> known;//variable -> constant last stored to it

  for (auto I = BB.begin(), End = BB.end()/*init*/; I != End/*termination-condition*/; ) {
    llvm::Instruction *Inst = &*I;//get Instruction from iterator
    I++;//increment the iterator I

    if(nanIsDeadInst(Inst)) {//(a) result never used
      errs()<<"DCE removing: "<< *Inst <<"\n";
      Inst->eraseFromParent();
      changed = true;
      continue;
    }

    if(StoreInst *SI = dyn_cast<StoreInst>(Inst)) {//(b)
      AllocaInst *AI = nanTrackedPtr(SI->getPointerOperand());
      if(!AI) continue;
      if(nanIsSimpleConst(SI->getValueOperand())) {
        Constant *C = cast<Constant>(SI->getValueOperand());
        if(known.lookup(AI) == C) {//same constant already stored
          errs()<<"DCE redundant store: "<< *SI <<"\n";
          SI->eraseFromParent();
          changed = true;
        } else {
          known[AI] = C;
        }
      } else {
        known.erase(AI);
      }
    }
  }
  return changed;
}

//(c) variables that are only written, never read. Needs the whole function.
bool nanRemoveWriteOnlyVars(Function &F)
{
  SmallVector<AllocaInst *, 8> dead;
  for(Instruction &I : F.getEntryBlock())
    if(AllocaInst *AI = dyn_cast<AllocaInst>(&I))
      if(nanIsTrackable(AI) && none_of(AI->users(), [](User *U) { return isa<LoadInst>(U); }))
        dead.push_back(AI);

  for(AllocaInst *AI : dead) {
    for(User *U : make_early_inc_range(AI->users()))
      cast<StoreInst>(U)->eraseFromParent();//delete all stores to it
    errs()<<"DCE write-only variable: "<< *AI <<"\n";
    AI->eraseFromParent();
  }
  return !dead.empty();
}

//---------------- driver ----------------
bool nanPerformLocalOpt(BasicBlock &BB)
{
  bool changed = false;
  bool iterChanged = true;

  errs() << "optimizing BB " << BB.getName() << " of function " << BB.getParent()->getName() << "\n";

  //one optimization exposes another (fold -> propagate -> fold ...), so repeat until nothing changes
  while(iterChanged) {
    iterChanged = false;
    iterChanged |= nanConstFoldProp(BB);//constant folding + propagation
    iterChanged |= nanCopyProp(BB);//copy propagation
    iterChanged |= nanAlgebraicXopId(BB);//x+0, x*1
    iterChanged |= nanReduceStrength(BB); // "Reduce strength X* 4 --> shl x, 2
    iterChanged |= nanDCE(BB);//dead code + redundant stores
    changed |= iterChanged;
  }
  return changed;
}

void dumpBasicBlock(BasicBlock &BB, const char *title)
{
  errs() << "=== " << title << " ===" << "\n";

  for(Instruction &I : BB) {
    errs() << I << "\n";
  }

  errs() << "=================" << "\n\n";
}

PreservedAnalyses HelloWorldPass::run(Function &F, FunctionAnalysisManager &AM) {

  bool changed = false;

  bool iterChanged = true;
  while(iterChanged) {
    iterChanged = false;
    for(BasicBlock &BB : F)
      iterChanged |= nanPerformLocalOpt(BB);//Optimization local to basic block
    //removing write-only vars can make more code dead, so loop again
    iterChanged |= nanRemoveWriteOnlyVars(F);
    changed |= iterChanged;
  }

  for(BasicBlock &BB : F)
    dumpBasicBlock(BB, "AFTER  OPT");

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
