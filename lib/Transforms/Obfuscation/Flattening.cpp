#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/CryptoUtils.h"

#define DEBUG_TYPE "flattening"

using namespace llvm;

// Stats
STATISTIC(Flattened, "Functions flattened");

static cl::opt<string> FunctionName(
        "funcFLA", cl::init(""),
        cl::desc(
                "Flatten only certain functions: -mllvm -funcFLA=\"func1,func2\""));

static cl::opt<int> Percentage(
        "perFLA", cl::init(100),
        cl::desc("Flatten only a certain percentage of functions"));

namespace {
    struct Flattening : public FunctionPass {
        static char ID;  // Pass identification, replacement for typeid
        bool flag;

        Flattening() : FunctionPass(ID) {}

        Flattening(bool flag) : FunctionPass(ID) {
            this->flag = flag;
            // Check if the number of applications is correct
            if (!((Percentage > 0) && (Percentage <= 100))) {
                LLVMContext ctx;
                ctx.emitError(Twine("Flattening application function percentage -perFLA=x must be 0 < x <= 100"));
            }
        }

        bool runOnFunction(Function &F);

        bool flatten(Function *f);
    };
}

char Flattening::ID = 0;
static RegisterPass<Flattening> X("flattening", "Call graph flattening");

Pass *llvm::createFlattening(bool flag) { return new Flattening(flag); }

bool Flattening::runOnFunction(Function &F) {
    Function *tmp = &F;

    // Do we obfuscate
    if (toObfuscate(flag, tmp, "fla") && ((int) llvm::cryptoutils->get_range(100) <= Percentage)) {
        //errs() << "fla " + F.getName() +"\n";
        if (flatten(tmp)) {
            ++Flattened;
        }
    }

    return false;
}

bool Flattening::flatten(Function *f) {
    vector<BasicBlock *> origBB;
    BasicBlock *loopEntry;
    BasicBlock *loopEnd;
    LoadInst *load;
    SwitchInst *switchI;
    AllocaInst *switchVar;

    // SCRAMBLER
    char scrambling_key[16];
    llvm::cryptoutils->get_bytes(scrambling_key, 16);
    // END OF SCRAMBLER

    // Lower switch
    FunctionPass *lower = createLowerSwitchPass();
    lower->runOnFunction(*f);

    std::vector<BasicBlock *> needSplite;
    unsigned instSum = 0;
    for ( Function::iterator I = std::next(f->begin()); I != f->end(); I++ ) {
        instSum += I->size();
    }
    for ( Function::iterator I = std::next(f->begin()); I != f->end(); I++ ) {
        if ( I->size() > instSum / f->size() ) {
            needSplite.push_back(&*I);
        }
    }
    for ( auto I = needSplite.begin(); I != needSplite.end(); I++ ) {
        //std::string tw = "split_" + std::to_string(++index);
        BasicBlock::iterator j = (*I)->begin();
        for ( unsigned step = 0; step < (*I)->size() / 2; step++ ) {
            j++;
        }
        BasicBlock *next = (*I)->splitBasicBlock(j/*, tw*/);

        Instruction *back = &(*I)->back();
        if ( isa<BranchInst>(back) ) {
            BranchInst *BI = dyn_cast<BranchInst>(back);
            BI->eraseFromParent();
            AllocaInst *tmpBool = new AllocaInst(Type::getInt32Ty(f->getContext()), 0, "tmp", *I);
            new StoreInst(
                    ConstantInt::get(Type::getInt32Ty(f->getContext()), 1), tmpBool, *I);
            LoadInst *loadInst = new LoadInst(tmpBool, "tmp", *I);
            ICmpInst *ICmp = new ICmpInst(**I, ICmpInst::ICMP_EQ, loadInst, loadInst);
            BranchInst::Create(next, (*I)->getPrevNode(), ICmp, *I);
        }

        (*I)->getPrevNode()->dump();
        (*I)->dump();
        next->dump();
    }

    // Save all original BB
    for (Function::iterator i = f->begin(); i != f->end(); ++i) {
        BasicBlock *tmp = &*i;
        origBB.push_back(tmp);

        BasicBlock *bb = &*i;
        if (isa<InvokeInst>(bb->getTerminator())) {
            return false;
        }
    }

    // Nothing to flatten
    if (origBB.size() <= 1) {
        return false;
    }

    // Remove first BB
    origBB.erase(origBB.begin());

    // Get a pointer on the first BB
    Function::iterator tmp = f->begin();  //++tmp;
    BasicBlock *insert = &*tmp;

    // If main begin with an if
    BranchInst *br = NULL;
    if (isa<BranchInst>(insert->getTerminator())) {
        br = cast<BranchInst>(insert->getTerminator());
    }

    if ((br != NULL && br->isConditional()) ||
        insert->getTerminator()->getNumSuccessors() > 1) {
        BasicBlock::iterator i = insert->back().getIterator();

        if (insert->size() > 1) {
            i--;
        }

        BasicBlock *tmpBB = insert->splitBasicBlock(i, "first");
        origBB.insert(origBB.begin(), tmpBB);
    }

    // Remove jump
    insert->getTerminator()->eraseFromParent();

    // Create switch variable and set as it
    switchVar =
            new AllocaInst(Type::getInt32Ty(f->getContext()), 0, "switchVar", insert);
    new StoreInst(
            ConstantInt::get(Type::getInt32Ty(f->getContext()),
                             llvm::cryptoutils->scramble32(0, scrambling_key)),
            switchVar, insert);

    // Create main loop
    loopEntry = BasicBlock::Create(f->getContext(), "loopEntry", f, insert);
    loopEnd = BasicBlock::Create(f->getContext(), "loopEnd", f, insert);

    load = new LoadInst(switchVar, "switchVar", loopEntry);

    // Move first BB on top
    insert->moveBefore(loopEntry);
    BranchInst::Create(loopEntry, insert);

    // loopEnd jump to loopEntry
    BranchInst::Create(loopEntry, loopEnd);

    BasicBlock *swDefault =
            BasicBlock::Create(f->getContext(), "switchDefault", f, loopEnd);
    BranchInst::Create(loopEnd, swDefault);

    // Create switch instruction itself and set condition
    switchI = SwitchInst::Create(&*(f->begin()), swDefault, 0, loopEntry);
    switchI->setCondition(load);

    // Remove branch jump from 1st BB and make a jump to the while
    f->begin()->getTerminator()->eraseFromParent();

    BranchInst::Create(loopEntry, &*(f->begin()));

    // Put all BB in the switch
    for (vector<BasicBlock *>::iterator b = origBB.begin(); b != origBB.end();
         ++b) {
        BasicBlock *i = *b;
        ConstantInt *numCase = NULL;

        // Move the BB inside the switch (only visual, no code logic)
        i->moveBefore(loopEnd);

        // Add case to switch
        numCase = cast<ConstantInt>(ConstantInt::get(
                switchI->getCondition()->getType(),
                llvm::cryptoutils->scramble32(switchI->getNumCases(), scrambling_key)));
        switchI->addCase(numCase, i);
    }

    // Recalculate switchVar
    for (vector<BasicBlock *>::iterator b = origBB.begin(); b != origBB.end();
         ++b) {
        BasicBlock *i = *b;
        ConstantInt *numCase = NULL;

        // Ret BB
        if (i->getTerminator()->getNumSuccessors() == 0) {
            continue;
        }

        // If it's a non-conditional jump
        if (i->getTerminator()->getNumSuccessors() == 1) {
            // Get successor and delete terminator
            BasicBlock *succ = i->getTerminator()->getSuccessor(0);
            i->getTerminator()->eraseFromParent();

            // Get next case
            numCase = switchI->findCaseDest(succ);

            // If next case == default case (switchDefault)
            if (numCase == NULL) {
                numCase = cast<ConstantInt>(
                        ConstantInt::get(switchI->getCondition()->getType(),
                                         llvm::cryptoutils->scramble32(
                                                 switchI->getNumCases() - 1, scrambling_key)));
            }

            // Update switchVar and jump to the end of loop
            new StoreInst(numCase, load->getPointerOperand(), i);
            BranchInst::Create(loopEnd, i);
            continue;
        }

        // If it's a conditional jump
        if (i->getTerminator()->getNumSuccessors() == 2) {
            // Get next cases
            ConstantInt *numCaseTrue =
                    switchI->findCaseDest(i->getTerminator()->getSuccessor(0));
            ConstantInt *numCaseFalse =
                    switchI->findCaseDest(i->getTerminator()->getSuccessor(1));

            // Check if next case == default case (switchDefault)
            if (numCaseTrue == NULL) {
                numCaseTrue = cast<ConstantInt>(
                        ConstantInt::get(switchI->getCondition()->getType(),
                                         llvm::cryptoutils->scramble32(
                                                 switchI->getNumCases() - 1, scrambling_key)));
            }

            if (numCaseFalse == NULL) {
                numCaseFalse = cast<ConstantInt>(
                        ConstantInt::get(switchI->getCondition()->getType(),
                                         llvm::cryptoutils->scramble32(
                                                 switchI->getNumCases() - 1, scrambling_key)));
            }

            // Create a SelectInst
            BranchInst *br = cast<BranchInst>(i->getTerminator());
            SelectInst *sel =
                    SelectInst::Create(br->getCondition(), numCaseTrue, numCaseFalse, "",
                                       i->getTerminator());

            // Erase terminator
            i->getTerminator()->eraseFromParent();

            // Update switchVar and jump to the end of loop
            new StoreInst(sel, load->getPointerOperand(), i);
            BranchInst::Create(loopEnd, i);
            continue;
        }
    }

    fixStack(f);

    return true;
}
