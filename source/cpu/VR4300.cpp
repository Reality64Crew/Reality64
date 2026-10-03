#include "cpu/VR4300.h"

#include <algorithm>
#include <cstdio>

#include "core/MemoryBus.h"

namespace reality64 {

namespace {

using namespace cpu;

constexpr uint64_t ExceptionBase = 0xFFFFFFFF80000000ull;
constexpr uint64_t ExceptionBaseBev = 0xFFFFFFFFBFC00200ull;
constexpr uint32_t ExceptionVectorGeneral = 0x180;

bool addOverflow32(uint32_t a, uint32_t b, uint32_t r) { return ((~(a ^ b) & (a ^ r)) >> 31) != 0; }
bool subOverflow32(uint32_t a, uint32_t b, uint32_t r) { return (((a ^ b) & (a ^ r)) >> 31) != 0; }
bool addOverflow64(uint64_t a, uint64_t b, uint64_t r) { return ((~(a ^ b) & (a ^ r)) >> 63) != 0; }
bool subOverflow64(uint64_t a, uint64_t b, uint64_t r) { return (((a ^ b) & (a ^ r)) >> 63) != 0; }

// 64x64 -> 128 bit multiply without relying on compiler extensions.
void mulu64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {
    const uint64_t aLo = a & 0xFFFFFFFFu, aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
    const uint64_t ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
    const uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
    lo = (mid << 32) | (ll & 0xFFFFFFFFu);
    hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

void muls64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {
    mulu64(a, b, hi, lo);
    if (static_cast<int64_t>(a) < 0) hi -= b;
    if (static_cast<int64_t>(b) < 0) hi -= a;
}

}  // namespace

VR4300::VR4300(MemoryBus& bus) : bus_(bus) { reset(); }

void VR4300::reset() {
    gpr_.fill(0);
    cop0_.fill(0);
    hi_ = lo_ = 0;
    cop0_[Status] = 0x34000000;
    cop0_[Config] = 0x0006E463;
    cop0_[PRId] = 0x00000B22;
    cop0_[Random] = 31;
    cop0_[Wired] = 0;
    fpr_.fill(0);
    fcsr_ = 0;
    tlb_.fill(TlbEntry{});
    llBit_ = false;
    countHalfTick_ = false;
    halted_ = false;
    haltReason_.clear();
    instructionCount_ = 0;
    inDelaySlot_ = false;
    setPc(0xFFFFFFFFBFC00000ull);
}

void VR4300::halt(const std::string& reason) {
    char where[40];
    std::snprintf(where, sizeof(where), " (pc=%016llx)", static_cast<unsigned long long>(currentPc_));
    halted_ = true;
    haltReason_ = reason + where;
}

// One instruction with full per-instruction bookkeeping.
bool VR4300::step() {
    if (halted_) return false;
    if (serviceTimersAndInterrupts(1)) {
        ++instructionCount_;  // the interrupt entry itself
        return true;
    }
    executeOne();
    return !halted_;
}

// The fast path: timers and interrupts are serviced once per batch rather than
// once per instruction. An interrupt can therefore be taken up to a batch late,
// which no software can observe at this granularity.
uint64_t VR4300::run(uint64_t maxInstructions) {
    constexpr uint64_t Batch = 32;
    uint64_t done = 0;
    while (done < maxInstructions && !halted_) {
        const uint64_t n = std::min(maxInstructions - done, Batch);
        serviceTimersAndInterrupts(n);  // if one is taken, the batch continues in the handler
        for (uint64_t i = 0; i < n; ++i) {
            executeOne();
            if (halted_) return done + i;
        }
        done += n;
    }
    return done;
}

// Advances Count/Compare by `instructions` and takes a pending interrupt.
// Returns true if an interrupt was taken.
bool VR4300::serviceTimersAndInterrupts(uint64_t instructions) {
    // Count advances at half the pipeline clock; keep the odd half-tick between calls.
    const uint64_t halfTicks = (countHalfTick_ ? 1u : 0u) + instructions;
    countHalfTick_ = (halfTicks & 1) != 0;
    const uint32_t increment = static_cast<uint32_t>(halfTicks / 2);
    if (increment) {
        const uint32_t before = static_cast<uint32_t>(cop0_[Count]);
        cop0_[Count] = before + increment;
        // Compare was crossed (or hit) if it lies in (before, before + increment].
        const uint32_t distance = static_cast<uint32_t>(cop0_[Compare]) - before;
        if (distance >= 1 && distance <= increment) cop0_[Cause] |= CauseIP7;
    }

    // The MI interrupt line is level-triggered into Cause.IP2.
    if (bus_.interruptPending()) cop0_[Cause] |= CauseIP2;
    else cop0_[Cause] &= ~static_cast<uint64_t>(CauseIP2);

    const uint32_t status = static_cast<uint32_t>(cop0_[Status]);
    const uint32_t cause = static_cast<uint32_t>(cop0_[Cause]);
    if (!(status & StatusIE) || (status & (StatusEXL | StatusERL))) return false;
    if (!(cause & status & (CauseIPMask & StatusIM))) return false;

    currentPc_ = pc_;
    inDelaySlot_ = nextIsDelaySlot_;
    raiseException(ExcInterrupt);
    return true;
}

// Fetches and executes the instruction at pc_. No timer or interrupt work.
void VR4300::executeOne() {
    currentPc_ = pc_;
    inDelaySlot_ = nextIsDelaySlot_;
    nextIsDelaySlot_ = false;
    ++instructionCount_;

    if (pc_ & 3) {
        cop0_[BadVAddr] = pc_;
        raiseException(ExcAddressErrorLoad);
        return;
    }

    uint32_t paddr;
    const uint32_t a = static_cast<uint32_t>(pc_);
    if ((a & 0xC0000000u) == 0x80000000u && pc_ == sx32(a)) {
        paddr = a & 0x1FFFFFFFu;  // KSEG0/KSEG1: where nearly all code runs
    } else if (!translate(pc_, Access::Fetch, paddr)) {
        return;  // halted, or a TLB exception was taken instead
    }

    const uint32_t instr = bus_.read32(paddr);
    if (traceHook_) traceHook_(pc_, instr);

    pc_ = nextPc_;
    nextPc_ += 4;
    execute(instr);
    gpr_[0] = 0;
}

// KSEG0/KSEG1 are direct-mapped; everything else goes through the TLB. Returns
// false when no physical address was produced: either a TLB/address exception
// was raised (the CPU keeps running at the handler) or the CPU halted.
bool VR4300::translate(uint64_t vaddr, Access access, uint32_t& paddr) {
    const uint32_t a = static_cast<uint32_t>(vaddr);
    if (vaddr != sx32(a)) {
        halt("64-bit virtual addresses are not supported");
        return false;
    }
    if (a >= 0x80000000u && a < 0xC0000000u) {
        paddr = a & 0x1FFFFFFFu;
        return true;
    }
    return translateTlb(vaddr, access, paddr);
}

bool VR4300::readMem(uint64_t vaddr, unsigned size, uint64_t& value) {
    if (vaddr & (size - 1)) {
        cop0_[BadVAddr] = vaddr;
        raiseException(ExcAddressErrorLoad);
        return false;
    }
    uint32_t paddr;
    if (!translate(vaddr, Access::Load, paddr)) return false;
    value = bus_.read(paddr, size);
    return true;
}

bool VR4300::writeMem(uint64_t vaddr, unsigned size, uint64_t value) {
    if (vaddr & (size - 1)) {
        cop0_[BadVAddr] = vaddr;
        raiseException(ExcAddressErrorStore);
        return false;
    }
    uint32_t paddr;
    if (!translate(vaddr, Access::Store, paddr)) return false;
    bus_.write(paddr, size, value);
    return true;
}

// pc_ already points at the delay slot when an instruction executes.
void VR4300::branch(bool taken, uint64_t target, bool likely) {
    if (taken) {
        nextPc_ = target;
        nextIsDelaySlot_ = true;
    } else if (likely) {
        pc_ = nextPc_;  // nullify the delay slot
        nextPc_ = pc_ + 4;
    } else {
        nextIsDelaySlot_ = true;
    }
}

void VR4300::raiseException(unsigned code, unsigned coprocessor, bool tlbRefill) {
    uint32_t status = static_cast<uint32_t>(cop0_[Status]);
    uint32_t cause = static_cast<uint32_t>(cop0_[Cause]);
    const bool wasInException = (status & StatusEXL) != 0;

    if (!wasInException) {
        cop0_[EPC] = inDelaySlot_ ? currentPc_ - 4 : currentPc_;
        cause = inDelaySlot_ ? (cause | CauseBD) : (cause & ~CauseBD);
        status |= StatusEXL;
    }
    cause = (cause & ~CauseExcMask) | (code << 2);
    cause = (cause & ~CauseCEMask) | (coprocessor << 28);

    cop0_[Status] = status;
    cop0_[Cause] = cause;

    // A TLB refill (miss) uses the dedicated vector unless already in an exception.
    const uint32_t offset = (tlbRefill && !wasInException) ? 0 : ExceptionVectorGeneral;
    const uint64_t base = (status & StatusBEV) ? ExceptionBaseBev : ExceptionBase;
    setPc(base + offset);
}

void VR4300::coprocessorUnusable(unsigned coprocessor) {
    raiseException(ExcCoprocessorUnusable, coprocessor);
}

// Random counts down from 31 to Wired on every clock. Nothing but TLBWR and
// MFC0 can observe it, so it is derived from the instruction count on demand
// instead of being decremented every instruction.
uint32_t VR4300::randomValue() const {
    const uint32_t wired = static_cast<uint32_t>(cop0_[Wired] & 0x3F);
    const uint32_t range = wired >= 31 ? 1 : 32 - wired;
    return 31 - static_cast<uint32_t>(instructionCount_ % range);
}

uint64_t VR4300::readCop0(unsigned reg) const {
    if (reg == Random) return randomValue();
    return sx32(static_cast<uint32_t>(cop0_[reg]));
}

void VR4300::writeCop0(unsigned reg, uint64_t value) {
    switch (reg) {
        case Random:
        case PRId:
        case BadVAddr:
            break;  // read-only
        case Compare:
            cop0_[Compare] = static_cast<uint32_t>(value);
            cop0_[Cause] &= ~static_cast<uint64_t>(CauseIP7);
            break;
        case Cause:
            cop0_[Cause] = (cop0_[Cause] & ~static_cast<uint64_t>(CauseSoftwareIP)) |
                           (value & CauseSoftwareIP);
            break;
        case Config:
            cop0_[Config] = (cop0_[Config] & ~0x0F00800Full) | (value & 0x0F00800Full);
            break;
        case EPC:
        case ErrorEPC:
        case XContext:  // 64-bit registers keep all bits
            cop0_[reg] = value;
            break;
        case Context:  // only PTEBase (bits 31:23) is writable
            cop0_[Context] = (cop0_[Context] & 0x7FFFFFull) | (value & ~0x7FFFFFull);
            break;
        case Index:
            cop0_[Index] = value & 0x8000003Full;
            break;
        case EntryLo0:
        case EntryLo1:
            cop0_[reg] = value & 0x3FFFFFFFull;
            break;
        case PageMask:
            cop0_[PageMask] = value & 0x01FFE000ull;
            break;
        case EntryHi:
            cop0_[EntryHi] = sx32(static_cast<uint32_t>(value) & 0xFFFFE0FFu);
            break;
        case Wired:
            cop0_[Wired] = value & 0x3F;
            break;
        default:
            cop0_[reg] = static_cast<uint32_t>(value);
            break;
    }
}

void VR4300::execute(uint32_t instr) {
    const uint32_t op = instr >> 26;
    const unsigned rs = (instr >> 21) & 31;
    const unsigned rt = (instr >> 16) & 31;
    const uint64_t simm = sx16(instr & 0xFFFF);
    const uint64_t uimm = instr & 0xFFFF;
    const uint64_t addr = gpr_[rs] + simm;

    switch (op) {
        case 0x00: executeSpecial(instr); break;
        case 0x01: executeRegimm(instr); break;

        case 0x02:  // J
        case 0x03: {  // JAL
            const uint64_t target = (pc_ & 0xFFFFFFFFF0000000ull) | ((instr & 0x03FFFFFFu) << 2);
            if (op == 0x03) gpr_[31] = pc_ + 4;
            branch(true, target, false);
            break;
        }

        case 0x04: branch(gpr_[rs] == gpr_[rt], pc_ + (simm << 2), false); break;   // BEQ
        case 0x05: branch(gpr_[rs] != gpr_[rt], pc_ + (simm << 2), false); break;   // BNE
        case 0x06: branch(static_cast<int64_t>(gpr_[rs]) <= 0, pc_ + (simm << 2), false); break;  // BLEZ
        case 0x07: branch(static_cast<int64_t>(gpr_[rs]) > 0, pc_ + (simm << 2), false); break;   // BGTZ
        case 0x14: branch(gpr_[rs] == gpr_[rt], pc_ + (simm << 2), true); break;    // BEQL
        case 0x15: branch(gpr_[rs] != gpr_[rt], pc_ + (simm << 2), true); break;    // BNEL
        case 0x16: branch(static_cast<int64_t>(gpr_[rs]) <= 0, pc_ + (simm << 2), true); break;   // BLEZL
        case 0x17: branch(static_cast<int64_t>(gpr_[rs]) > 0, pc_ + (simm << 2), true); break;    // BGTZL

        case 0x08: {  // ADDI
            const uint32_t a = static_cast<uint32_t>(gpr_[rs]);
            const uint32_t b = static_cast<uint32_t>(simm);
            const uint32_t r = a + b;
            if (addOverflow32(a, b, r)) raiseException(ExcOverflow);
            else gpr_[rt] = sx32(r);
            break;
        }
        case 0x09: gpr_[rt] = sx32(static_cast<uint32_t>(gpr_[rs] + simm)); break;  // ADDIU
        case 0x0A: gpr_[rt] = static_cast<int64_t>(gpr_[rs]) < static_cast<int64_t>(simm); break;  // SLTI
        case 0x0B: gpr_[rt] = gpr_[rs] < simm; break;  // SLTIU
        case 0x0C: gpr_[rt] = gpr_[rs] & uimm; break;  // ANDI
        case 0x0D: gpr_[rt] = gpr_[rs] | uimm; break;  // ORI
        case 0x0E: gpr_[rt] = gpr_[rs] ^ uimm; break;  // XORI
        case 0x0F: gpr_[rt] = sx32(static_cast<uint32_t>(uimm << 16)); break;  // LUI

        case 0x10: executeCop0(instr); break;
        case 0x11: executeCop1(instr); break;
        case 0x31:  // LWC1
        case 0x35:  // LDC1
        case 0x39:  // SWC1
        case 0x3D:  // SDC1
            executeFpuMemory(instr, addr);
            break;

        case 0x18: {  // DADDI
            const uint64_t r = gpr_[rs] + simm;
            if (addOverflow64(gpr_[rs], simm, r)) raiseException(ExcOverflow);
            else gpr_[rt] = r;
            break;
        }
        case 0x19: gpr_[rt] = gpr_[rs] + simm; break;  // DADDIU

        case 0x20: {  // LB
            uint64_t v;
            if (readMem(addr, 1, v)) gpr_[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v)));
            break;
        }
        case 0x21: {  // LH
            uint64_t v;
            if (readMem(addr, 2, v)) gpr_[rt] = sx16(static_cast<uint32_t>(v));
            break;
        }
        case 0x23: {  // LW
            uint64_t v;
            if (readMem(addr, 4, v)) gpr_[rt] = sx32(static_cast<uint32_t>(v));
            break;
        }
        case 0x24: {  // LBU
            uint64_t v;
            if (readMem(addr, 1, v)) gpr_[rt] = v;
            break;
        }
        case 0x25: {  // LHU
            uint64_t v;
            if (readMem(addr, 2, v)) gpr_[rt] = v;
            break;
        }
        case 0x27: {  // LWU
            uint64_t v;
            if (readMem(addr, 4, v)) gpr_[rt] = v;
            break;
        }
        case 0x37: {  // LD
            uint64_t v;
            if (readMem(addr, 8, v)) gpr_[rt] = v;
            break;
        }
        case 0x30: {  // LL
            uint64_t v;
            if (readMem(addr, 4, v)) {
                gpr_[rt] = sx32(static_cast<uint32_t>(v));
                llBit_ = true;
                uint32_t paddr;
                if (translate(addr, Access::Load, paddr)) cop0_[LLAddr] = paddr >> 4;
            }
            break;
        }
        case 0x34: {  // LLD
            uint64_t v;
            if (readMem(addr, 8, v)) {
                gpr_[rt] = v;
                llBit_ = true;
                uint32_t paddr;
                if (translate(addr, Access::Load, paddr)) cop0_[LLAddr] = paddr >> 4;
            }
            break;
        }

        case 0x22: {  // LWL
            uint64_t w;
            if (!readMem(addr & ~3ull, 4, w)) break;
            const unsigned shift = (addr & 3) * 8;
            const uint32_t r = (static_cast<uint32_t>(gpr_[rt]) & ~(0xFFFFFFFFu << shift)) |
                               (static_cast<uint32_t>(w) << shift);
            gpr_[rt] = sx32(r);
            break;
        }
        case 0x26: {  // LWR
            uint64_t w;
            if (!readMem(addr & ~3ull, 4, w)) break;
            const unsigned shift = (3 - (addr & 3)) * 8;
            const uint32_t r = (static_cast<uint32_t>(gpr_[rt]) & ~(0xFFFFFFFFu >> shift)) |
                               (static_cast<uint32_t>(w) >> shift);
            gpr_[rt] = sx32(r);
            break;
        }
        case 0x1A: {  // LDL
            uint64_t w;
            if (!readMem(addr & ~7ull, 8, w)) break;
            const unsigned shift = (addr & 7) * 8;
            gpr_[rt] = (gpr_[rt] & ~(~0ull << shift)) | (w << shift);
            break;
        }
        case 0x1B: {  // LDR
            uint64_t w;
            if (!readMem(addr & ~7ull, 8, w)) break;
            const unsigned shift = (7 - (addr & 7)) * 8;
            gpr_[rt] = (gpr_[rt] & ~(~0ull >> shift)) | (w >> shift);
            break;
        }

        case 0x28: writeMem(addr, 1, gpr_[rt]); break;  // SB
        case 0x29: writeMem(addr, 2, gpr_[rt]); break;  // SH
        case 0x2B: writeMem(addr, 4, gpr_[rt]); break;  // SW
        case 0x3F: writeMem(addr, 8, gpr_[rt]); break;  // SD
        case 0x38:  // SC
            if (llBit_) {
                if (writeMem(addr, 4, gpr_[rt])) gpr_[rt] = 1;
            } else {
                gpr_[rt] = 0;
            }
            break;
        case 0x3C:  // SCD
            if (llBit_) {
                if (writeMem(addr, 8, gpr_[rt])) gpr_[rt] = 1;
            } else {
                gpr_[rt] = 0;
            }
            break;

        case 0x2A: {  // SWL
            uint64_t w;
            if (!readMem(addr & ~3ull, 4, w)) break;
            const unsigned shift = (addr & 3) * 8;
            const uint32_t r = (static_cast<uint32_t>(w) & ~(0xFFFFFFFFu >> shift)) |
                               (static_cast<uint32_t>(gpr_[rt]) >> shift);
            writeMem(addr & ~3ull, 4, r);
            break;
        }
        case 0x2E: {  // SWR
            uint64_t w;
            if (!readMem(addr & ~3ull, 4, w)) break;
            const unsigned shift = (3 - (addr & 3)) * 8;
            const uint32_t r = (static_cast<uint32_t>(w) & ~(0xFFFFFFFFu << shift)) |
                               (static_cast<uint32_t>(gpr_[rt]) << shift);
            writeMem(addr & ~3ull, 4, r);
            break;
        }
        case 0x2C: {  // SDL
            uint64_t w;
            if (!readMem(addr & ~7ull, 8, w)) break;
            const unsigned shift = (addr & 7) * 8;
            writeMem(addr & ~7ull, 8, (w & ~(~0ull >> shift)) | (gpr_[rt] >> shift));
            break;
        }
        case 0x2D: {  // SDR
            uint64_t w;
            if (!readMem(addr & ~7ull, 8, w)) break;
            const unsigned shift = (7 - (addr & 7)) * 8;
            writeMem(addr & ~7ull, 8, (w & ~(~0ull << shift)) | (gpr_[rt] << shift));
            break;
        }

        case 0x2F:  // CACHE: no cache is modelled
            break;

        default:
            raiseException(ExcReservedInstruction);
            break;
    }
}

void VR4300::executeSpecial(uint32_t instr) {
    const unsigned rs = (instr >> 21) & 31;
    const unsigned rt = (instr >> 16) & 31;
    const unsigned rd = (instr >> 11) & 31;
    const unsigned sa = (instr >> 6) & 31;
    const uint64_t a = gpr_[rs];
    const uint64_t b = gpr_[rt];
    const uint32_t a32 = static_cast<uint32_t>(a);
    const uint32_t b32 = static_cast<uint32_t>(b);

    switch (instr & 0x3F) {
        case 0x00: gpr_[rd] = sx32(b32 << sa); break;  // SLL
        case 0x02: gpr_[rd] = sx32(b32 >> sa); break;  // SRL
        case 0x03: gpr_[rd] = sx32(static_cast<uint32_t>(static_cast<int32_t>(b32) >> sa)); break;  // SRA
        case 0x04: gpr_[rd] = sx32(b32 << (a32 & 31)); break;  // SLLV
        case 0x06: gpr_[rd] = sx32(b32 >> (a32 & 31)); break;  // SRLV
        case 0x07: gpr_[rd] = sx32(static_cast<uint32_t>(static_cast<int32_t>(b32) >> (a32 & 31))); break;  // SRAV

        case 0x08: branch(true, a, false); break;  // JR
        case 0x09: {  // JALR
            const uint64_t target = a;
            gpr_[rd] = pc_ + 4;
            branch(true, target, false);
            break;
        }
        case 0x0C: raiseException(ExcSyscall); break;     // SYSCALL
        case 0x0D: raiseException(ExcBreakpoint); break;  // BREAK
        case 0x0F: break;                                 // SYNC

        case 0x10: gpr_[rd] = hi_; break;  // MFHI
        case 0x11: hi_ = a; break;         // MTHI
        case 0x12: gpr_[rd] = lo_; break;  // MFLO
        case 0x13: lo_ = a; break;         // MTLO

        case 0x14: gpr_[rd] = b << (a & 63); break;  // DSLLV
        case 0x16: gpr_[rd] = b >> (a & 63); break;  // DSRLV
        case 0x17: gpr_[rd] = static_cast<uint64_t>(static_cast<int64_t>(b) >> (a & 63)); break;  // DSRAV

        case 0x18: {  // MULT
            const int64_t p = static_cast<int64_t>(static_cast<int32_t>(a32)) * static_cast<int32_t>(b32);
            lo_ = sx32(static_cast<uint32_t>(p));
            hi_ = sx32(static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32));
            break;
        }
        case 0x19: {  // MULTU
            const uint64_t p = static_cast<uint64_t>(a32) * b32;
            lo_ = sx32(static_cast<uint32_t>(p));
            hi_ = sx32(static_cast<uint32_t>(p >> 32));
            break;
        }
        case 0x1A: {  // DIV
            const int32_t n = static_cast<int32_t>(a32);
            const int32_t d = static_cast<int32_t>(b32);
            if (d == 0) {
                lo_ = n < 0 ? 1 : ~0ull;
                hi_ = sx32(a32);
            } else if (n == INT32_MIN && d == -1) {
                lo_ = sx32(0x80000000u);
                hi_ = 0;
            } else {
                lo_ = sx32(static_cast<uint32_t>(n / d));
                hi_ = sx32(static_cast<uint32_t>(n % d));
            }
            break;
        }
        case 0x1B:  // DIVU
            if (b32 == 0) {
                lo_ = ~0ull;
                hi_ = sx32(a32);
            } else {
                lo_ = sx32(a32 / b32);
                hi_ = sx32(a32 % b32);
            }
            break;
        case 0x1C: muls64(a, b, hi_, lo_); break;  // DMULT
        case 0x1D: mulu64(a, b, hi_, lo_); break;  // DMULTU
        case 0x1E: {  // DDIV
            const int64_t n = static_cast<int64_t>(a);
            const int64_t d = static_cast<int64_t>(b);
            if (d == 0) {
                lo_ = n < 0 ? 1 : ~0ull;
                hi_ = a;
            } else if (n == INT64_MIN && d == -1) {
                lo_ = a;
                hi_ = 0;
            } else {
                lo_ = static_cast<uint64_t>(n / d);
                hi_ = static_cast<uint64_t>(n % d);
            }
            break;
        }
        case 0x1F:  // DDIVU
            if (b == 0) {
                lo_ = ~0ull;
                hi_ = a;
            } else {
                lo_ = a / b;
                hi_ = a % b;
            }
            break;

        case 0x20: {  // ADD
            const uint32_t r = a32 + b32;
            if (addOverflow32(a32, b32, r)) raiseException(ExcOverflow);
            else gpr_[rd] = sx32(r);
            break;
        }
        case 0x21: gpr_[rd] = sx32(a32 + b32); break;  // ADDU
        case 0x22: {  // SUB
            const uint32_t r = a32 - b32;
            if (subOverflow32(a32, b32, r)) raiseException(ExcOverflow);
            else gpr_[rd] = sx32(r);
            break;
        }
        case 0x23: gpr_[rd] = sx32(a32 - b32); break;  // SUBU
        case 0x24: gpr_[rd] = a & b; break;            // AND
        case 0x25: gpr_[rd] = a | b; break;            // OR
        case 0x26: gpr_[rd] = a ^ b; break;            // XOR
        case 0x27: gpr_[rd] = ~(a | b); break;         // NOR
        case 0x2A: gpr_[rd] = static_cast<int64_t>(a) < static_cast<int64_t>(b); break;  // SLT
        case 0x2B: gpr_[rd] = a < b; break;            // SLTU
        case 0x2C: {  // DADD
            const uint64_t r = a + b;
            if (addOverflow64(a, b, r)) raiseException(ExcOverflow);
            else gpr_[rd] = r;
            break;
        }
        case 0x2D: gpr_[rd] = a + b; break;  // DADDU
        case 0x2E: {  // DSUB
            const uint64_t r = a - b;
            if (subOverflow64(a, b, r)) raiseException(ExcOverflow);
            else gpr_[rd] = r;
            break;
        }
        case 0x2F: gpr_[rd] = a - b; break;  // DSUBU

        case 0x30: if (static_cast<int64_t>(a) >= static_cast<int64_t>(b)) raiseException(ExcTrap); break;  // TGE
        case 0x31: if (a >= b) raiseException(ExcTrap); break;                                               // TGEU
        case 0x32: if (static_cast<int64_t>(a) < static_cast<int64_t>(b)) raiseException(ExcTrap); break;   // TLT
        case 0x33: if (a < b) raiseException(ExcTrap); break;                                                // TLTU
        case 0x34: if (a == b) raiseException(ExcTrap); break;                                               // TEQ
        case 0x36: if (a != b) raiseException(ExcTrap); break;                                               // TNE

        case 0x38: gpr_[rd] = b << sa; break;                                              // DSLL
        case 0x3A: gpr_[rd] = b >> sa; break;                                              // DSRL
        case 0x3B: gpr_[rd] = static_cast<uint64_t>(static_cast<int64_t>(b) >> sa); break;  // DSRA
        case 0x3C: gpr_[rd] = b << (sa + 32); break;                                       // DSLL32
        case 0x3E: gpr_[rd] = b >> (sa + 32); break;                                       // DSRL32
        case 0x3F: gpr_[rd] = static_cast<uint64_t>(static_cast<int64_t>(b) >> (sa + 32)); break;  // DSRA32

        default:
            raiseException(ExcReservedInstruction);
            break;
    }
}

void VR4300::executeRegimm(uint32_t instr) {
    const unsigned rs = (instr >> 21) & 31;
    const unsigned sel = (instr >> 16) & 31;
    const uint64_t simm = sx16(instr & 0xFFFF);
    const uint64_t target = pc_ + (simm << 2);
    const int64_t v = static_cast<int64_t>(gpr_[rs]);

    // Evaluate the condition before the link register is written: rs may be r31.
    switch (sel) {
        case 0x00: branch(v < 0, target, false); break;   // BLTZ
        case 0x01: branch(v >= 0, target, false); break;  // BGEZ
        case 0x02: branch(v < 0, target, true); break;    // BLTZL
        case 0x03: branch(v >= 0, target, true); break;   // BGEZL

        case 0x08: if (v >= static_cast<int64_t>(simm)) raiseException(ExcTrap); break;      // TGEI
        case 0x09: if (gpr_[rs] >= simm) raiseException(ExcTrap); break;                     // TGEIU
        case 0x0A: if (v < static_cast<int64_t>(simm)) raiseException(ExcTrap); break;       // TLTI
        case 0x0B: if (gpr_[rs] < simm) raiseException(ExcTrap); break;                      // TLTIU
        case 0x0C: if (gpr_[rs] == simm) raiseException(ExcTrap); break;                     // TEQI
        case 0x0E: if (gpr_[rs] != simm) raiseException(ExcTrap); break;                     // TNEI

        case 0x10: case 0x11: case 0x12: case 0x13: {  // BLTZAL, BGEZAL, BLTZALL, BGEZALL
            const bool taken = (sel & 1) ? (v >= 0) : (v < 0);
            const bool likely = (sel & 2) != 0;
            gpr_[31] = pc_ + 4;
            branch(taken, target, likely);
            break;
        }
        default:
            raiseException(ExcReservedInstruction);
            break;
    }
}

void VR4300::executeCop0(uint32_t instr) {
    const unsigned fmt = (instr >> 21) & 31;
    const unsigned rt = (instr >> 16) & 31;
    const unsigned rd = (instr >> 11) & 31;

    switch (fmt) {
        case 0x00: gpr_[rt] = readCop0(rd); break;                       // MFC0
        case 0x01: gpr_[rt] = cop0_[rd]; break;                          // DMFC0
        case 0x04: writeCop0(rd, sx32(static_cast<uint32_t>(gpr_[rt]))); break;  // MTC0
        case 0x05: writeCop0(rd, gpr_[rt]); break;                       // DMTC0
        default:
            if (!(fmt & 0x10)) {
                raiseException(ExcReservedInstruction);
                break;
            }
            switch (instr & 0x3F) {
                case 0x18: {  // ERET
                    uint32_t status = static_cast<uint32_t>(cop0_[Status]);
                    uint64_t target;
                    if (status & StatusERL) {
                        target = cop0_[ErrorEPC];
                        status &= ~StatusERL;
                    } else {
                        target = cop0_[EPC];
                        status &= ~StatusEXL;
                    }
                    cop0_[Status] = status;
                    llBit_ = false;
                    setPc(target);
                    break;
                }
                case 0x01: tlbRead(); break;                                         // TLBR
                case 0x02: tlbWrite(static_cast<unsigned>(cop0_[Index] & 0x3F)); break;  // TLBWI
                case 0x06: tlbWrite(randomValue()); break;                                // TLBWR
                case 0x08: tlbProbe(); break;                                        // TLBP
                default:
                    raiseException(ExcReservedInstruction);
                    break;
            }
            break;
    }
}

}  // namespace reality64
