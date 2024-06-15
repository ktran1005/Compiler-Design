/*
    Name: Charles Tran (kdt57)
    ID: 14377220
    Instructor: Dr. Naga Kandasamy, Dr. Shihao Song
    Assignment: Optimization – Loop Unrolling
*/
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Passes/PassBuilder.h"
#include <memory>
#include <string>
#include <cstdlib>

using namespace llvm;

/* 
    Unrolling Section - Adjust Loop Control
    - Analyze the exiting block of the loop to identify where the upper bound is set.
    - Modify the upper bound so that it aligns with the unrolling factor.
*/
void adjustLoopControl(Loop *L, size_t unroll_factor, LLVMContext &Context) {
    BasicBlock *ExitingBlock = L->getExitingBlock();
    if (!ExitingBlock) {
        return;
    }
    
    // Find the comparison instruction in the exiting block
    for (Instruction &I : *ExitingBlock) {
        if (CmpInst *CmpI = dyn_cast<CmpInst>(&I)) {
            // Assuming a simple comparison like 'i < EndVal'

            // Check if one of the operands is the loop induction variable
            // and the other is a constant integer
            for (int OpIdx = 0; OpIdx < 2; ++OpIdx) {
                Value *Op = CmpI->getOperand(OpIdx);
                if (auto *CI = dyn_cast<ConstantInt>(Op)) {
                    Value *new_upper_bound = ConstantInt::get(Op->getType(), CI->getSExtValue() / unroll_factor); 
                    CmpI->setOperand(OpIdx, new_upper_bound);
                }
            }
        }
    }
}

/*
    Hints and Tips:
    Use the 'IRBuilder' class from LLVM to facilitate instruction insertion
    The 'Ins.clone()' method is useful for creating a copy of an instruction
    Pay attention to how the operands of cloned instruction might need to be updated to reflect the changes due to unrolling
    Consider how and where the cloned instructions should be inserted back into the loop
*/
void cloneLoopBody(Loop *L, size_t unroll_factor, LLVMContext &Context) {
    BasicBlock *Latch = L->getLoopLatch();
    if (!Latch) {
        errs() << "Could not find latch block.\n";
        return;
    }

    Function *ParentFunction = Latch->getParent();
    
    // Create a new basic block for the cloned body
    BasicBlock *ClonedBody = BasicBlock::Create(Context,
                                                Latch->getName() + ".cloned",
                                                ParentFunction,
                                                Latch);

    IRBuilder<> ClonedBodyBuilder(ClonedBody);
    Value *LastLoadResult = nullptr;
    Value *LastAddResult = nullptr;

    for (size_t i = 0; i < unroll_factor - 1; i++) {
        for (Instruction &Inst : *Latch) {
            Instruction *ClonedInst = Inst.clone();
            
            // TODO: Insert logic to update cloned instruction operands if necessary   
            if (auto *LI = dyn_cast<LoadInst>(ClonedInst)) {
                // record the register of the cloned instruction (load instruction) 
                // i.e %5 = load i32, i32* %1, aligh 4 
                LastLoadResult = ClonedInst;        
            }

            if (auto *AddInst = dyn_cast<BinaryOperator>(ClonedInst)) {
                if (AddInst->getOpcode() == Instruction::Add) {
                    // Override the first operand of the add instruction with the register we recorded previously
                    // %6 = add i32, %11, 1 (originial add instruction) -> %6 = add i32, %5, 1 (modified add instruction)  
                    AddInst->setOperand(0, LastLoadResult);
                    LastAddResult = ClonedInst;
                }
            }

            if (auto *SI = dyn_cast<StoreInst>(ClonedInst)) {
                if (LastAddResult) {
                    // store i32, %12, i32* %1, aligh 4 (original store instruction) -> store i32, %6, i32* %1, aligh 4 (modified store instruction) 
                    SI->setOperand(0, LastAddResult);
                }
            }

            ClonedBodyBuilder.Insert(ClonedInst);
            if (isa<StoreInst>(Inst)) break;
        }
    }

    IRBuilder<> LatchBuilder(Latch);

    // Move the insertion point to just before the terminator instruction of Latch
    // LatchBuilder.SetInsertPoint(Latch->getTerminator());
    LatchBuilder.SetInsertPoint(&Latch->front());

    // Move instructions from ClonedBody to Latch
    while (!ClonedBody->empty()) {
        Instruction &Inst = ClonedBody->front();
        Inst.removeFromParent();
        LatchBuilder.Insert(&Inst);
    }
    
    ClonedBody->eraseFromParent();
}

void unrollLoop(Loop *L, size_t unroll_factor, LLVMContext &Context) 
{
    if (unroll_factor == 0)
        return;

    adjustLoopControl(L, unroll_factor, Context);
    cloneLoopBody(L, unroll_factor, Context);
}

void opt(Module &M, size_t unroll_factor) 
{
    // Create the analysis managers
    FunctionAnalysisManager FAM;
    LoopAnalysisManager LAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    // Create the pass builder and register analysis managers
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);


    for (Function &F : M) 
    {
        if (!F.isDeclaration()) 
        {
            // Run loop analysis
            LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);

            for (auto *L : LI) 
            {
                unrollLoop(L, unroll_factor, F.getContext());
            }
        }
    }

}


int main(int argc, char **argv) {
    if (argc < 4) {
        errs() << "Usage: " << argv[0] << " <input.bc> <output.bc> <unroll_factor>\n";
        return 1;
    }

    LLVMContext Context;
    SMDiagnostic Err;

    // Read the input bitcode
    std::unique_ptr<Module> M = parseIRFile(argv[1], Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }
    errs() << "\n******************* Original IR ******************* \n";
    M->print(errs(), nullptr);
    errs() << "*************************************************** \n";

    // Identify loops
    opt(*M, std::stoi(argv[3]));

    errs() << "\n********************* New IR ********************** \n";
    M->print(errs(), nullptr);
    errs() << "*************************************************** \n";
    opt(*M, 0);

    // Write the (unmodified) module to the output bitcode file
    std::error_code EC;
    raw_fd_ostream OS(argv[2], EC);
    if (EC) {
        errs() << "Error opening file " << argv[2] << ": " << EC.message() << "\n";
        return 1;
    }
    WriteBitcodeToFile(*M, OS);

    return 0;
}
