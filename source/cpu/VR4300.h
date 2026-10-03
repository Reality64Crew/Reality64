#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "cpu/CpuCommon.h"

namespace reality64 {

class MemoryBus;

// NEC VR4300 (MIPS III) interpreter.
//
// Implemented: the integer instruction set (incl. 64-bit ops, delay slots and
// branch-likely), LL/SC, COP0 (Count/Compare, interrupts, exceptions, the
// 32-entry TLB) and the FPU (COP1, both FR modes).
// Not implemented: user/supervisor modes (the core always runs in kernel mode),
// 64-bit address space, and FPU exceptions (flags are never raised, so the
// unimplemented-operation trap for denormals never fires).
// Anything unsupported halts the CPU with a message instead of guessing.
class VR4300 {
public:
    enum Cop0Reg : unsigned {
        Index = 0, Random = 1, EntryLo0 = 2, EntryLo1 = 3, Context = 4, PageMask = 5,
        Wired = 6, BadVAddr = 8, Count = 9, EntryHi = 10, Compare = 11, Status = 12,
        Cause = 13, EPC = 14, PRId = 15, Config = 16, LLAddr = 17, WatchLo = 18,
        WatchHi = 19, XContext = 20, ParityError = 26, CacheError = 27, TagLo = 28,
        TagHi = 29, ErrorEPC = 30
    };

    enum ExceptionCode : unsigned {
        ExcInterrupt = 0, ExcTlbModified = 1, ExcTlbLoad = 2, ExcTlbStore = 3,
        ExcAddressErrorLoad = 4, ExcAddressErrorStore = 5,
        ExcSyscall = 8, ExcBreakpoint = 9, ExcReservedInstruction = 10,
        ExcCoprocessorUnusable = 11, ExcOverflow = 12, ExcTrap = 13
    };

    using TraceHook = std::function<void(uint64_t pc, uint32_t instruction)>;

    explicit VR4300(MemoryBus& bus);

    void reset();
    // Executes one instruction. Returns false once the CPU has halted.
    bool step();
    // Executes up to maxInstructions and returns how many completed; stops early
    // if the CPU halts. Faster than calling step() in a loop.
    uint64_t run(uint64_t maxInstructions);

    bool halted() const { return halted_; }
    const std::string& haltReason() const { return haltReason_; }
    void setTraceHook(TraceHook hook) { traceHook_ = std::move(hook); }

    uint64_t gpr(unsigned i) const { return gpr_[i & 31]; }
    void setGpr(unsigned i, uint64_t v) { if (i & 31) gpr_[i & 31] = v; }
    uint64_t hi() const { return hi_; }
    uint64_t lo() const { return lo_; }
    uint64_t pc() const { return pc_; }
    // Jumps without a pending delay slot.
    void setPc(uint64_t pc) { pc_ = pc; nextPc_ = pc + 4; nextIsDelaySlot_ = false; }
    uint64_t cop0(unsigned i) const { return cop0_[i & 31]; }
    void setCop0(unsigned i, uint64_t v) { cop0_[i & 31] = v; }
    uint64_t fpr(unsigned i) const { return fpr_[i & 31]; }
    uint32_t fcsr() const { return fcsr_; }
    uint64_t instructionCount() const { return instructionCount_; }

private:
    enum class Access { Fetch, Load, Store };

    struct TlbEntry {
        uint32_t mask = 0;
        uint32_t hi = 0x80000000u;  // never matches a mapped address
        uint32_t lo0 = 0;
        uint32_t lo1 = 0;
        bool global = false;
    };

    void executeOne();
    bool serviceTimersAndInterrupts(uint64_t instructions);
    uint32_t randomValue() const;

    void execute(uint32_t instr);
    void executeSpecial(uint32_t instr);
    void executeRegimm(uint32_t instr);
    void executeCop0(uint32_t instr);
    void executeCop1(uint32_t instr);
    void executeFpuMemory(uint32_t instr, uint64_t addr);

    bool translate(uint64_t vaddr, Access access, uint32_t& paddr);
    bool translateTlb(uint64_t vaddr, Access access, uint32_t& paddr);
    bool readMem(uint64_t vaddr, unsigned size, uint64_t& value);
    bool writeMem(uint64_t vaddr, unsigned size, uint64_t value);

    void tlbRead();
    void tlbWrite(unsigned index);
    void tlbProbe();
    void raiseTlbException(uint64_t vaddr, Access access, bool refill, bool modified);

    void branch(bool taken, uint64_t target, bool likely);
    void raiseException(unsigned code, unsigned coprocessor = 0, bool tlbRefill = false);
    void halt(const std::string& reason);
    void coprocessorUnusable(unsigned coprocessor);
    uint64_t readCop0(unsigned reg) const;
    void writeCop0(unsigned reg, uint64_t value);

    // FPU register file views; the layout depends on Status.FR.
    bool fpuFr() const;
    uint32_t fprWord(unsigned r) const;
    void setFprWord(unsigned r, uint32_t v);
    uint64_t fprDouble(unsigned r) const;
    void setFprDouble(unsigned r, uint64_t v);

    MemoryBus& bus_;
    std::array<uint64_t, 32> gpr_{};
    std::array<uint64_t, 32> cop0_{};
    std::array<uint64_t, 32> fpr_{};
    std::array<TlbEntry, cpu::TlbEntries> tlb_{};
    uint32_t fcsr_ = 0;
    uint64_t hi_ = 0;
    uint64_t lo_ = 0;
    uint64_t pc_ = 0;
    uint64_t nextPc_ = 0;
    uint64_t currentPc_ = 0;
    bool nextIsDelaySlot_ = false;
    bool inDelaySlot_ = false;
    bool llBit_ = false;
    bool countHalfTick_ = false;
    bool halted_ = false;
    std::string haltReason_;
    uint64_t instructionCount_ = 0;
    TraceHook traceHook_;
};

}  // namespace reality64
