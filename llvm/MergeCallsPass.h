#ifndef MERGE_CALLS_PASS_H
#define MERGE_CALLS_PASS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include <map>
#include <vector>

using namespace llvm;

namespace {
struct MergeCalls : public FunctionPass {
  static char ID;
  MergeCalls() : FunctionPass(ID) {}

  /// Get a/the BasicBlock containing an unreachable instruction.
  static BasicBlock *getUnreachableBlock(Function *F) {
    for (BasicBlock &BB : *F) {
      if (BB.getInstList().size() == 1) {
        // Single-instruction block, let's see if it only consists of a
        // unreachable instruction.
        const Instruction *instr = BB.getFirstNonPHI();
        assert(instr && "basic block contains no non-phi instructions");

        if (isa<UnreachableInst>(instr)) {
          // We only have a single instruction, and it's an "unreachable".
          // Return the basic block it's in.
          return &BB;
        }
      }
    }

    // No 'unreachable' basic block. Let's build our own.
    BasicBlock *unreachableBlock =
        BasicBlock::Create(F->getContext(), "", F, nullptr);
    new UnreachableInst(F->getContext(), unreachableBlock);

    return unreachableBlock;
  }

  /// Modify function F, merging all calls to function Target down to a single
  /// instruction, thereby making Target easier to inline into F.
  ///
  /// \param F the function to modify
  /// \param Target the target function, called by F, to be called only once
  ///
  /// \returns the only CallInst that calls Target. If Target isn't called by F,
  /// nullptr.
  static CallInst *mergeCallSites(Function *F, Function *Target) {
    std::vector<CallInst *> CallSites;

    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *C = dyn_cast<CallInst>(&I)) {
          if (!C->isInlineAsm() && C->getCalledFunction() == Target) {
            CallSites.push_back(C);
          }
        }
      }
    }

    // If T isn't actually called, do nothing.
    if (!CallSites.size())
      return nullptr;

    // Also do nothing if T is already called only once.
    if (CallSites.size() == 1)
      return CallSites[0];

    std::vector<Value *> CallArgs;
    std::map<BasicBlock *, BasicBlock *> CallSiteToRet;
    std::map<Instruction *, BasicBlock *> CallSiteToOrigParent;
    BasicBlock *CallBlock = BasicBlock::Create(F->getContext(), "", F, nullptr);

    for (CallInst *C : CallSites) {
      BasicBlock *ParentBlock = C->getParent();
      BasicBlock *ReturnBlock =
          ParentBlock->splitBasicBlock(C->getNextNode(), "");
      CallSiteToOrigParent[C] = ParentBlock;
      CallSiteToRet[ParentBlock] = ReturnBlock;

      // Move the call instruction to the beginning of the return block
      // (before the first non-PHI instruction).
      C->moveBefore(ReturnBlock->getFirstNonPHI());

      // Generate a branch to our call block and get rid of the branch
      // generated by splitBasicBlock.
      BranchInst *B = BranchInst::Create(CallBlock, ParentBlock);
      B->getPrevNode()->eraseFromParent();
    }

    if (Target->arg_size() > 0) {
      // We have to create a PHI node for each incoming basic block/value
      // pair.
      int argCtr = 0;
      for (Argument &A : Target->args()) {
        PHINode *ArgNode =
            PHINode::Create(A.getType(), CallSites.size(), "", CallBlock);
        for (CallInst *C : CallSites) {
          ArgNode->addIncoming(C->getArgOperand(argCtr),
                               CallSiteToOrigParent[C]);
        }

        CallArgs.push_back(cast<Value>(ArgNode));
        ++argCtr;
      }
    }

    // This is the actual call to the target
    CallInst *UnifiedCall = CallInst::Create(
        cast<Value>(Target), ArrayRef<Value *>(CallArgs), "", CallBlock);

    for (CallInst *C : CallSites) {
      // Get rid of the original call, replace all references to it with the
      // call in our call block.
      C->replaceAllUsesWith(UnifiedCall);
      C->eraseFromParent();
    }

    // Emit PHI/switch instructions for branching back to the return blocks:
    PHINode *WhereFromNode = PHINode::Create(Type::getInt32Ty(F->getContext()),
                                             CallSites.size(), "", UnifiedCall);
    // Our default is gonna be a basic block that only contains an
    // 'unreachable' instruction, because we're accounting for every case.
    SwitchInst *SwitchBackInstr = SwitchInst::Create(
        WhereFromNode, getUnreachableBlock(F), CallSiteToRet.size(), CallBlock);

    int switchCtr = 0;
    for (auto &KV : CallSiteToRet) {
      llvm::ConstantInt *BranchIdx = llvm::ConstantInt::get(
          F->getContext(), llvm::APInt(32, switchCtr++, true));
      WhereFromNode->addIncoming(BranchIdx, KV.first);
      SwitchBackInstr->addCase(BranchIdx, KV.second);
    }

    return UnifiedCall;
  }

  bool runOnFunction(Function &F) override {
    std::map<Function *, std::vector<CallInst *>> FuncToInvokers;
    bool Modified = false;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (isa<CallInst>(I)) {
          CallInst &C = cast<CallInst>(I);
          if (C.isInlineAsm()) {
            // This is inline assembly; this can be deduplicated by a
            // different pass if necessary. It doesn't call anything.
            continue;
          }
          if (C.getCalledFunction() == nullptr) {
            // Indirect invocation (call-by-ptr). Skip for now.
            continue;
          }
          if (C.getCalledFunction()->isIntrinsic()) {
            // LLVM intrinsic - don't tamper with this!
            continue;
          }

          FuncToInvokers[C.getCalledFunction()].push_back(&C);
        }
      }
    }

    for (auto &KV : FuncToInvokers) {
      Function *Target = KV.first;
      std::vector<CallInst *> &CallSites = KV.second;
      if (CallSites.size() > 1) {
        mergeCallSites(&F, Target);
        Modified = true;
      }
    }

    // Finally, apply necessary fix-ups to stack.
    if (Modified) {
      FunctionPass *reg2mem = llvm::createDemoteRegisterToMemoryPass();
      reg2mem->runOnFunction(F);
    }

    return Modified;
  }
}; // end of struct MergeCalls
} // end of anonymous namespace

char MergeCalls::ID = 0;
#endif
