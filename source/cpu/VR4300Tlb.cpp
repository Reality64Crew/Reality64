#include "cpu/VR4300.h"

namespace reality64 {

using namespace cpu;

namespace {

constexpr uint32_t LoValid = 1u << 1;
constexpr uint32_t LoDirty = 1u << 2;
constexpr uint32_t LoGlobal = 1u << 0;

}  // namespace

void VR4300::raiseTlbException(uint64_t vaddr, Access access, bool refill, bool modified) {
    const uint32_t a = static_cast<uint32_t>(vaddr);
    cop0_[BadVAddr] = vaddr;
    // Context.BadVPN2 and EntryHi.VPN2 tell the refill handler which pair to load.
    cop0_[Context] = (cop0_[Context] & ~0x7FFFF0ull) | ((static_cast<uint64_t>(a >> 13) << 4) & 0x7FFFF0ull);
    cop0_[EntryHi] = sx32((a & 0xFFFFE000u) | static_cast<uint32_t>(cop0_[EntryHi] & 0xFF));

    unsigned code = ExcTlbLoad;
    if (modified) code = ExcTlbModified;
    else if (access == Access::Store) code = ExcTlbStore;
    raiseException(code, 0, refill);
}

bool VR4300::translateTlb(uint64_t vaddr, Access access, uint32_t& paddr) {
    const uint32_t a = static_cast<uint32_t>(vaddr);
    const uint32_t asid = static_cast<uint32_t>(cop0_[EntryHi]) & 0xFF;

    for (const TlbEntry& e : tlb_) {
        const uint32_t vpnMask = ~(e.mask | 0x1FFFu);
        if ((a & vpnMask) != (e.hi & vpnMask)) continue;
        if (!e.global && (e.hi & 0xFF) != asid) continue;

        // The entry maps an even/odd pair of pages; the lowest bit above the
        // page offset picks which half.
        const uint32_t pageSize = (e.mask + 0x2000u) >> 1;
        const uint32_t lo = (a & pageSize) ? e.lo1 : e.lo0;

        if (!(lo & LoValid)) {
            raiseTlbException(vaddr, access, false, false);
            return false;
        }
        if (access == Access::Store && !(lo & LoDirty)) {
            raiseTlbException(vaddr, access, false, true);
            return false;
        }
        const uint32_t pfn = (lo >> 6) & 0xFFFFFu;
        paddr = ((pfn << 12) & ~(pageSize - 1)) | (a & (pageSize - 1));
        return true;
    }

    raiseTlbException(vaddr, access, true, false);
    return false;
}

void VR4300::tlbWrite(unsigned index) {
    if (index >= TlbEntries) return;
    TlbEntry& e = tlb_[index];
    e.mask = static_cast<uint32_t>(cop0_[PageMask]) & 0x01FFE000u;
    e.hi = static_cast<uint32_t>(cop0_[EntryHi]);
    e.lo0 = static_cast<uint32_t>(cop0_[EntryLo0]);
    e.lo1 = static_cast<uint32_t>(cop0_[EntryLo1]);
    // An entry is global only if both halves have G set.
    e.global = (e.lo0 & e.lo1 & LoGlobal) != 0;
}

void VR4300::tlbRead() {
    const unsigned index = static_cast<unsigned>(cop0_[Index] & 0x3F);
    if (index >= TlbEntries) return;
    const TlbEntry& e = tlb_[index];
    cop0_[PageMask] = e.mask;
    cop0_[EntryHi] = sx32(e.hi & ~(e.mask | 0x1F00u));
    cop0_[EntryLo0] = (e.lo0 & ~LoGlobal) | (e.global ? LoGlobal : 0);
    cop0_[EntryLo1] = (e.lo1 & ~LoGlobal) | (e.global ? LoGlobal : 0);
}

void VR4300::tlbProbe() {
    const uint32_t hi = static_cast<uint32_t>(cop0_[EntryHi]);
    const uint32_t asid = hi & 0xFF;
    for (unsigned i = 0; i < TlbEntries; ++i) {
        const TlbEntry& e = tlb_[i];
        const uint32_t vpnMask = ~(e.mask | 0x1FFFu);
        if ((hi & vpnMask) != (e.hi & vpnMask)) continue;
        if (!e.global && (e.hi & 0xFF) != asid) continue;
        cop0_[Index] = i;
        return;
    }
    cop0_[Index] = 0x80000000u;  // P bit: no match
}

}  // namespace reality64
