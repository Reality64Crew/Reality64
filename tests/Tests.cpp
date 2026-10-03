// Self-contained test runner (no external framework) so it builds on every CI target.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/Cic.h"
#include "core/Emulator.h"
#include "core/MemoryBus.h"
#include "core/Rom.h"
#include "core/Settings.h"
#include "cpu/VR4300.h"
#include "input/InputMapper.h"

using namespace reality64;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        ++g_checks;                                                                             \
        const uint64_t a_ = static_cast<uint64_t>(actual);                                      \
        const uint64_t e_ = static_cast<uint64_t>(expected);                                    \
        if (a_ != e_) {                                                                         \
            ++g_failures;                                                                       \
            std::printf("  FAIL %s:%d: %s\n    got      0x%016llx\n    expected 0x%016llx\n",   \
                        __FILE__, __LINE__, #actual, static_cast<unsigned long long>(a_),       \
                        static_cast<unsigned long long>(e_));                                   \
        }                                                                                       \
    } while (0)

// --- tiny assembler -------------------------------------------------------

enum Reg : unsigned { zero = 0, at = 1, v0 = 2, a0 = 4, t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12,
                      t5 = 13, t6 = 14, t7 = 15, t8 = 24, ra = 31 };

constexpr uint32_t rType(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned fn) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
}
constexpr uint32_t iType(unsigned op, unsigned rs, unsigned rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
constexpr uint32_t NOP = 0;
constexpr uint32_t addiu(unsigned rt, unsigned rs, int16_t imm) { return iType(0x09, rs, rt, static_cast<uint16_t>(imm)); }
constexpr uint32_t lui(unsigned rt, uint16_t imm) { return iType(0x0F, 0, rt, imm); }
constexpr uint32_t ori(unsigned rt, unsigned rs, uint16_t imm) { return iType(0x0D, rs, rt, imm); }
constexpr uint32_t beq(unsigned rs, unsigned rt, int16_t off) { return iType(0x04, rs, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t bnel(unsigned rs, unsigned rt, int16_t off) { return iType(0x15, rs, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t jal(uint32_t target) { return (0x03u << 26) | ((target >> 2) & 0x03FFFFFF); }
constexpr uint32_t add(unsigned rd, unsigned rs, unsigned rt) { return rType(rs, rt, rd, 0, 0x20); }
constexpr uint32_t mult(unsigned rs, unsigned rt) { return rType(rs, rt, 0, 0, 0x18); }
constexpr uint32_t div(unsigned rs, unsigned rt) { return rType(rs, rt, 0, 0, 0x1A); }
constexpr uint32_t dmultu(unsigned rs, unsigned rt) { return rType(rs, rt, 0, 0, 0x1D); }
constexpr uint32_t dsll32(unsigned rd, unsigned rt, unsigned sa) { return rType(0, rt, rd, sa, 0x3C); }
constexpr uint32_t dsrl32(unsigned rd, unsigned rt, unsigned sa) { return rType(0, rt, rd, sa, 0x3E); }
constexpr uint32_t dsra32(unsigned rd, unsigned rt, unsigned sa) { return rType(0, rt, rd, sa, 0x3F); }
constexpr uint32_t syscall() { return 0x0C; }
constexpr uint32_t brk() { return 0x0D; }
constexpr uint32_t lb(unsigned rt, int16_t off, unsigned base) { return iType(0x20, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t lh(unsigned rt, int16_t off, unsigned base) { return iType(0x21, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t lwl(unsigned rt, int16_t off, unsigned base) { return iType(0x22, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t sh(unsigned rt, int16_t off, unsigned base) { return iType(0x29, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t lw(unsigned rt, int16_t off, unsigned base) { return iType(0x23, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t lbu(unsigned rt, int16_t off, unsigned base) { return iType(0x24, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t lwr(unsigned rt, int16_t off, unsigned base) { return iType(0x26, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t lwu(unsigned rt, int16_t off, unsigned base) { return iType(0x27, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t sw(unsigned rt, int16_t off, unsigned base) { return iType(0x2B, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t ll(unsigned rt, int16_t off, unsigned base) { return iType(0x30, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t sc(unsigned rt, int16_t off, unsigned base) { return iType(0x38, base, rt, static_cast<uint16_t>(off)); }
constexpr uint32_t mfc0(unsigned rt, unsigned rd) { return (0x10u << 26) | (0u << 21) | (rt << 16) | (rd << 11); }
constexpr uint32_t mtc0(unsigned rt, unsigned rd) { return (0x10u << 26) | (4u << 21) | (rt << 16) | (rd << 11); }
constexpr uint32_t ERET = 0x42000018;
constexpr uint32_t TLBWI = 0x42000002;
constexpr uint32_t TLBP = 0x42000008;

constexpr unsigned FmtS = 16, FmtD = 17, FmtW = 20;
constexpr uint32_t cop1(unsigned fmt, unsigned ft, unsigned fs, unsigned fd, unsigned fn) {
    return (0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn;
}
constexpr uint32_t mtc1(unsigned rt, unsigned fs) { return cop1(4, rt, fs, 0, 0); }
constexpr uint32_t mfc1(unsigned rt, unsigned fs) { return cop1(0, rt, fs, 0, 0); }
constexpr uint32_t dmfc1(unsigned rt, unsigned fs) { return cop1(1, rt, fs, 0, 0); }
constexpr uint32_t bc1t(int16_t off) { return (0x11u << 26) | (8u << 21) | (1u << 16) | static_cast<uint16_t>(off); }
constexpr uint32_t ldc1(unsigned ft, int16_t off, unsigned base) { return iType(0x35, base, ft, static_cast<uint16_t>(off)); }
constexpr uint32_t sdc1(unsigned ft, int16_t off, unsigned base) { return iType(0x3D, base, ft, static_cast<uint16_t>(off)); }

constexpr uint32_t CodePhys = 0x1000;
constexpr uint64_t CodeVirt = 0xFFFFFFFF80001000ull;
constexpr uint32_t DataPhys = 0x2000;
constexpr uint64_t NoSignBits = 0xFFFFFFFFull;

std::vector<uint8_t> makeRomZ64(uint32_t bootAddress);

struct Machine {
    MemoryBus bus;
    VR4300 cpu{bus};

    void load(uint32_t phys, std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) {
            bus.write32(phys, w);
            phys += 4;
        }
    }
    void program(std::initializer_list<uint32_t> words) {
        load(CodePhys, words);
        cpu.setPc(CodeVirt);
    }
    void run(int steps) {
        for (int i = 0; i < steps; ++i) cpu.step();
    }
    uint64_t causeCode() const { return (cpu.cop0(VR4300::Cause) >> 2) & 0x1F; }
};

// --- CPU tests ------------------------------------------------------------

void testImmediates() {
    Machine m;
    m.program({lui(t0, 0x8000), ori(t0, t0, 0x1234), addiu(t1, zero, -1), lui(t2, 0x1234)});
    m.run(4);
    CHECK_EQ(m.cpu.gpr(t0), 0xFFFFFFFF80001234ull);  // LUI sign-extends
    CHECK_EQ(m.cpu.gpr(t1), ~0ull);
    CHECK_EQ(m.cpu.gpr(t2), 0x12340000ull);
}

void testRegisterZeroIsHardwired() {
    Machine m;
    m.program({addiu(zero, zero, 7)});
    m.run(1);
    CHECK_EQ(m.cpu.gpr(0), 0);
}

void testDelaySlot() {
    Machine m;
    m.program({
        addiu(t0, zero, 1),
        beq(zero, zero, 2),   // taken: target = delay slot + 8
        addiu(t1, zero, 5),   // delay slot still executes
        addiu(t2, zero, 9),   // skipped
        addiu(t3, zero, 7),
    });
    m.run(5);
    CHECK_EQ(m.cpu.gpr(t0), 1);
    CHECK_EQ(m.cpu.gpr(t1), 5);
    CHECK_EQ(m.cpu.gpr(t2), 0);
    CHECK_EQ(m.cpu.gpr(t3), 7);
}

void testBranchLikelyNullifiesDelaySlot() {
    Machine m;
    m.program({
        bnel(zero, zero, 2),  // not taken
        addiu(t0, zero, 1),   // nullified
        addiu(t1, zero, 2),
    });
    m.run(2);
    CHECK_EQ(m.cpu.gpr(t0), 0);
    CHECK_EQ(m.cpu.gpr(t1), 2);
}

void testJalLinksPastDelaySlot() {
    Machine m;
    m.program({jal(CodePhys + 0x10), NOP});
    m.load(CodePhys + 0x10, {addiu(t0, zero, 3)});
    m.run(3);
    CHECK_EQ(m.cpu.gpr(ra), CodeVirt + 8);
    CHECK_EQ(m.cpu.gpr(t0), 3);
}

void testLoadStoreIsBigEndian() {
    Machine m;
    m.program({
        lui(t0, 0x8000), ori(t0, t0, 0x2000),
        lui(t1, 0x1234), ori(t1, t1, 0x5678),
        sw(t1, 0, t0),
        lbu(t2, 0, t0), lbu(t3, 3, t0), lh(t4, 0, t0), lw(t5, 0, t0),
        lui(t6, 0x8000), sw(t6, 4, t0), lw(t7, 4, t0), lwu(t8, 4, t0),
    });
    m.run(13);
    CHECK_EQ(m.cpu.gpr(t2), 0x12);
    CHECK_EQ(m.cpu.gpr(t3), 0x78);
    CHECK_EQ(m.cpu.gpr(t4), 0x1234);
    CHECK_EQ(m.cpu.gpr(t5), 0x12345678);
    CHECK_EQ(m.cpu.gpr(t7), 0xFFFFFFFF80000000ull);  // LW sign-extends
    CHECK_EQ(m.cpu.gpr(t8), 0x80000000ull);          // LWU does not
    CHECK_EQ(m.bus.read(DataPhys, 1), 0x12);
}

void testLoadByteSignExtends() {
    Machine m;
    m.bus.write(DataPhys, 1, 0x80);
    m.program({lui(t0, 0x8000), ori(t0, t0, 0x2000), lb(t1, 0, t0)});
    m.run(3);
    CHECK_EQ(m.cpu.gpr(t1), 0xFFFFFFFFFFFFFF80ull);
}

void testUnalignedLoadPair() {
    Machine m;
    m.bus.write(DataPhys, 8, 0x0102030405060708ull);
    m.program({
        lui(t0, 0x8000), ori(t0, t0, 0x2000),
        lwl(t1, 1, t0), lwr(t1, 4, t0),  // word at the unaligned address 0x2001
    });
    m.run(4);
    CHECK_EQ(m.cpu.gpr(t1), 0x02030405);
}

void testMisalignedLoadRaisesAddressError() {
    Machine m;
    m.program({lui(t0, 0x8000), ori(t0, t0, 0x2001), lw(t1, 0, t0)});
    m.run(3);
    CHECK_EQ(m.causeCode(), VR4300::ExcAddressErrorLoad);
    CHECK_EQ(m.cpu.cop0(VR4300::BadVAddr), 0xFFFFFFFF80002001ull);
    CHECK_EQ(m.cpu.pc(), 0xFFFFFFFF80000180ull);
}

void testMultiplyAndDivide() {
    Machine m;
    m.program({addiu(t0, zero, -3), addiu(t1, zero, 7), mult(t0, t1)});
    m.run(3);
    CHECK_EQ(m.cpu.lo(), static_cast<uint64_t>(-21));
    CHECK_EQ(m.cpu.hi(), ~0ull);

    m.program({div(t0, t1)});
    m.run(1);
    CHECK_EQ(m.cpu.lo(), 0);
    CHECK_EQ(m.cpu.hi(), static_cast<uint64_t>(-3));

    m.program({addiu(t0, zero, -1), addiu(t1, zero, 2), dmultu(t0, t1)});
    m.run(3);
    CHECK_EQ(m.cpu.hi(), 1);
    CHECK_EQ(m.cpu.lo(), 0xFFFFFFFFFFFFFFFEull);
}

void testDivideByZeroDoesNotTrap() {
    Machine m;
    m.program({addiu(t0, zero, 5), div(t0, zero)});
    m.run(2);
    CHECK_EQ(m.cpu.lo(), ~0ull);
    CHECK_EQ(m.cpu.hi(), 5);
    CHECK_EQ(m.causeCode(), 0);
}

void testDoublewordShifts() {
    Machine m;
    m.program({
        lui(t0, 1), dsll32(t1, t0, 0),
        addiu(t2, zero, -1), dsll32(t2, t2, 0), dsra32(t3, t2, 0), dsrl32(t4, t2, 0),
    });
    m.run(6);
    CHECK_EQ(m.cpu.gpr(t1), 0x0001000000000000ull);
    CHECK_EQ(m.cpu.gpr(t3), ~0ull);
    CHECK_EQ(m.cpu.gpr(t4), NoSignBits);
}

void testOverflowException() {
    Machine m;
    m.program({lui(t0, 0x7FFF), ori(t0, t0, 0xFFFF), add(t1, t0, t0)});
    m.run(3);
    CHECK_EQ(m.causeCode(), VR4300::ExcOverflow);
    CHECK_EQ(m.cpu.cop0(VR4300::EPC), CodeVirt + 8);
    CHECK_EQ(m.cpu.gpr(t1), 0);
    CHECK_EQ(m.cpu.pc(), 0xFFFFFFFF80000180ull);
    CHECK_EQ(m.cpu.cop0(VR4300::Status) & 2, 2);  // EXL
}

void testExceptionInDelaySlotPointsAtBranch() {
    Machine m;
    m.program({beq(zero, zero, 4), brk()});
    m.run(2);
    CHECK_EQ(m.causeCode(), VR4300::ExcBreakpoint);
    CHECK_EQ(m.cpu.cop0(VR4300::EPC), CodeVirt);
    CHECK_EQ(m.cpu.cop0(VR4300::Cause) >> 31, 1);  // BD
}

void testSyscallAndEret() {
    Machine m;
    m.program({syscall(), addiu(t0, zero, 1)});
    m.load(0x180, {mfc0(t1, VR4300::EPC), addiu(t1, t1, 4), mtc0(t1, VR4300::EPC), ERET});
    m.run(6);
    CHECK_EQ(m.cpu.gpr(t0), 1);
    CHECK_EQ(m.cpu.cop0(VR4300::Status) & 2, 0);  // EXL cleared by ERET
}

void testLoadLinkedStoreConditional() {
    Machine m;
    m.bus.write(DataPhys, 4, 5);
    m.program({
        lui(t0, 0x8000), ori(t0, t0, 0x2000),
        ll(t1, 0, t0), addiu(t1, t1, 1), sc(t1, 0, t0),
    });
    m.run(5);
    CHECK_EQ(m.cpu.gpr(t1), 1);
    CHECK_EQ(m.bus.read(DataPhys, 4), 6);

    // SC without a preceding LL must fail and leave memory alone.
    Machine n;
    n.bus.write(DataPhys, 4, 5);
    n.program({lui(t0, 0x8000), ori(t0, t0, 0x2000), addiu(t1, zero, 9), sc(t1, 0, t0)});
    n.run(4);
    CHECK_EQ(n.cpu.gpr(t1), 0);
    CHECK_EQ(n.bus.read(DataPhys, 4), 5);
}

void testCompareInterrupt() {
    Machine m;
    m.program({
        lui(t0, 0x3400), ori(t0, t0, 0x8001), mtc0(t0, VR4300::Status),  // IE + IM7
        addiu(t1, zero, 4), mtc0(t1, VR4300::Compare),
    });
    m.run(30);  // the rest of RAM is NOPs
    CHECK_EQ(m.cpu.cop0(VR4300::Cause) & (1u << 15), 1u << 15);  // IP7
    CHECK_EQ(m.cpu.cop0(VR4300::Status) & 2, 2);                 // EXL: interrupt taken
    CHECK_EQ((m.cpu.cop0(VR4300::Cause) >> 2) & 0x1F, VR4300::ExcInterrupt);
}

void testFpuSingleArithmetic() {
    Machine m;
    m.program({
        lui(t0, 0x3FC0), lui(t1, 0x4000),  // 1.5f, 2.0f
        mtc1(t0, 0), mtc1(t1, 1),
        cop1(FmtS, 1, 0, 2, 0x00),  // add.s f2,f0,f1
        cop1(FmtS, 1, 0, 3, 0x02),  // mul.s f3,f0,f1
        cop1(FmtS, 1, 0, 4, 0x03),  // div.s f4,f0,f1
        mfc1(t2, 2), mfc1(t3, 3), mfc1(t4, 4),
    });
    m.run(10);
    CHECK_EQ(m.cpu.gpr(t2), 0x40600000);  // 3.5f
    CHECK_EQ(m.cpu.gpr(t3), 0x40400000);  // 3.0f
    CHECK_EQ(m.cpu.gpr(t4), 0x3F400000);  // 0.75f
}

void testFpuConversions() {
    Machine m;
    m.program({
        lui(t0, 0x4060), mtc1(t0, 0),            // 3.5f
        cop1(FmtS, 0, 0, 1, 0x0D),               // trunc.w.s
        cop1(FmtS, 0, 0, 2, 0x0C),               // round.w.s (ties to even)
        cop1(FmtS, 0, 0, 3, 0x0E),               // ceil.w.s
        cop1(FmtS, 0, 0, 4, 0x0F),               // floor.w.s
        mfc1(t1, 1), mfc1(t2, 2), mfc1(t3, 3), mfc1(t4, 4),
        lui(t5, 0xC020), mtc1(t5, 5),            // -2.5f
        cop1(FmtS, 0, 5, 6, 0x0C),               // round.w.s -> -2
        mfc1(t6, 6),
        addiu(t7, zero, 7), mtc1(t7, 8),
        cop1(FmtW, 0, 8, 9, 0x20),               // cvt.s.w
        mfc1(t8, 9),
    });
    m.run(18);
    CHECK_EQ(m.cpu.gpr(t1), 3);
    CHECK_EQ(m.cpu.gpr(t2), 4);
    CHECK_EQ(m.cpu.gpr(t3), 4);
    CHECK_EQ(m.cpu.gpr(t4), 3);
    CHECK_EQ(m.cpu.gpr(t6), 0xFFFFFFFFFFFFFFFEull);
    CHECK_EQ(m.cpu.gpr(t8), 0x40E00000);  // 7.0f
}

void testFpuDoublePrecisionAndMemory() {
    Machine m;
    m.bus.write(DataPhys, 8, 0x4004000000000000ull);      // 2.5
    m.bus.write(DataPhys + 8, 8, 0x3FF8000000000000ull);  // 1.5
    m.program({
        lui(t0, 0x8000), ori(t0, t0, 0x2000),
        ldc1(0, 0, t0), ldc1(2, 8, t0),
        cop1(FmtD, 2, 0, 4, 0x00),  // add.d f4,f0,f2
        sdc1(4, 16, t0),
    });
    m.run(6);
    CHECK_EQ(m.bus.read(DataPhys + 16, 8), 0x4010000000000000ull);  // 4.0
}

void testFpuCompareAndBranch() {
    Machine m;
    m.program({
        lui(t0, 0x3FC0), lui(t1, 0x4000), mtc1(t0, 0), mtc1(t1, 1),  // 1.5f, 2.0f
        cop1(FmtS, 1, 0, 0, 0x3C),  // c.lt.s f0,f1 -> true
        bc1t(2),
        addiu(t2, zero, 1),         // delay slot
        addiu(t3, zero, 9),         // skipped
        addiu(t4, zero, 5),
    });
    m.run(8);
    CHECK_EQ(m.cpu.gpr(t2), 1);
    CHECK_EQ(m.cpu.gpr(t3), 0);
    CHECK_EQ(m.cpu.gpr(t4), 5);
}

void testFpuRegisterPairsWhenFrIsClear() {
    Machine m;
    m.program({
        lui(t0, 0x3000), mtc0(t0, VR4300::Status),  // CU0|CU1, FR=0
        lui(t1, 0x1111), ori(t1, t1, 0x1111),
        lui(t2, 0x2222), ori(t2, t2, 0x2222),
        mtc1(t1, 0), mtc1(t2, 1),                   // odd register is the upper half
        dmfc1(t3, 0),
    });
    m.run(9);
    CHECK_EQ(m.cpu.gpr(t3), 0x2222222211111111ull);
}

void testFpuUnusableWithoutCu1() {
    Machine m;
    m.program({mtc0(zero, VR4300::Status), cop1(FmtS, 0, 0, 0, 0x00)});
    m.run(2);
    CHECK_EQ(m.causeCode(), VR4300::ExcCoprocessorUnusable);
    CHECK_EQ((m.cpu.cop0(VR4300::Cause) >> 28) & 3, 1);
}

// --- TLB ------------------------------------------------------------------

constexpr uint32_t TlbSetup[] = {
    lui(t0, 0x0040), mtc0(t0, VR4300::EntryHi),
    iType(0x09, 0, t1, 0xC7), mtc0(t1, VR4300::EntryLo0),    // pfn 3, V|D|G
    iType(0x09, 0, t1, 0x107), mtc0(t1, VR4300::EntryLo1),   // pfn 4, V|D|G
    mtc0(zero, VR4300::Index), mtc0(zero, VR4300::PageMask), TLBWI,
};

void testTlbMapsPages() {
    Machine m;
    m.bus.write32(0x3004, 0xCAFEBABE);
    m.bus.write32(0x4008, 0x12345678);
    m.program({
        TlbSetup[0], TlbSetup[1], TlbSetup[2], TlbSetup[3], TlbSetup[4], TlbSetup[5], TlbSetup[6],
        TlbSetup[7], TlbSetup[8],
        lw(t2, 4, t0),        // even page -> 0x3000
        lw(t3, 0x1008, t0),   // odd page  -> 0x4000
        TLBP, mfc0(t4, VR4300::Index),
        lui(t5, 0x00A0), mtc0(t5, VR4300::EntryHi), TLBP, mfc0(t6, VR4300::Index),
    });
    m.run(17);
    CHECK_EQ(m.cpu.gpr(t2), 0xFFFFFFFFCAFEBABEull);
    CHECK_EQ(m.cpu.gpr(t3), 0x12345678);
    CHECK_EQ(m.cpu.gpr(t4), 0);                       // probe hit entry 0
    CHECK_EQ(m.cpu.gpr(t6), 0xFFFFFFFF80000000ull);   // probe miss sets P
}

void testTlbMissRaisesRefillException() {
    Machine m;
    m.program({lui(t0, 0x0080), lw(t1, 0, t0)});
    m.run(2);
    CHECK_EQ(m.causeCode(), VR4300::ExcTlbLoad);
    CHECK_EQ(m.cpu.pc(), 0xFFFFFFFF80000000ull);  // refill vector
    CHECK_EQ(m.cpu.cop0(VR4300::BadVAddr), 0x00800000);
    CHECK_EQ(m.cpu.cop0(VR4300::EntryHi) & 0xFFFFE000ull, 0x00800000);
}

void testTlbWriteProtect() {
    Machine m;
    m.program({
        TlbSetup[0], TlbSetup[1],
        iType(0x09, 0, t1, 0xC3), mtc0(t1, VR4300::EntryLo0),    // V|G but not dirty
        TlbSetup[4], TlbSetup[5], TlbSetup[6], TlbSetup[7], TlbSetup[8],
        sw(zero, 4, t0),
    });
    m.run(10);
    CHECK_EQ(m.causeCode(), VR4300::ExcTlbModified);
    CHECK_EQ(m.cpu.pc(), 0xFFFFFFFF80000180ull);  // not a refill: general vector
}

// --- Input -----------------------------------------------------------------

void testKeyboardDefaults() {
    InputMapper in;
    in.keyEvent("X", true);
    in.keyEvent("Return", true);
    CHECK_EQ(in.state(0).buttons, N64Button::A | N64Button::Start);
    in.keyEvent("x", false);  // names are case-insensitive
    CHECK_EQ(in.state(0).buttons, N64Button::Start);

    in.keyEvent("Up", true);
    CHECK_EQ(in.state(0).stickY, 85);
    in.keyEvent("Right", true);
    CHECK_EQ(in.state(0).stickX, 60);  // diagonals stay on the circle
    CHECK_EQ(in.state(0).stickY, 60);
    in.keyEvent("Up", false);
    in.keyEvent("Right", false);
    in.keyEvent("Left", true);
    CHECK_EQ(static_cast<int>(in.state(0).stickX), -85);

    CHECK_EQ(in.state(0).connected, true);   // keyboard is always port 1
    CHECK_EQ(in.state(1).connected, false);
}

void testGamepadPortAssignment() {
    InputMapper in;
    CHECK_EQ(in.padConnected(10), 0);
    CHECK_EQ(in.padConnected(11), 1);
    CHECK_EQ(in.padConnected(12), 2);
    CHECK_EQ(in.padConnected(13), 3);
    CHECK_EQ(in.padConnected(14), -1);   // no free port
    CHECK_EQ(in.padConnected(11), 1);    // reconnect is idempotent
    CHECK_EQ(in.state(1).connected, true);

    in.padDisconnected(11);              // the waiting pad takes the freed port
    CHECK_EQ(in.state(1).connected, true);
    in.padButton(14, "a", true);
    CHECK_EQ(in.state(1).buttons, N64Button::A);
    in.padDisconnected(14);
    CHECK_EQ(in.state(1).connected, false);
}

void testGamepadButtonsAndTriggers() {
    InputMapper in;
    in.padConnected(1);
    in.padButton(1, "a", true);
    in.padButton(1, "dpup", true);
    in.padButton(1, "leftshoulder", true);
    CHECK_EQ(in.state(0).buttons, N64Button::A | N64Button::DUp | N64Button::L);

    in.padAxis(1, "lefttrigger", 0.3f);
    CHECK_EQ(in.state(0).buttons & N64Button::Z, 0);
    in.padAxis(1, "lefttrigger", 1.0f);
    CHECK_EQ(in.state(0).buttons & N64Button::Z, N64Button::Z);

    in.padAxis(1, "rightx", 1.0f);   // right stick doubles as the C buttons
    in.padAxis(1, "righty", -1.0f);
    CHECK_EQ(in.state(0).buttons & (N64Button::CRight | N64Button::CUp), N64Button::CRight | N64Button::CUp);

    in.releaseAll();
    CHECK_EQ(in.state(0).buttons, 0);
}

void testGamepadAnalogStick() {
    InputMapper in;
    in.padConnected(1);
    in.padAxis(1, "leftx", 0.1f);  // inside the deadzone
    CHECK_EQ(static_cast<int>(in.state(0).stickX), 0);

    in.padAxis(1, "leftx", 1.0f);
    in.padAxis(1, "lefty", -1.0f);   // SDL up is negative; N64 up is positive
    CHECK_EQ(static_cast<int>(in.state(0).stickX) > 0, true);
    CHECK_EQ(static_cast<int>(in.state(0).stickY) > 0, true);
    CHECK_EQ(static_cast<int>(in.state(0).stickX) <= 85, true);

    in.padAxis(1, "leftx", 0.5f);
    in.padAxis(1, "lefty", 0.0f);
    CHECK_EQ(static_cast<int>(in.state(0).stickX), 35);  // deadzone rescaled: (0.5-0.15)/0.85*85
}

void testKeyboardAndPadMergeOnPortOne() {
    InputMapper in;
    in.padConnected(1);
    in.padButton(1, "a", true);
    in.keyEvent("Z", true);
    CHECK_EQ(in.state(0).buttons, N64Button::A | N64Button::Z);
}

void testInputConfig() {
    InputMapper in;
    std::vector<std::string> errors;
    const bool ok = in.loadConfig(
        "# comment\n"
        "key.A = Space, Left Shift   # two keys\n"
        "key.B =\n"
        "pad.Z = rightshoulder\n"
        "pad.Deadzone = 0.3\n",
        errors);
    CHECK_EQ(ok, true);
    CHECK_EQ(errors.size(), 0);

    in.keyEvent("X", true);  // old default is gone
    CHECK_EQ(in.state(0).buttons, 0);
    in.keyEvent("Left Shift", true);
    CHECK_EQ(in.state(0).buttons, N64Button::A);
    in.keyEvent("C", true);  // unbound
    CHECK_EQ(in.state(0).buttons, N64Button::A);

    in.padConnected(1);
    in.padButton(1, "rightshoulder", true);
    CHECK_EQ(in.state(0).buttons & N64Button::Z, N64Button::Z);

    in.padAxis(1, "leftx", 0.25f);  // below the new 0.3 deadzone
    CHECK_EQ(static_cast<int>(in.state(0).stickX), 0);
}

void testInputConfigReportsBadLines() {
    InputMapper in;
    std::vector<std::string> errors;
    const bool ok = in.loadConfig(
        "nonsense\n"
        "key.Bogus = X\n"
        "pad.Deadzone = 5\n"
        "joy.A = a\n"
        "key.A = Q\n",  // still applied
        errors);
    CHECK_EQ(ok, false);
    CHECK_EQ(errors.size(), 4);
    CHECK_EQ(errors[0].find("line 1") != std::string::npos, true);
    in.keyEvent("Q", true);
    CHECK_EQ(in.state(0).buttons, N64Button::A);
}

// --- Hardware --------------------------------------------------------------

constexpr uint32_t MiMask = 0x0430000C;
constexpr uint32_t SiDramAddr = 0x04800000;
constexpr uint32_t SiRead64 = 0x04800004;
constexpr uint32_t SiWrite64 = 0x04800010;
constexpr uint32_t ViRegBase = 0x04400000;
constexpr uint32_t AiRegBase = 0x04500000;
constexpr uint32_t PiRegBase = 0x04600000;
constexpr uint32_t SpRegBase = 0x04040000;

constexpr uint32_t maskSet(unsigned intrBit) { return 1u << (2 * intrBit + 1); }  // MI_INTR_MASK write: set bit for interrupt n

void testJoybusReadsControllers() {
    MemoryBus bus;
    ControllerState pad0;
    pad0.connected = true;
    pad0.buttons = N64Button::A | N64Button::Start;
    pad0.stickX = -10;
    pad0.stickY = 20;
    bus.setControllerProvider([&](int port) { return port == 0 ? pad0 : ControllerState{}; });

    uint8_t* ram = bus.rdram();
    const uint8_t block[] = {
        0xFF,                                        // padding
        0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,   // port 1: read buttons
        0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,   // port 2: nobody there
        0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF,         // port 3: info (disconnected)
        0xFE};
    std::memcpy(ram + 0x1000, block, sizeof block);
    ram[0x1000 + 63] = 0x01;  // run the commands

    bus.write32(SiDramAddr, 0x1000);
    bus.write32(SiWrite64, 0);
    bus.write32(SiDramAddr, 0x2000);
    bus.write32(SiRead64, 0);

    CHECK_EQ(ram[0x2000 + 4], 0x90);            // buttons high byte: A|Start
    CHECK_EQ(ram[0x2000 + 5], 0x00);
    CHECK_EQ(ram[0x2000 + 6], 0xF6);            // stick X = -10
    CHECK_EQ(ram[0x2000 + 7], 20);              // stick Y
    CHECK_EQ(ram[0x2000 + 2], 0x04);            // port 1 answered: rx length untouched
    CHECK_EQ(ram[0x2000 + 9], 0x84);            // port 2 did not answer: error flag set
    CHECK_EQ(ram[0x2000 + 63], 0);              // control byte consumed
}

void testJoybusInfoCommand() {
    MemoryBus bus;
    ControllerState pad0;
    pad0.connected = true;
    bus.setControllerProvider([&](int port) { return port == 0 ? pad0 : ControllerState{}; });

    uint8_t* ram = bus.rdram();
    const uint8_t block[] = {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE};
    std::memcpy(ram + 0x1000, block, sizeof block);
    ram[0x1000 + 63] = 0x01;
    bus.write32(SiDramAddr, 0x1000);
    bus.write32(SiWrite64, 0);
    bus.write32(SiDramAddr, 0x2000);
    bus.write32(SiRead64, 0);

    CHECK_EQ(ram[0x2000 + 3], 0x05);  // standard controller
    CHECK_EQ(ram[0x2000 + 4], 0x00);
    CHECK_EQ(ram[0x2000 + 5], 0x02);  // no pak
}

void testSiInterrupt() {
    MemoryBus bus;
    bus.write32(MiMask, maskSet(1));  // enable SI
    bus.write32(SiDramAddr, 0x1000);
    bus.write32(SiRead64, 0);
    CHECK_EQ(bus.interruptPending(), false);  // takes time
    bus.advance(5000);
    CHECK_EQ(bus.interruptPending(), true);
    bus.write32(0x04800018, 0);               // SI_STATUS write acknowledges
    CHECK_EQ(bus.interruptPending(), false);
}

void testViFrameTimingAndInterrupt() {
    MemoryBus bus;
    bus.write32(MiMask, maskSet(3));   // enable VI
    bus.write32(ViRegBase + 0x0C, 2);  // interrupt at half-line 2
    CHECK_EQ(bus.read32(ViRegBase + 0x10), 0);
    bus.advance(3000);
    CHECK_EQ(bus.read32(ViRegBase + 0x10), 1);
    CHECK_EQ(bus.interruptPending(), false);
    bus.advance(3000);
    CHECK_EQ(bus.read32(ViRegBase + 0x10), 2);
    CHECK_EQ(bus.interruptPending(), true);
    bus.write32(ViRegBase + 0x10, 0);  // writing V_CURRENT acknowledges
    CHECK_EQ(bus.interruptPending(), false);

    CHECK_EQ(bus.frameCount(), 0);
    bus.advance(MemoryBus::CpuClockHz / 60);
    CHECK_EQ(bus.frameCount(), 1);
}

void testPiDmaCopiesRomToRdram() {
    std::vector<uint8_t> romData = makeRomZ64(0x80001000);
    for (int i = 0; i < 32; ++i) romData[0x1000 + i] = static_cast<uint8_t>(0xA0 + i);
    auto rom = std::make_shared<Rom>();
    std::string error;
    CHECK_EQ(rom->loadFromMemory(romData, error), true);

    MemoryBus bus;
    bus.attachRom(rom);
    bus.write32(MiMask, maskSet(4));  // enable PI
    bus.write32(PiRegBase + 0x00, 0x5000);
    bus.write32(PiRegBase + 0x04, 0x10001000);
    bus.write32(PiRegBase + 0x0C, 15);  // 16 bytes

    CHECK_EQ(bus.rdram()[0x5000], 0xA0);
    CHECK_EQ(bus.rdram()[0x500F], 0xAF);
    CHECK_EQ(bus.rdram()[0x5010], 0);  // only 16 bytes
    CHECK_EQ(bus.read32(PiRegBase + 0x10) & 1, 1);  // busy
    CHECK_EQ(bus.interruptPending(), false);
    bus.advance(1000);
    CHECK_EQ(bus.interruptPending(), true);
    bus.write32(PiRegBase + 0x10, 2);  // clear interrupt
    CHECK_EQ(bus.interruptPending(), false);
    CHECK_EQ(bus.read32(PiRegBase + 0x10) & 1, 0);
}

void testSpDmaAndNullRsp() {
    MemoryBus bus;
    for (int i = 0; i < 16; ++i) bus.rdram()[0x6000 + i] = static_cast<uint8_t>(i + 1);
    bus.write32(SpRegBase + 0x00, 0x100);   // SP_MEM_ADDR (DMEM)
    bus.write32(SpRegBase + 0x04, 0x6000);  // SP_DRAM_ADDR
    bus.write32(SpRegBase + 0x08, 15);      // RD_LEN: RDRAM -> SP, 16 bytes
    CHECK_EQ(bus.spMem()[0x100], 1);
    CHECK_EQ(bus.spMem()[0x10F], 16);

    bus.write32(SpRegBase + 0x00, 0x1100);  // IMEM
    bus.write32(SpRegBase + 0x04, 0x7000);
    bus.spMem()[0x1100] = 0x42;
    bus.write32(SpRegBase + 0x0C, 7);       // WR_LEN: SP -> RDRAM, 8 bytes
    CHECK_EQ(bus.rdram()[0x7000], 0x42);

    // Starting the RSP finishes at once and interrupts if break-interrupts are on.
    bus.write32(MiMask, maskSet(0));                 // enable SP
    CHECK_EQ(bus.read32(SpRegBase + 0x10) & 1, 1);   // halted at power-on
    bus.write32(SpRegBase + 0x10, (1u << 8) | 1u);   // set intr-on-break, clear halt
    CHECK_EQ(bus.read32(SpRegBase + 0x10) & 3, 3);   // halted + broke again
    CHECK_EQ(bus.interruptPending(), true);
    CHECK_EQ(bus.rspStartRequests(), 1);
}

void testAudioDmaDeliversSamplesAndInterrupts() {
    MemoryBus bus;
    uint32_t gotBytes = 0, gotRate = 0;
    const uint8_t* gotPtr = nullptr;
    bus.setAudioSink([&](const uint8_t* samples, uint32_t bytes, uint32_t rate) {
        gotPtr = samples;
        gotBytes = bytes;
        gotRate = rate;
    });
    bus.write32(MiMask, maskSet(2));  // enable AI
    bus.write32(AiRegBase + 0x10, 1103);  // DAC rate -> ~44.1 kHz
    bus.write32(AiRegBase + 0x00, 0x7000);
    bus.write32(AiRegBase + 0x04, 0x800);  // 2048 bytes = 512 stereo frames

    CHECK_EQ(gotPtr == bus.rdram() + 0x7000, true);
    CHECK_EQ(gotBytes, 0x800);
    CHECK_EQ(gotRate, 48681812u / 1104u);
    CHECK_EQ(bus.read32(AiRegBase + 0x0C) & 0x40000000u, 0x40000000u);  // busy
    CHECK_EQ(bus.interruptPending(), false);

    bus.advance(1200000);  // longer than 512 frames at 44.1 kHz
    CHECK_EQ(bus.interruptPending(), true);
    CHECK_EQ(bus.read32(AiRegBase + 0x0C) & 0x40000000u, 0);
}

void testFramebufferDecoding() {
    MemoryBus bus;
    bus.write32(ViRegBase + 0x08, 4);       // width
    bus.write32(ViRegBase + 0x04, 0x8000);  // origin
    VideoFrame frame;
    CHECK_EQ(bus.captureFrame(frame), false);  // control = 0: video off

    bus.write32(ViRegBase + 0x00, 2);  // 16-bit
    bus.write(0x8000, 2, 0xF801);      // red
    bus.write(0x8002, 2, 0x07C1);      // green
    CHECK_EQ(bus.captureFrame(frame), true);
    CHECK_EQ(frame.width, 4);
    CHECK_EQ(frame.height, 240);
    CHECK_EQ(frame.rgba[0], 255);
    CHECK_EQ(frame.rgba[1], 0);
    CHECK_EQ(frame.rgba[2], 0);
    CHECK_EQ(frame.rgba[3], 255);
    CHECK_EQ(frame.rgba[4], 0);
    CHECK_EQ(frame.rgba[5], 255);

    bus.write32(ViRegBase + 0x00, 3);  // 32-bit
    bus.write32(0x8000, 0x11223344);
    CHECK_EQ(bus.captureFrame(frame), true);
    CHECK_EQ(frame.rgba[0], 0x11);
    CHECK_EQ(frame.rgba[1], 0x22);
    CHECK_EQ(frame.rgba[2], 0x33);
    CHECK_EQ(frame.rgba[3], 255);
}

// A guest program configures the VI through KSEG1 addresses and draws a pixel:
// exercises CPU address translation, MMIO stores and framebuffer decoding together.
void testGuestProgramDrawsThroughVi() {
    Machine m;
    m.program({
        lui(t0, 0xA440),                                       // VI registers, uncached
        addiu(t1, zero, 2), sw(t1, 0x00, t0),                  // CONTROL: 16-bit colour
        ori(t1, zero, 0x8000), sw(t1, 0x04, t0),               // ORIGIN
        addiu(t1, zero, 16), sw(t1, 0x08, t0),                 // WIDTH
        lui(t2, 0xA000), ori(t2, t2, 0x8000),                  // framebuffer, uncached
        addiu(t3, zero, -1), sh(t3, 0, t2),                    // one white pixel
    });
    m.run(12);

    VideoFrame frame;
    CHECK_EQ(m.bus.captureFrame(frame), true);
    CHECK_EQ(frame.width, 16);
    CHECK_EQ(frame.rgba[0], 255);
    CHECK_EQ(frame.rgba[1], 255);
    CHECK_EQ(frame.rgba[2], 255);
    CHECK_EQ(frame.rgba[4], 0);  // next pixel untouched
    CHECK_EQ(m.bus.unmappedAccesses(), 0);
}

void testViInterruptReachesTheCpu() {
    Machine m;
    m.bus.write32(MiMask, maskSet(3));
    m.bus.write32(ViRegBase + 0x0C, 1);  // first half-line after the frame start
    m.program({lui(t0, 0x3400), ori(t0, t0, 0x0401), mtc0(t0, VR4300::Status)});  // IE + IM2
    // A line is ~2976 cycles, so the VI interrupt arrives within ~30 slices.
    for (int i = 0; i < 60; ++i) {
        m.run(100);
        m.bus.advance(100);
        if (m.cpu.cop0(VR4300::Status) & 2) break;  // EXL set: exception taken
    }
    CHECK_EQ(m.cpu.cop0(VR4300::Status) & 2, 2);                         // EXL: interrupt taken
    CHECK_EQ(m.causeCode(), VR4300::ExcInterrupt);
    CHECK_EQ(m.cpu.cop0(VR4300::Cause) & (1u << 10), 1u << 10);          // IP2
    CHECK_EQ(m.cpu.pc() >= 0xFFFFFFFF80000180ull, true);
}

void testCrc32AndCicFallback() {
    const char* text = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(text), 9), 0xCBF43926u);

    std::vector<uint8_t> blank(0x2000, 0);
    CHECK_EQ(detectCic(blank) == Cic::Unknown, true);
    CHECK_EQ(cicSeed(Cic::Unknown), 0x3F);
    CHECK_EQ(cicEntryAdjust(Cic::Nus6103), 0x100000);
}

void testRegionAndFrameLoop() {
    std::vector<uint8_t> rom = makeRomZ64(0x80001000);
    rom[0x3E] = 'P';  // game code ends in the region letter
    const uint32_t spin[] = {beq(zero, zero, -1), NOP};  // branch to itself
    for (int i = 0; i < 2; ++i) {
        for (int b = 0; b < 4; ++b) rom[0x1000 + i * 4 + b] = static_cast<uint8_t>(spin[i] >> (24 - 8 * b));
    }
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "reality64_test_pal.z64";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }

    Emulator emu;
    std::string error;
    CHECK_EQ(emu.loadRom(path.string(), error), true);
    CHECK_EQ(emu.boot(error), true);
    CHECK_EQ(emu.isPal(), true);
    CHECK_EQ(emu.bus().read32(0x300), 0);  // osTvType = PAL
    CHECK_EQ(static_cast<int>(emu.bus().frameRate()), 50);

    const uint64_t steps = emu.runFrame();
    CHECK_EQ(emu.bus().frameCount(), 1);
    const uint64_t cycles = steps * emu.cyclesPerInstruction();
    CHECK_EQ(cycles > 1800000 && cycles < 1900000, true);  // ~93.75M / 50 cycles
    CHECK_EQ(emu.cpu().halted(), false);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// --- ROM / bus / emulator tests --------------------------------------------

std::vector<uint8_t> makeRomZ64(uint32_t bootAddress) {
    std::vector<uint8_t> d(0x2000, 0);
    const uint8_t magic[4] = {0x80, 0x37, 0x12, 0x40};
    std::copy(magic, magic + 4, d.begin());
    for (int i = 0; i < 4; ++i) d[8 + i] = static_cast<uint8_t>(bootAddress >> (24 - 8 * i));
    const std::string title = "TEST ROM            ";
    std::copy(title.begin(), title.begin() + 20, d.begin() + 0x20);
    d[0x3B] = 'N'; d[0x3C] = 'T'; d[0x3D] = 'S'; d[0x3E] = 'E';
    d[0x3F] = 2;
    return d;
}

std::vector<uint8_t> toV64(std::vector<uint8_t> d) {
    for (size_t i = 0; i < d.size(); i += 2) std::swap(d[i], d[i + 1]);
    return d;
}

std::vector<uint8_t> toN64(std::vector<uint8_t> d) {
    for (size_t i = 0; i < d.size(); i += 4) {
        std::swap(d[i], d[i + 3]);
        std::swap(d[i + 1], d[i + 2]);
    }
    return d;
}

void testRomByteOrders() {
    const std::vector<uint8_t> z64 = makeRomZ64(0x80001000);
    const std::vector<uint8_t> variants[] = {z64, toV64(z64), toN64(z64)};
    const RomFormat formats[] = {RomFormat::BigEndian, RomFormat::ByteSwapped, RomFormat::LittleEndian};

    for (int i = 0; i < 3; ++i) {
        Rom rom;
        std::string error;
        const bool ok = rom.loadFromMemory(variants[i], error);
        CHECK_EQ(ok, true);
        if (!ok) continue;
        CHECK_EQ(static_cast<int>(rom.originalFormat()), static_cast<int>(formats[i]));
        CHECK_EQ(rom.header().bootAddress, 0x80001000);
        CHECK_EQ(rom.header().name == "TEST ROM", true);
        CHECK_EQ(rom.header().gameCode == "NTSE", true);
        CHECK_EQ(rom.header().version, 2);
        CHECK_EQ(rom.data() == z64, true);  // always normalised to z64
    }
}

void testRomRejectsGarbage() {
    Rom rom;
    std::string error;
    CHECK_EQ(rom.loadFromMemory(std::vector<uint8_t>(0x2000, 0xAB), error), false);
    CHECK_EQ(rom.loadFromMemory(std::vector<uint8_t>(16, 0), error), false);
}

void testBusMapping() {
    MemoryBus bus;
    auto rom = std::make_shared<Rom>();
    std::string error;
    CHECK_EQ(rom->loadFromMemory(makeRomZ64(0x80001000), error), true);
    bus.attachRom(rom);

    CHECK_EQ(bus.read32(MemoryBus::CartBase), 0x80371240);
    bus.write32(MemoryBus::CartBase, 0);  // ROM is read-only
    CHECK_EQ(bus.read32(MemoryBus::CartBase), 0x80371240);

    bus.write(MemoryBus::SpMemBase + 0x1000, 8, 0x1122334455667788ull);  // IMEM
    CHECK_EQ(bus.read(MemoryBus::SpMemBase + 0x1004, 4), 0x55667788);

    CHECK_EQ(bus.unmappedAccesses(), 1);  // the dropped ROM write
    CHECK_EQ(bus.read32(0x04600000), 0);  // PI_DRAM_ADDR: a real register now
    CHECK_EQ(bus.unmappedAccesses(), 1);
    CHECK_EQ(bus.read32(0x05000000), 0);  // cart domain 2: not modelled
    CHECK_EQ(bus.unmappedAccesses(), 2);

    // Sub-word accesses to a register read the right lane of the word.
    bus.write32(0x04600000, 0x00123456);
    CHECK_EQ(bus.read(0x04600001, 1), 0x12);
    CHECK_EQ(bus.read(0x04600002, 2), 0x3456);
}

void testEmulatorBoot() {
    std::vector<uint8_t> rom = makeRomZ64(0x80001000);
    const uint32_t code[] = {addiu(t0, zero, 42), addiu(t1, t0, 1)};
    for (int i = 0; i < 2; ++i) {
        for (int b = 0; b < 4; ++b) rom[0x1000 + i * 4 + b] = static_cast<uint8_t>(code[i] >> (24 - 8 * b));
    }

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "reality64_test_rom.z64";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }

    Emulator emu;
    std::string error;
    CHECK_EQ(emu.loadRom(path.string(), error), true);
    CHECK_EQ(emu.boot(error), true);
    CHECK_EQ(emu.cpu().pc(), 0xFFFFFFFF80001000ull);
    CHECK_EQ(emu.run(2), 2);
    CHECK_EQ(emu.cpu().gpr(t0), 42);
    CHECK_EQ(emu.cpu().gpr(t1), 43);

    std::error_code ec;
    std::filesystem::remove(path, ec);

    Emulator missing;
    CHECK_EQ(missing.loadRom((path.parent_path() / "does_not_exist.z64").string(), error), false);
}

// --- Settings and save states ---------------------------------------------------

void testSettingsRoundTrip() {
    Settings a;
    a.force60 = false;
    a.cyclesPerInstruction = 3;
    a.smoothScaling = false;
    a.volume = 40;
    a.fullscreen = true;
    a.addRecent("C:\\roms\\one.z64");
    a.addRecent("/home/me/two # with hash.z64");
    a.addRecent("C:\\roms\\one.z64");  // moves to the front instead of duplicating

    CHECK_EQ(a.recent.size(), 2);
    CHECK_EQ(a.recent[0] == "C:\\roms\\one.z64", true);

    Settings b;
    std::vector<std::string> errors;
    CHECK_EQ(b.parse(a.serialize(), errors), true);
    CHECK_EQ(errors.size(), 0);
    CHECK_EQ(b.force60, false);
    CHECK_EQ(b.cyclesPerInstruction, 3);
    CHECK_EQ(b.smoothScaling, false);
    CHECK_EQ(b.volume, 40);
    CHECK_EQ(b.fullscreen, true);
    CHECK_EQ(b.recent == a.recent, true);

    Settings defaults;  // "always 60 fps" is the default
    CHECK_EQ(defaults.force60, true);
}

void testSettingsRecentListIsCappedAndBadLinesReported() {
    Settings s;
    for (int i = 0; i < 10; ++i) s.addRecent("rom" + std::to_string(i) + ".z64");
    CHECK_EQ(s.recent.size(), Settings::MaxRecent);
    CHECK_EQ(s.recent[0] == "rom9.z64", true);
    s.removeRecent("rom9.z64");
    CHECK_EQ(s.recent[0] == "rom8.z64", true);

    Settings t;
    std::vector<std::string> errors;
    const bool ok = t.parse(
        "volume = 500\n"
        "cyclesPerInstruction = 0\n"
        "force60 = maybe\n"
        "colour = blue\n"
        "just text\n"
        "volume = 25\n",  // still applied
        errors);
    CHECK_EQ(ok, false);
    CHECK_EQ(errors.size(), 5);
    CHECK_EQ(t.volume, 25);
    CHECK_EQ(t.cyclesPerInstruction, 2);  // unchanged
}

std::filesystem::path writeRomToTemp(const std::vector<uint8_t>& rom, const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    return path;
}

void putWord(std::vector<uint8_t>& rom, size_t offset, uint32_t word) {
    for (int b = 0; b < 4; ++b) rom[offset + b] = static_cast<uint8_t>(word >> (24 - 8 * b));
}

// A ROM whose game counts forever and mirrors the counter into RDRAM.
std::vector<uint8_t> makeCounterRom(const char* title) {
    std::vector<uint8_t> rom = makeRomZ64(0x80001000);
    for (size_t i = 0; i < 20; ++i) rom[0x20 + i] = i < std::strlen(title) ? static_cast<uint8_t>(title[i]) : ' ';
    const uint32_t code[] = {
        lui(t1, 0x8000), ori(t1, t1, 0x2000),
        addiu(t0, t0, 1),        // loop:
        sw(t0, 0, t1),
        beq(zero, zero, -3),
        NOP,
    };
    for (size_t i = 0; i < 6; ++i) putWord(rom, 0x1000 + i * 4, code[i]);
    return rom;
}

void testSaveStateIsDeterministic() {
    const auto path = writeRomToTemp(makeCounterRom("COUNTER"), "reality64_state_a.z64");
    Emulator emu;
    std::string error;
    CHECK_EQ(emu.loadRom(path.string(), error), true);
    CHECK_EQ(emu.boot(error), true);

    emu.run(100000);
    const std::vector<uint8_t> snapshot = emu.saveState();
    const uint64_t counterAtSave = emu.cpu().gpr(t0);

    emu.run(50000);  // let it diverge from the snapshot
    const uint64_t counterLater = emu.cpu().gpr(t0);
    const uint64_t pcLater = emu.cpu().pc();
    const uint64_t ramLater = emu.bus().read32(0x2000);
    const uint64_t cyclesLater = emu.bus().cycles();
    CHECK_EQ(counterLater > counterAtSave, true);

    CHECK_EQ(emu.loadState(snapshot, error), true);
    CHECK_EQ(emu.cpu().gpr(t0), counterAtSave);  // back at the snapshot

    emu.run(50000);                              // the same 50000 instructions again
    CHECK_EQ(emu.cpu().gpr(t0), counterLater);
    CHECK_EQ(emu.cpu().pc(), pcLater);
    CHECK_EQ(emu.bus().read32(0x2000), ramLater);
    CHECK_EQ(emu.bus().cycles(), cyclesLater);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void testSaveStateRejectsBadInput() {
    const auto pathA = writeRomToTemp(makeCounterRom("GAME A"), "reality64_state_a2.z64");
    const auto pathB = writeRomToTemp(makeCounterRom("GAME B"), "reality64_state_b.z64");
    Emulator a, b;
    std::string error;
    CHECK_EQ(a.loadRom(pathA.string(), error) && a.boot(error), true);
    CHECK_EQ(b.loadRom(pathB.string(), error) && b.boot(error), true);
    a.run(1000);
    std::vector<uint8_t> state = a.saveState();

    CHECK_EQ(b.loadState(state, error), false);  // different game
    CHECK_EQ(error.find("different game") != std::string::npos, true);

    std::vector<uint8_t> truncated(state.begin(), state.end() - 100);
    CHECK_EQ(a.loadState(truncated, error), false);
    CHECK_EQ(error.find("truncated") != std::string::npos, true);

    CHECK_EQ(a.loadState(std::vector<uint8_t>(64, 0xAB), error), false);  // garbage
    CHECK_EQ(a.loadState(std::vector<uint8_t>{}, error), false);          // empty

    CHECK_EQ(a.loadState(state, error), true);  // the real one still loads

    std::error_code ec;
    std::filesystem::remove(pathA, ec);
    std::filesystem::remove(pathB, ec);
}

void testCyclesPerInstructionScalesTime() {
    for (unsigned cpi : {1u, 2u, 4u}) {
        Emulator emu;
        emu.setCyclesPerInstruction(cpi);
        const auto path = writeRomToTemp(makeCounterRom("CPI"), "reality64_cpi.z64");
        std::string error;
        CHECK_EQ(emu.loadRom(path.string(), error) && emu.boot(error), true);
        emu.run(10000);
        CHECK_EQ(emu.bus().cycles(), 10000ull * cpi);  // time advances in cycles
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

void testForcedSixtyHertzForPal() {
    std::vector<uint8_t> rom = makeCounterRom("PAL GAME");
    rom[0x3E] = 'P';
    const auto path = writeRomToTemp(rom, "reality64_pal60.z64");
    std::string error;

    Emulator real;
    CHECK_EQ(real.loadRom(path.string(), error) && real.boot(error), true);
    CHECK_EQ(real.isPal(), true);
    CHECK_EQ(static_cast<int>(real.bus().frameRate()), 50);

    Emulator forced;
    forced.setForcedRefresh(60);
    CHECK_EQ(forced.loadRom(path.string(), error) && forced.boot(error), true);
    CHECK_EQ(forced.isPal(), true);                           // still reported as a PAL cart
    CHECK_EQ(static_cast<int>(forced.bus().frameRate()), 60);  // but displayed at 60 Hz

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

}  // namespace

int main() {
    const TestCase tests[] = {
        {"immediates", testImmediates},
        {"r0 hardwired", testRegisterZeroIsHardwired},
        {"delay slot", testDelaySlot},
        {"branch likely", testBranchLikelyNullifiesDelaySlot},
        {"jal link", testJalLinksPastDelaySlot},
        {"big-endian load/store", testLoadStoreIsBigEndian},
        {"lb sign extension", testLoadByteSignExtends},
        {"lwl/lwr", testUnalignedLoadPair},
        {"address error", testMisalignedLoadRaisesAddressError},
        {"mult/div", testMultiplyAndDivide},
        {"div by zero", testDivideByZeroDoesNotTrap},
        {"doubleword shifts", testDoublewordShifts},
        {"overflow exception", testOverflowException},
        {"delay slot exception", testExceptionInDelaySlotPointsAtBranch},
        {"syscall/eret", testSyscallAndEret},
        {"ll/sc", testLoadLinkedStoreConditional},
        {"compare interrupt", testCompareInterrupt},
        {"fpu single", testFpuSingleArithmetic},
        {"fpu conversions", testFpuConversions},
        {"fpu double + memory", testFpuDoublePrecisionAndMemory},
        {"fpu compare/branch", testFpuCompareAndBranch},
        {"fpu FR=0 pairs", testFpuRegisterPairsWhenFrIsClear},
        {"fpu unusable", testFpuUnusableWithoutCu1},
        {"tlb mapping + probe", testTlbMapsPages},
        {"tlb refill exception", testTlbMissRaisesRefillException},
        {"tlb write protect", testTlbWriteProtect},
        {"keyboard defaults", testKeyboardDefaults},
        {"gamepad ports", testGamepadPortAssignment},
        {"gamepad buttons", testGamepadButtonsAndTriggers},
        {"gamepad analog", testGamepadAnalogStick},
        {"keyboard + pad merge", testKeyboardAndPadMergeOnPortOne},
        {"input config", testInputConfig},
        {"input config errors", testInputConfigReportsBadLines},
        {"joybus buttons", testJoybusReadsControllers},
        {"joybus info", testJoybusInfoCommand},
        {"si interrupt", testSiInterrupt},
        {"vi timing", testViFrameTimingAndInterrupt},
        {"pi dma", testPiDmaCopiesRomToRdram},
        {"sp dma + null rsp", testSpDmaAndNullRsp},
        {"audio dma", testAudioDmaDeliversSamplesAndInterrupts},
        {"framebuffer decode", testFramebufferDecoding},
        {"settings round trip", testSettingsRoundTrip},
        {"settings recent + errors", testSettingsRecentListIsCappedAndBadLinesReported},
        {"save state determinism", testSaveStateIsDeterministic},
        {"save state rejects bad input", testSaveStateRejectsBadInput},
        {"cycles per instruction", testCyclesPerInstructionScalesTime},
        {"forced 60 Hz for PAL", testForcedSixtyHertzForPal},
        {"guest draws via vi", testGuestProgramDrawsThroughVi},
        {"vi interrupt -> cpu", testViInterruptReachesTheCpu},
        {"crc32 + cic", testCrc32AndCicFallback},
        {"pal region + frame loop", testRegionAndFrameLoop},
        {"rom byte orders", testRomByteOrders},
        {"rom rejects garbage", testRomRejectsGarbage},
        {"bus mapping", testBusMapping},
        {"emulator boot", testEmulatorBoot},
    };

    for (const TestCase& t : tests) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
