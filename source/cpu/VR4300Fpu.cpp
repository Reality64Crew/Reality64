#include <cmath>
#include <cstring>

#include "cpu/VR4300.h"

namespace reality64 {

using namespace cpu;

namespace {

enum class Round { Nearest = 0, Trunc = 1, Ceil = 2, Floor = 3 };

float bitsToFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}
uint32_t floatToBits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}
double bitsToDouble(uint64_t v) {
    double d;
    std::memcpy(&d, &v, sizeof d);
    return d;
}
uint64_t doubleToBits(double d) {
    uint64_t v;
    std::memcpy(&v, &d, sizeof v);
    return v;
}

double applyRound(double d, Round mode) {
    switch (mode) {
        case Round::Trunc: return std::trunc(d);
        case Round::Ceil: return std::ceil(d);
        case Round::Floor: return std::floor(d);
        case Round::Nearest: break;
    }
    return std::nearbyint(d);  // host default mode is round-half-to-even
}

// Out-of-range and NaN inputs saturate to the largest positive value, which is
// what the hardware returns for an invalid conversion.
uint32_t toWord(double d, Round mode) {
    const double r = applyRound(d, mode);
    if (std::isnan(r) || r < -2147483648.0 || r >= 2147483648.0) return 0x7FFFFFFFu;
    return static_cast<uint32_t>(static_cast<int32_t>(r));
}

uint64_t toLong(double d, Round mode) {
    const double r = applyRound(d, mode);
    if (std::isnan(r) || r < -9223372036854775808.0 || r >= 9223372036854775808.0) {
        return 0x7FFFFFFFFFFFFFFFull;
    }
    return static_cast<uint64_t>(static_cast<int64_t>(r));
}

bool compareCondition(unsigned cond, bool less, bool equal, bool unordered) {
    return ((cond & 4) && less) || ((cond & 2) && equal) || ((cond & 1) && unordered);
}

}  // namespace

bool VR4300::fpuFr() const { return (cop0_[Status] & StatusFR) != 0; }

// FR=1: 32 independent 64-bit registers. FR=0: 16 64-bit registers, where an
// odd register number addresses the upper half of the even register below it.
uint32_t VR4300::fprWord(unsigned r) const {
    if (fpuFr()) return static_cast<uint32_t>(fpr_[r]);
    return (r & 1) ? static_cast<uint32_t>(fpr_[r & ~1u] >> 32) : static_cast<uint32_t>(fpr_[r]);
}

void VR4300::setFprWord(unsigned r, uint32_t v) {
    if (fpuFr()) {
        fpr_[r] = (fpr_[r] & 0xFFFFFFFF00000000ull) | v;
    } else if (r & 1) {
        fpr_[r & ~1u] = (fpr_[r & ~1u] & 0xFFFFFFFFull) | (static_cast<uint64_t>(v) << 32);
    } else {
        fpr_[r] = (fpr_[r] & 0xFFFFFFFF00000000ull) | v;
    }
}

uint64_t VR4300::fprDouble(unsigned r) const { return fpr_[fpuFr() ? r : (r & ~1u)]; }

void VR4300::setFprDouble(unsigned r, uint64_t v) { fpr_[fpuFr() ? r : (r & ~1u)] = v; }

void VR4300::executeFpuMemory(uint32_t instr, uint64_t addr) {
    if (!(cop0_[Status] & StatusCU1)) {
        coprocessorUnusable(1);
        return;
    }
    const unsigned ft = (instr >> 16) & 31;
    uint64_t v;
    switch (instr >> 26) {
        case 0x31:  // LWC1
            if (readMem(addr, 4, v)) setFprWord(ft, static_cast<uint32_t>(v));
            break;
        case 0x35:  // LDC1
            if (readMem(addr, 8, v)) setFprDouble(ft, v);
            break;
        case 0x39: writeMem(addr, 4, fprWord(ft)); break;     // SWC1
        case 0x3D: writeMem(addr, 8, fprDouble(ft)); break;   // SDC1
    }
}

void VR4300::executeCop1(uint32_t instr) {
    if (!(cop0_[Status] & StatusCU1)) {
        coprocessorUnusable(1);
        return;
    }

    const unsigned fmt = (instr >> 21) & 31;
    const unsigned ft = (instr >> 16) & 31;
    const unsigned fs = (instr >> 11) & 31;
    const unsigned fd = (instr >> 6) & 31;
    const unsigned fn = instr & 0x3F;
    const Round rm = static_cast<Round>(fcsr_ & FcsrRoundMask);

    switch (fmt) {
        case 0x00: gpr_[ft] = sx32(fprWord(fs)); break;               // MFC1
        case 0x01: gpr_[ft] = fprDouble(fs); break;                   // DMFC1
        case 0x02:                                                    // CFC1
            if (fs == 31) gpr_[ft] = sx32(fcsr_);
            else if (fs == 0) gpr_[ft] = 0x00000A00;  // implementation/revision
            else gpr_[ft] = 0;
            break;
        case 0x04: setFprWord(fs, static_cast<uint32_t>(gpr_[ft])); break;  // MTC1
        case 0x05: setFprDouble(fs, gpr_[ft]); break;                       // DMTC1
        case 0x06:                                                          // CTC1
            if (fs == 31) fcsr_ = static_cast<uint32_t>(gpr_[ft]) & FcsrWritable;
            break;

        case 0x08: {  // BC1F / BC1T / BC1FL / BC1TL
            const bool condition = (fcsr_ & FcsrCondition) != 0;
            const bool wantTrue = (ft & 1) != 0;
            const bool likely = (ft & 2) != 0;
            branch(condition == wantTrue, pc_ + (sx16(instr & 0xFFFF) << 2), likely);
            break;
        }

        case 0x10: {  // single precision
            const float a = bitsToFloat(fprWord(fs));
            const float b = bitsToFloat(fprWord(ft));
            switch (fn) {
                case 0x00: setFprWord(fd, floatToBits(a + b)); break;
                case 0x01: setFprWord(fd, floatToBits(a - b)); break;
                case 0x02: setFprWord(fd, floatToBits(a * b)); break;
                case 0x03: setFprWord(fd, floatToBits(a / b)); break;
                case 0x04: setFprWord(fd, floatToBits(std::sqrt(a))); break;
                case 0x05: setFprWord(fd, fprWord(fs) & 0x7FFFFFFFu); break;  // ABS
                case 0x06: setFprWord(fd, fprWord(fs)); break;                 // MOV
                case 0x07: setFprWord(fd, fprWord(fs) ^ 0x80000000u); break;   // NEG
                case 0x08: setFprDouble(fd, toLong(a, Round::Nearest)); break;
                case 0x09: setFprDouble(fd, toLong(a, Round::Trunc)); break;
                case 0x0A: setFprDouble(fd, toLong(a, Round::Ceil)); break;
                case 0x0B: setFprDouble(fd, toLong(a, Round::Floor)); break;
                case 0x0C: setFprWord(fd, toWord(a, Round::Nearest)); break;
                case 0x0D: setFprWord(fd, toWord(a, Round::Trunc)); break;
                case 0x0E: setFprWord(fd, toWord(a, Round::Ceil)); break;
                case 0x0F: setFprWord(fd, toWord(a, Round::Floor)); break;
                case 0x21: setFprDouble(fd, doubleToBits(static_cast<double>(a))); break;  // CVT.D.S
                case 0x24: setFprWord(fd, toWord(a, rm)); break;                           // CVT.W.S
                case 0x25: setFprDouble(fd, toLong(a, rm)); break;                         // CVT.L.S
                default:
                    if (fn >= 0x30) {
                        const bool nan = std::isnan(a) || std::isnan(b);
                        const bool result = compareCondition(fn & 0xF, a < b, a == b, nan);
                        fcsr_ = result ? (fcsr_ | FcsrCondition) : (fcsr_ & ~FcsrCondition);
                    } else {
                        raiseException(ExcReservedInstruction);
                    }
                    break;
            }
            break;
        }

        case 0x11: {  // double precision
            const double a = bitsToDouble(fprDouble(fs));
            const double b = bitsToDouble(fprDouble(ft));
            switch (fn) {
                case 0x00: setFprDouble(fd, doubleToBits(a + b)); break;
                case 0x01: setFprDouble(fd, doubleToBits(a - b)); break;
                case 0x02: setFprDouble(fd, doubleToBits(a * b)); break;
                case 0x03: setFprDouble(fd, doubleToBits(a / b)); break;
                case 0x04: setFprDouble(fd, doubleToBits(std::sqrt(a))); break;
                case 0x05: setFprDouble(fd, fprDouble(fs) & 0x7FFFFFFFFFFFFFFFull); break;
                case 0x06: setFprDouble(fd, fprDouble(fs)); break;
                case 0x07: setFprDouble(fd, fprDouble(fs) ^ 0x8000000000000000ull); break;
                case 0x08: setFprDouble(fd, toLong(a, Round::Nearest)); break;
                case 0x09: setFprDouble(fd, toLong(a, Round::Trunc)); break;
                case 0x0A: setFprDouble(fd, toLong(a, Round::Ceil)); break;
                case 0x0B: setFprDouble(fd, toLong(a, Round::Floor)); break;
                case 0x0C: setFprWord(fd, toWord(a, Round::Nearest)); break;
                case 0x0D: setFprWord(fd, toWord(a, Round::Trunc)); break;
                case 0x0E: setFprWord(fd, toWord(a, Round::Ceil)); break;
                case 0x0F: setFprWord(fd, toWord(a, Round::Floor)); break;
                case 0x20: setFprWord(fd, floatToBits(static_cast<float>(a))); break;  // CVT.S.D
                case 0x24: setFprWord(fd, toWord(a, rm)); break;                       // CVT.W.D
                case 0x25: setFprDouble(fd, toLong(a, rm)); break;                     // CVT.L.D
                default:
                    if (fn >= 0x30) {
                        const bool nan = std::isnan(a) || std::isnan(b);
                        const bool result = compareCondition(fn & 0xF, a < b, a == b, nan);
                        fcsr_ = result ? (fcsr_ | FcsrCondition) : (fcsr_ & ~FcsrCondition);
                    } else {
                        raiseException(ExcReservedInstruction);
                    }
                    break;
            }
            break;
        }

        case 0x14: {  // 32-bit integer source
            const int32_t w = static_cast<int32_t>(fprWord(fs));
            if (fn == 0x20) setFprWord(fd, floatToBits(static_cast<float>(w)));                  // CVT.S.W
            else if (fn == 0x21) setFprDouble(fd, doubleToBits(static_cast<double>(w)));        // CVT.D.W
            else raiseException(ExcReservedInstruction);
            break;
        }

        case 0x15: {  // 64-bit integer source
            const int64_t l = static_cast<int64_t>(fprDouble(fs));
            if (fn == 0x20) setFprWord(fd, floatToBits(static_cast<float>(l)));                  // CVT.S.L
            else if (fn == 0x21) setFprDouble(fd, doubleToBits(static_cast<double>(l)));        // CVT.D.L
            else raiseException(ExcReservedInstruction);
            break;
        }

        default:
            raiseException(ExcReservedInstruction);
            break;
    }
}

}  // namespace reality64
