/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */

#include <iostream>
#include <vector>

#include <llvm/Support/raw_ostream.h>
#include <llvm/Linker.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Pass.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/IR/Instruction.h>

#include "llvm_taint_lib.h"
#include "guestarch.h"
#include "my_mem.h"
#include "taint2.h"

extern "C" {
#include "libgen.h"
}

extern char *qemu_loc;

using namespace llvm;
using std::vector;
using std::pair;

/***
 *** PandaTaintFunctionPass
 ***/

char PandaTaintFunctionPass::ID = 0;
//static RegisterPass<PandaTaintFunctionPass>
//X("PandaTaint", "Analyze each instruction in a function for taint operations");

static inline ConstantInt *const_uint64(LLVMContext &C, uint64_t val) {
    return ConstantInt::get(Type::getInt64Ty(C), val);
}

static inline ConstantInt *const_uint64_ptr(LLVMContext &C, void *ptr) {
    return ConstantInt::get(Type::getInt64Ty(C), (uint64_t)ptr);
}

static inline Constant *const_i64p(LLVMContext &C, void *ptr) {
    return ConstantExpr::getIntToPtr(const_uint64_ptr(C, ptr),
            Type::getInt64PtrTy(C));
}

bool PandaTaintFunctionPass::doInitialization(Module &M) {
    // Add taint functions to module
    char *exe = strdup(qemu_loc);
    std::string bitcode(dirname(exe));
    free(exe);
    bitcode.append("/panda_plugins/panda_taint2_ops.bc");
    std::cerr << "Linking taint ops from " << bitcode << std::endl;

    LLVMContext &ctx = M.getContext();
    SMDiagnostic Err;
    Module *taintopmod(ParseIRFile(bitcode, Err, ctx));
    if (!taintopmod) {
        Err.print("qemu", llvm::errs());
        return false;
    }

    MDNode *md = MDNode::get(ctx, ArrayRef<Value *>());
    for (auto it = taintopmod->begin(); it != taintopmod->end(); it++) {
        if (it->size() == 0) continue;
        if (it->front().size() == 0) continue;
        it->front().front().setMetadata("tainted", md);
    }

    std::string err;
    Linker::LinkModules(&M, taintopmod, Linker::DestroySource, &err);
    if (!err.empty()) {
        std::cerr << err << std::endl;
        return false;
    }
    verifyModule(M, llvm::AbortProcessAction, &err);
    if (!err.empty()) {
        std::cerr << err << std::endl;
        return true;
    }

    PTV.deleteF = M.getFunction("taint_delete"),
    PTV.mixF = M.getFunction("taint_mix"),
    PTV.mixCompF = M.getFunction("taint_mix_compute"),
    PTV.parallelCompF = M.getFunction("taint_parallel_compute"),
    PTV.copyF = M.getFunction("taint_copy");
    PTV.moveF = M.getFunction("taint_move");
    PTV.sextF = M.getFunction("taint_sext");
    PTV.selectF = M.getFunction("taint_select");
    PTV.hostCopyF = M.getFunction("taint_host_copy");
    PTV.pushFrameF = M.getFunction("taint_push_frame");
    PTV.popFrameF = M.getFunction("taint_pop_frame");
    PTV.breadcrumbF = M.getFunction("taint_breadcrumb");

    PTV.llvConst = const_uint64_ptr(ctx, shad->llv);
    PTV.memConst = const_uint64_ptr(ctx, shad->ram);
    PTV.grvConst = const_uint64_ptr(ctx, shad->grv);
    PTV.gsvConst = const_uint64_ptr(ctx, shad->gsv);
    PTV.retConst = const_uint64_ptr(ctx, shad->ret);

    PTV.dataLayout = new DataLayout(&M);

    PTV.memlogPopF = M.getFunction("taint2_memlog_pop");
    PTV.memlogConst = const_uint64_ptr(ctx, memlog);

    PTV.prevBbConst = const_i64p(ctx, &shad->prev_bb);

    return true;
}

bool PandaTaintFunctionPass::runOnFunction(Function &F) {
#ifdef TAINTDEBUG
    printf("\n\n%s\n", F.getName().str().c_str());
#endif
    if (F.front().front().getMetadata("tainted") ||
            F.getName().startswith("taint")) { // already processed!!
        return false;
    }
    //printf("Processing entry BB...\n");
    PTV.visit(F);
    F.dump();

    return true;
}

/***
 *** PandaSlotTracker
 ***/

void PandaSlotTracker::initialize() {
    if (TheFunction && !FunctionProcessed) {
        processFunction();
    }
}

void PandaSlotTracker::processFunction() {
    // Add arguments without names
    // We make sure that arguments have 
    for(Function::arg_iterator AI = TheFunction->arg_begin(),
        AE = TheFunction->arg_end(); AI != AE; ++AI) {
        if (!AI->hasName()) {
            CreateFunctionSlot(AI);
        }
        else {
            AI->setName("");
            CreateFunctionSlot(AI);
        }
    }
    // Add all of the basic blocks and instructions with no names.
    for (Function::iterator BB = TheFunction->begin(),
        E = TheFunction->end(); BB != E; ++BB) {
        if (!BB->hasName()) {
            CreateFunctionSlot(BB);
        }
        else {
            // the naming of the 'entry' BB happens by default, so leave it
            if (strcmp(BB->getName().str().c_str(), "entry")) {
                BB->setName("");
                CreateFunctionSlot(BB);
            }
        }
        for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E;
            ++I) {
            if (I->getType() != Type::getVoidTy(TheFunction->getContext()) &&
                !I->hasName()) {
                CreateFunctionSlot(I);
            }
            else if (I->getType() != Type::getVoidTy(TheFunction->getContext())
                && I->hasName()) {
                I->setName("");
                CreateFunctionSlot(I);
            }

            // We currently are assuming no metadata, but we will need this if
            // we start using metadata
            /*for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i) {
                if (MDNode *N = dyn_cast_or_null<MDNode>(I->getOperand(i))) {
                    CreateMetadataSlot(N);
                }
            }*/
        }
    }
    FunctionProcessed = true;
}

void PandaSlotTracker::CreateFunctionSlot(const Value *V) {
    assert(V->getType() != Type::getVoidTy(TheFunction->getContext()) &&
        !V->hasName() && "Doesn't need a slot!");
    unsigned DestSlot = fNext++;
    fMap[V] = DestSlot;
}

//void PandaSlotTracker::CreateMetadataSlot(const MDNode *N) {
    // don't currently need this, but we will if we start using metadata
//}

int PandaSlotTracker::getLocalSlot(const Value *V) {
    ValueMap::iterator FI = fMap.find(V);
    return FI == fMap.end() ? -1 : (int)FI->second;
}

/***
 *** PandaTaintVisitor
 ***/

/*
 * Returns size in bytes of a generic LLVM value (could be operand or
 * instruction).
 */
unsigned PandaTaintVisitor::getValueSize(Value *V) {
    uint64_t size = dataLayout->getTypeSizeInBits(V->getType());
    return (size < 8) ? 1 : size / 8;
}

void PandaTaintVisitor::inlineCallAfter(Instruction &I, Function *F, vector<Value *> &args) {
    assert(F);
    CallInst *CI = CallInst::Create(F, args);
    if (!CI) {
        printf("Couldn't create call inst!!\n");
    }
    CI->insertAfter(&I);

    if (F->size() == 1) { // no control flow
        // Inline.
#ifndef TAINT2_NOINLINE
        InlineFunctionInfo IFI;
        if (!InlineFunction(CI, IFI)) {
            printf("Inlining failed!\n");
        }
#endif
    }
}

void PandaTaintVisitor::inlineCallBefore(Instruction &I, Function *F, vector<Value *> &args) {
    assert(F);
    CallInst *CI = CallInst::Create(F, args);
    if (!CI) {
        printf("Couldn't create call inst!!\n");
    }
    CI->insertBefore(&I);

    if (F->size() == 1) { // no control flow
        // Inline.
#ifndef TAINT2_NOINLINE
        InlineFunctionInfo IFI;
        if (!InlineFunction(CI, IFI)) {
            printf("Inlining failed!\n");
        }
#endif
    }
}

Constant *PandaTaintVisitor::constSlot(LLVMContext &ctx, Value *value) {
    assert(value && !isa<Constant>(value));
    return const_uint64(ctx, MAXREGSIZE * PST->getLocalSlot(value));
}

Constant *PandaTaintVisitor::constWeakSlot(LLVMContext &ctx, Value *value) {
    int slot = PST->getLocalSlot(value);
    uint64_t result = MAXREGSIZE * slot;
    if (slot < 0) {
        result = ~0UL;
    }
    return const_uint64(ctx, MAXREGSIZE * result);
}

int PandaTaintVisitor::intValue(Value *value) {
    ConstantInt *CI;
    if ((CI = dyn_cast<ConstantInt>(value))) {
        return CI->getZExtValue();
    } else return -1;
}

void PandaTaintVisitor::visitFunction(Function& F) {
    LLVMContext &ctx = F.getContext();
    // Two things: Insert "tainted" metadata.
    MDNode *md = MDNode::get(ctx, ArrayRef<Value *>());

    // create slot tracker to keep track of LLVM values
    PST.reset(new PandaSlotTracker(&F));
    PST->initialize();

    // Insert call to clear llvm shadow mem.
    vector<Value *> args{
        llvConst, const_uint64(ctx, 0),
        const_uint64(ctx, MAXREGSIZE * shad->num_vals)
    };
    assert(F.front().getFirstNonPHI() != NULL);
    inlineCallBefore(*F.front().getFirstNonPHI(), deleteF, args);
    F.front().front().setMetadata("tainted", md);
}

void PandaTaintVisitor::visitBasicBlock(BasicBlock &BB) {
    // At end of BB, log where we just were.
    LLVMContext &ctx = BB.getContext();
    vector<Value *> args{
        const_uint64_ptr(ctx, &shad->prev_bb), constSlot(ctx, &BB)
    };
    assert(BB.getTerminator() != NULL);
    inlineCallBefore(*BB.getTerminator(), breadcrumbF, args);
}

// Insert a log pop after this instruction.
CallInst *PandaTaintVisitor::insertLogPop(Instruction &after) {
    vector<Value *> args{ memlogConst };
    CallInst *CI = CallInst::Create(memlogPopF, args);
    if (!CI) {
        printf("Couldn't create call inst!!\n");
    }
    CI->insertAfter(&after);
    return CI;
}

void PandaTaintVisitor::insertTaintMove(Instruction &I,
        Constant *shad_dest, Value *dest, Constant *shad_src, Value *src,
        uint64_t size) {
    insertTaintBulk(I, shad_dest, dest, shad_src, src, size, moveF);
}

void PandaTaintVisitor::insertTaintCopy(Instruction &I,
        Constant *shad_dest, Value *dest, Constant *shad_src, Value *src,
        uint64_t size) {
    insertTaintBulk(I, shad_dest, dest, shad_src, src, size, copyF);
}

void PandaTaintVisitor::insertTaintBulk(Instruction &I,
        Constant *shad_dest, Value *dest, Constant *shad_src, Value *src,
        uint64_t size, Function *func) {
    LLVMContext &ctx = I.getContext();
    CallInst *srcCI = NULL, *destCI = NULL;
    if (!src) { // grab from memlog. Src will be below dest.
        assert(shad_src == memConst);
        src = (srcCI = insertLogPop(I));
    }
    if (!dest) { // grab from memlog. Dest will be on top of stack.
        assert(shad_dest == memConst);
        dest = (destCI = insertLogPop(I));
    }
    // If these are llvm regs we have to interpret them as slots.
    if (shad_dest == llvConst) dest = constSlot(ctx, dest);
    if (shad_src == llvConst) src = constSlot(ctx, src);

    vector<Value *> args{ shad_dest, dest, shad_src, src, const_uint64(ctx, size) };
    Instruction *after = srcCI ? srcCI : (destCI ? destCI : &I);
    inlineCallAfter(*after, func, args);

#ifndef TAINT2_NOINLINE
    InlineFunctionInfo IFI;
    if (destCI && !InlineFunction(destCI, IFI)) {
        printf("Inlining failed!\n");
    }
    if (srcCI && !InlineFunction(srcCI, IFI)) {
        printf("Inlining failed!\n");
    }
#endif
}

void PandaTaintVisitor::insertTaintMix(Instruction &I, Value *src) {
    insertTaintMix(I, &I, src);
}
void PandaTaintVisitor::insertTaintMix(Instruction &I, Value *dest, Value *src) {
    LLVMContext &ctx = I.getContext();
    if (isa<Constant>(src)) return;

    if (!dest) dest = &I;
    Constant *dest_size = const_uint64(ctx, getValueSize(dest));
    Constant *src_size = const_uint64(ctx, getValueSize(src));

    vector<Value *> args{
        llvConst, constSlot(ctx, dest), dest_size, constSlot(ctx, src), src_size
    };
    inlineCallAfter(I, mixF, args);
}

void PandaTaintVisitor::insertTaintCompute(Instruction &I, Value *src1, Value *src2, bool is_mixed) {
    insertTaintCompute(I, &I, src1, src2, is_mixed);
}

// Compute operations
void PandaTaintVisitor::insertTaintCompute(Instruction &I, Value *dest, Value *src1, Value *src2, bool is_mixed) {
    LLVMContext &ctx = I.getContext();
    if (!dest) dest = &I;

    if (isa<Constant>(src1) && isa<Constant>(src2)) {
        return; // do nothing.
    } else if (isa<Constant>(src1) || isa<Constant>(src2)) {
        Value *tainted = isa<Constant>(src1) ? src2 : src1;
        if (is_mixed) {
            insertTaintMix(I, tainted);
        } else {
            insertTaintCopy(I, llvConst, dest, llvConst, tainted, getValueSize(src2));
        }
        return;
    }

    if (!is_mixed) {
        assert(getValueSize(dest) == getValueSize(src1));
    }
    assert(getValueSize(src1) == getValueSize(src1));

    Constant *dest_size = const_uint64(ctx, getValueSize(dest));
    Constant *src_size = const_uint64(ctx, getValueSize(src1));

    vector<Value *> args{
        llvConst, constSlot(ctx, dest), dest_size,
        constSlot(ctx, src1), constSlot(ctx, src2), src_size
    };
    inlineCallAfter(I, is_mixed ? mixCompF : parallelCompF, args);
}

void PandaTaintVisitor::insertTaintSext(Instruction &I, Value *src) {
    LLVMContext &ctx = I.getContext();
    Value *dest = &I;
    Constant *dest_size = const_uint64(ctx, getValueSize(dest));
    Constant *src_size = const_uint64(ctx, getValueSize(src));

    vector<Value *> args{
        llvConst, constSlot(ctx, dest), dest_size, constSlot(ctx, src), src_size
    };
    inlineCallAfter(I, sextF, args);
}

void PandaTaintVisitor::insertTaintSelect(Instruction &after, Value *dest,
        Value *selector, vector<pair<Value *, Value *>> &selections) {
    LLVMContext &ctx = after.getContext();
    Constant *dest_size = const_uint64(ctx, getValueSize(dest));

    vector<Value *> args{
        llvConst, constSlot(ctx, dest), dest_size, selector
    };
    for (auto &selection : selections) {
        args.push_back(selection.first);
        args.push_back(selection.second);
    }
    args.push_back(const_uint64(ctx, 0));
    inlineCallAfter(after, selectF, args);
}

void PandaTaintVisitor::insertTaintDelete(Instruction &I,
        Constant *shad, Value *dest, Value *size) {
    if (shad == llvConst) dest = constSlot(I.getContext(), dest);
    vector<Value *> args{ shad, dest, size };
    inlineCallAfter(I, deleteF, args);
}

// Terminator instructions
void PandaTaintVisitor::visitReturnInst(ReturnInst &I) {
    Value *ret = I.getReturnValue();
    if (!ret) return;

    LLVMContext &ctx = I.getContext();
    vector<Value *> args{
        retConst, const_uint64(ctx, 0),
        llvConst, const_uint64(ctx, PST->getLocalSlot(ret)),
        const_uint64(ctx, getValueSize(ret))
    };
    inlineCallBefore(I, copyF, args);

    visitTerminatorInst(I);
}

// On a branch we just have to log the previous BB.
void PandaTaintVisitor::visitTerminatorInst(TerminatorInst &I) {
    // FIXME: Insert BB logging.
}

void PandaTaintVisitor::visitInvokeInst(InvokeInst &I) {
    assert(false && "Can't handle invoke!!");
}

/*
 * Treat unreachable the same way as return.  This matters, for example, when
 * there is a call to cpu_loop_exit() in a helper function, followed by an
 * unreachable instruction.  Functions that end with unreachable return void, so
 * we don't have to worry about taint transfer.
 */
void PandaTaintVisitor::visitUnreachableInst(UnreachableInst &I) {}

// Binary operators
void PandaTaintVisitor::visitBinaryOperator(BinaryOperator &I) {
    bool is_mixed = false;
    switch (I.getOpcode()) {
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Mul:
        case Instruction::UDiv:
        case Instruction::SDiv:
        case Instruction::FAdd:
        case Instruction::FSub:
        case Instruction::FMul:
        case Instruction::FDiv:
        case Instruction::URem:
        case Instruction::SRem:
        case Instruction::FRem:
        case Instruction::Shl:
        case Instruction::LShr:
        case Instruction::AShr:
            is_mixed = true;
            break;
            // mixed

        case Instruction::And:
        case Instruction::Or:
        case Instruction::Xor:
            is_mixed = false;
            break;
            // parallel

        default:
            assert(false && "Bad BinaryOperator!!");
    }

    insertTaintCompute(I, &I, I.getOperand(0), I.getOperand(1), is_mixed);
}

// Memory operators

// Do nothing.
void PandaTaintVisitor::visitAllocaInst(AllocaInst &I) {}

// Find address and constant given a load/store (i.e. host vmem) address.
// Argument should be an inttoptr instruction.
bool PandaTaintVisitor::getAddr(Value *addrVal, Addr& addrOut) {
    IntToPtrInst *I2PI;
    GetElementPtrInst *GEPI;
    BinaryOperator *AI;
    int offset = -1;
    // Structure produced by code gen should always be inttoptr(add(env_v, off)).
    // Helper functions are GEP's.
    if ((I2PI = dyn_cast<IntToPtrInst>(addrVal)) != NULL) {
        assert(I2PI->getOperand(0));
        if ((AI = dyn_cast<BinaryOperator>(I2PI->getOperand(0))) == NULL) return false;

        assert(AI->getOperand(0) && AI->getOperand(1));
        // This gets erased by the slot tracker. Never mind.
        //if (AI->getOperand(0)->getName() != "env_v") return false;
        if (PST->getLocalSlot(AI->getOperand(0)) != 0) return false;

        offset = intValue(AI->getOperand(1));
    } else if ((GEPI = dyn_cast<GetElementPtrInst>(addrVal)) != NULL) {
        // unsupported as of yet.
        // this happens in helper functions.
        return false;
    } else {
        return false;
    }
    if (offset < 0 || (unsigned)offset >= sizeof(CPUState)) return false;

#define m_off(member) (uint64_t)(&((CPUState *)0)->member)
#define m_size(member) sizeof(((CPUState *)0)->member)
#define m_endoff(member) (m_off(member) + m_size(member))
#define contains_offset(member) ((signed)m_off(member) <= (offset) && (unsigned)(offset) < m_endoff(member))
    if (contains_offset(regs)) {
        addrOut.typ = GREG;
        addrOut.val.gr = (offset - m_off(regs)) / m_size(regs[0]);
        addrOut.off = (offset - m_off(regs)) % m_size(regs[0]);
        return true;
    }
    addrOut.typ = GSPEC;
    addrOut.val.gs = offset;
    addrOut.off = 0;
    return true;
#undef contains_offset
#undef m_endoff
#undef m_size
#undef m_off
}

void PandaTaintVisitor::insertStateOp(Instruction &I) {
    // These are loads/stores from CPUState etc.
    LLVMContext &ctx = I.getContext();
    Addr addr;

    Value *ptr = I.getOperand(isa<StoreInst>(I) ? 1 : 0);
    Value *val = isa<StoreInst>(I) ? I.getOperand(0) : &I;
    uint64_t size = getValueSize(val);
    if (getAddr(I.getOperand(0), addr)) {
        // Successfully statically found offset.
        Constant *destConst;
        uint64_t destAddr;
        if (addr.typ == GREG) {
            destConst = grvConst;
            destAddr = addr.val.gr * MAXREGSIZE + addr.off;
        } else {
            destConst = gsvConst;
            destAddr = addr.val.gs;
        }
        insertTaintCopy(I, llvConst, &I, destConst, const_uint64(ctx, destAddr), size);
    } else {
        PtrToIntInst *P2II = new PtrToIntInst(ptr, Type::getInt64Ty(ctx), "", &I);
        vector<Value *> args{
            const_uint64_ptr(ctx, cpu_single_env), P2II,
            llvConst, constSlot(ctx, &I), grvConst, gsvConst,
            const_uint64(ctx, size), ConstantInt::getTrue(ctx)
        };
        inlineCallAfter(I, hostCopyF, args);
    }
}

void PandaTaintVisitor::visitLoadInst(LoadInst &I) {
    insertStateOp(I);
}

/*
 * We should only care about non-volatile stores, the volatile stores are
 * irrelevant to guest execution.  Volatile stores come in pairs for each guest
 * instruction, so we can gather statistics looking at every other volatile
 * store.
 */
void PandaTaintVisitor::visitStoreInst(StoreInst &I) {
    // look for magic taint pc update info
    MDNode *md = I.getMetadata("pcupdate.md");
    if (md != NULL) {
        // found store instruction that contains PC.  
    }

    if (I.isVolatile()) {
        return;
    }

    insertStateOp(I);
}

void PandaTaintVisitor::visitFenceInst(FenceInst &I) {}
void PandaTaintVisitor::visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {}
void PandaTaintVisitor::visitAtomicRMWInst(AtomicRMWInst &I) {}

/*
 * In TCG->LLVM translation, it seems like this instruction is only used to get
 * the pointer to the CPU state.  Because of this, we will just delete taint at
 * the destination LLVM register.
 */
void PandaTaintVisitor::visitGetElementPtrInst(GetElementPtrInst &I) {
    insertTaintMix(I, I.getOperand(0));
}

// Cast operators
void PandaTaintVisitor::visitCastInst(CastInst &I) {
    Value *src = I.getOperand(0);

    unsigned srcSize = getValueSize(src), destSize = getValueSize(&I);
    switch (I.getOpcode()) {
        // Mixed cases
        case Instruction::FPExt:
        case Instruction::FPToSI:
        case Instruction::FPTrunc:
        case Instruction::SIToFP:
        case Instruction::UIToFP:
            insertTaintMix(I, &I, src);
            break;

        case Instruction::SExt:
            if (destSize > srcSize) {
                // Generate a sext.
                insertTaintSext(I, src);
                break;
            }
            // Else fall through to a copy.
        // Parallel cases. Assume little-endian...
        // Both involve a simple copy.
        case Instruction::BitCast:
        case Instruction::IntToPtr:
        case Instruction::PtrToInt:
        case Instruction::Trunc:
        case Instruction::ZExt:
           insertTaintCopy(I, llvConst, &I,
                   llvConst, src,
                   std::min(srcSize, destSize));
           break;
        default:
           // BROKEN
           assert(false && "Bad CastInst!!");
    }
}

// Other operators

/*
 * If both operands are LLVM registers, then the result will be a one bit (byte)
 * compute taint.  If only one operand is a register, then the result will be a
 * compute, but only propagating taint from the register source.  If both are
 * constants, then it will be a delete.  Since this is usually used for a branch
 * condition, this could let us see if we can
 * potentially affect control flow.
 */
void PandaTaintVisitor::visitCmpInst(CmpInst &I) {
    insertTaintCompute(I, &I, I.getOperand(0), I.getOperand(1), true);
}

void PandaTaintVisitor::visitPHINode(PHINode &I) {
    LLVMContext &ctx = I.getContext();
    LoadInst *LI = new LoadInst(prevBbConst);
    assert(LI != NULL);
    assert(I.getParent()->getFirstNonPHI() != NULL);

    LI->insertBefore(I.getParent()->getFirstNonPHI());
    vector<pair<Value *,Value *>> selections;
    for (unsigned i = 0; i < I.getNumIncomingValues(); ++i) {
        Constant *select = constSlot(ctx, I.getIncomingBlock(i));
        Constant *value = constWeakSlot(ctx, I.getIncomingValue(i));
        selections.push_back(std::make_pair(select, value));
    }
    insertTaintSelect(*LI, &I, LI, selections);
}

void PandaTaintVisitor::visitMemCpyInst(MemTransferInst &I) {
    int size = intValue(I.getLength());
    assert(size >= 0);

    insertTaintCopy(I, memConst, NULL, memConst, NULL, size);
}

void PandaTaintVisitor::visitMemMoveInst(MemTransferInst &I) {
    int size = intValue(I.getLength());
    assert(size >= 0);

    insertTaintMove(I, memConst, NULL, memConst, NULL, size);
}

void PandaTaintVisitor::visitMemSetInst(MemSetInst &I) {
    LLVMContext &ctx = I.getContext();

    CallInst *destCI = insertLogPop(I);
    assert(destCI != NULL);

    Value *size = I.getLength();
    Value *writeval = I.getValue();
    if (isa<Constant>(writeval)) {
        insertTaintDelete(*destCI, memConst, destCI, size);
    } else {
        assert(size->getType()->isIntegerTy());
        assert(getValueSize(size) <= 8);
        if (getValueSize(size) < 8) {
            // insert ZExtInst before I.
            size = new ZExtInst(size, IntegerType::get(ctx, 64), "", &I);
        }
        vector<Value *> args{
            memConst, destCI, size, llvConst, constSlot(ctx, writeval)
        };
        inlineCallAfter(*destCI, setF, args);
    }
#ifndef TAINT2_NOINLINE
    InlineFunctionInfo IFI;
    if (!InlineFunction(destCI, IFI)) {
        printf("Couldn't inline!!\n");
    }
#endif
}

void PandaTaintVisitor::visitCallInst(CallInst &I) {
    Function *called = I.getCalledFunction();
    LLVMContext &ctx = I.getContext();
    if (!called) {
        //assert(1==0);
        //return; // doesn't have name, we can't process it
        // Might be ok for now, but we might need to revisit.
        printf("Note: skipping taint analysis of statically unknowable call in %s.\n",
            I.getParent()->getParent()->getName().str().c_str());
        return;
    }
    std::string calledName = called->getName().str();

    switch (I.getCalledFunction()->getIntrinsicID()) {
        case Intrinsic::uadd_with_overflow:
            insertTaintCompute(I, I.getArgOperand(0), I.getArgOperand(1), 1);
            return;
        case Intrinsic::bswap:
        case Intrinsic::ctlz:
            insertTaintMix(I, I.getArgOperand(0));
            return;
        case Intrinsic::not_intrinsic:
            break;
        default:
            printf("Note: unsupported intrinsic %s in %s.\n",
                I.getCalledFunction()->getName().str().c_str(),
                I.getParent()->getParent()->getName().str().c_str());
            return;
    }

    assert(!I.getCalledFunction()->isIntrinsic());
    if (!calledName.compare("__ldb_mmu_panda")
            || !calledName.compare("__ldw_mmu_panda")
            || !calledName.compare("__ldl_mmu_panda")
            || !calledName.compare("__ldq_mmu_panda")) {
        insertTaintCopy(I, llvConst, &I, memConst, NULL, getValueSize(&I));
        return;
    }
    else if (!calledName.compare("__stb_mmu_panda")
            || !calledName.compare("__stw_mmu_panda")
            || !calledName.compare("__stl_mmu_panda")
            || !calledName.compare("__stq_mmu_panda")) {
        Value *src = I.getArgOperand(1);
        if (isa<Constant>(src)) {
            insertTaintDelete(I, llvConst, &I, const_uint64(ctx, getValueSize(src)));
        } else {
            insertTaintCopy(I, memConst, NULL, llvConst, src, getValueSize(src));
        }
        return;
    }
    else if (called->getName().startswith("taint")) {
        return;
    }
    else if (!calledName.compare("sin")
            || !calledName.compare("cos")
            || !calledName.compare("tan")
            || !calledName.compare("log")
            || !calledName.compare("__isinf")
            || !calledName.compare("__isnan")
            || !calledName.compare("rint")
            || !calledName.compare("floor")
            || !calledName.compare("abs")
            || !calledName.compare("ceil")
            || !calledName.compare("exp2")) {
        insertTaintMix(I, I.getArgOperand(0));
        return;
    }
    else if (!calledName.compare("ldexp")
            || !calledName.compare("atan2")) {
        insertTaintCompute(I, I.getArgOperand(0), I.getArgOperand(1), true);
        return;
    }
    else if (!calledName.compare(0, 9, "helper_in") && calledName.size() == 10) {
        /*
         * The last character of the instruction name determines the size of data transfer
         * b = single byte
         * w = 2 bytes
         * l - 4 bytes
         */
        /*char type = *calledName.rbegin();
        int len;
        if (type == 'b') {
            len = 1;
        } else if (type == 'w') {
            len = 2;
        } else if (type == 'l') {
            len = 4;
        }
*/
        /* helper_in instructions will be modeled as loads with various lengths */
        // For now do nothing.
        return;
    }
    else if (!calledName.compare(0, 10, "helper_out") && calledName.size() == 11) {
        /*
         * The last character of the instruction name determines the size of data transfer
         * b = single byte
         * w = 2 bytes
         * l - 4 bytes
         */
        /*char type = *calledName.rbegin();
        int len;
        if (type == 'b') {
            len = 1;
        } else if (type == 'w') {
            len = 2;
        } else {
            len = 4;
        }
*/
        /* helper_out instructions will be modeled as stores with various lengths */
        // For now do nothing.
        //portStoreHelper(I.getArgOperand(1), I.getArgOperand(0), len);
        return;
    } else {
        // This is a call that we aren't going to model, so we need to process
        // it instruction by instruction.
        // First, we need to set up a new stack frame and copy argument taint.
        vector<Value *> fargs{ llvConst };
        int numArgs = I.getNumArgOperands();
        for (int i = 0; i < numArgs; i++) {
            Value *arg = I.getArgOperand(i);
            int argBytes = getValueSize(arg);
            assert(argBytes > 0);

            // if arg is constant then do nothing
            if (!isa<Constant>(arg)) {
                vector<Value *> copyargs{
                    llvConst, const_uint64(ctx, (shad->num_vals + i) * MAXREGSIZE),
                    llvConst, constSlot(ctx, arg), const_uint64(ctx, argBytes)
                };
                inlineCallBefore(I, copyF, copyargs);
            }
        }
        if (!called->getReturnType()->isVoidTy()) { // Copy from return slot.
            vector<Value *> retargs{
                llvConst, constSlot(ctx, &I), retConst,
                const_uint64(ctx, 0), const_uint64(ctx, MAXREGSIZE)
            };
            inlineCallAfter(I, copyF, retargs);
        }
        inlineCallBefore(I, pushFrameF, fargs);
        inlineCallAfter(I, popFrameF, fargs);
    }
}
/*
// For now delete dest taint.
void PandaTaintVisitor::portLoadHelper(Value *srcval, Value *dstval, int len) {

}

// this is essentially a copy of storeHelper without the tainted pointer code
void PandaTaintVisitor::portStoreHelper(Value *srcval, Value *dstval, int len) {
    // can't propagate taint from a constant
    bool srcConstant = isa<Constant>(srcval);

    struct addr_struct src = {};
    struct addr_struct dst = {};
    struct taint_op_struct op = {};
    char name[6] = "store";

    // write instruction boundary op
    op.typ = INSNSTARTOP;
    strncpy(op.val.insn_start.name, name, OPNAMELENGTH);
    op.val.insn_start.num_ops = len;
    op.val.insn_start.flag = INSNREADLOG;
    tob_op_write(tbuf, &op);

    if (srcConstant) {
        op.typ = DELETEOP;
        dst.typ = UNK;
        dst.val.ua = 0;
        dst.flag = READLOG;
        for (int i = 0; i < len; i++) {
            dst.off = i;
            op.val.deletel.a = dst;
            tob_op_write(tbuf, &op);
        }
    }
    else {
        op.typ = COPYOP;
        dst.typ = UNK;
        dst.flag = READLOG;
        dst.val.ua = 0;
        src.typ = LADDR;
        src.val.la = PST->getLocalSlot(srcval);
        for (int i = 0; i < len; i++) {
            src.off = i;
            dst.off = i;
            op.val.copy.a = src;
            op.val.copy.b = dst;
            tob_op_write(tbuf, &op);
        }
    }
}
*/

void PandaTaintVisitor::visitSelectInst(SelectInst &I) {
    LLVMContext &ctx = I.getContext();
    ZExtInst *ZEI = new ZExtInst(I.getCondition(), Type::getInt64Ty(ctx), "", &I);
    assert(ZEI);

    vector<pair<Value *, Value *>> selections;
    selections.push_back(std::make_pair(ConstantInt::get(ctx, APInt(64, 1)), I.getTrueValue()));
    selections.push_back(std::make_pair(ConstantInt::get(ctx, APInt(64, 0)), I.getFalseValue()));
    insertTaintSelect(I, &I, ZEI, selections);
}

void PandaTaintVisitor::visitExtractValueInst(ExtractValueInst &I) {}
void PandaTaintVisitor::visitInsertValueInst(InsertValueInst &I) {}

// Unhandled
void PandaTaintVisitor::visitInstruction(Instruction &I) {
    printf("Error: Unhandled instruction\n");
    assert(1==0);
}
