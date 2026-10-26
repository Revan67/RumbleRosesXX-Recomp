// Synthetic execution of the two leaf boundaries. Private generated bodies and
// input bytes are supplied locally; this test contains no game implementation.
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include "ppc_recomp_shared.h"

static void store32(uint8_t* base, uint32_t address, uint32_t value) {
    value = _byteswap_ulong(value);
    std::memcpy(base + address, &value, 4);
}
static uint32_t load32(uint8_t* base, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, base + address, 4);
    return _byteswap_ulong(value);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t{1} << 32,
        MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return 3;
    std::ifstream image(argv[1], std::ios::binary);
    for (uint32_t address : {0x10000u, 0x20000u, 0x30000u, 0x82000000u,
                             0x820A0000u, 0x82140000u}) {
        if (!VirtualAlloc(base + address, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return 4;
        if (address >= 0x82000000u) {
            image.seekg(address - 0x82000000u);
            image.read(reinterpret_cast<char*>(base + address), 0x10000);
            if (!image) return 5;
        }
    }
    int cases = 0;
    for (uint32_t state : {0u, 1u, 2u, 3u, 4u, 0xFFFFFFFFu}) {
        for (uint32_t selector = 0; selector < 6; ++selector) {
            std::memset(base + 0x20000, 0xCC, 0x1000);
            PPCContext ctx{};
            ctx.r1.u32 = 0x18000;
            ctx.r3.u32 = 0x20000;
            ctx.r4.u32 = selector;
            ctx.r5.u32 = state;
            ctx.lr = 0x12345678;
            sub_8214E878(ctx, base);
            // The case entering the shared store uses r7 initialized to 4;
            // the neighboring case first changes it to 7.
            const uint32_t expectedState[] = {5, 6, 4, 7};
            const uint32_t expectedSelection[] = {99, 105, 112, 118, 125, 131};
            const uint32_t expected = state < 4 ? expectedState[state] : 0xCCCCCCCC;
            const uint32_t expectedShared = state < 4 ? 0 : 0xCCCCCCCC;
            if (load32(base, 0x20000 + 144) != expected ||
                load32(base, 0x20000 + 132) != expectedShared ||
                load32(base, 0x20000 + 176) != expectedSelection[selector] ||
                ctx.r1.u32 != 0x18000 || ctx.lr != 0x12345678) {
                std::fprintf(stderr, "state boundary failed: state=%u selector=%u result=%08X shared=%08X selection=%08X\n",
                    state, selector, load32(base, 0x20000 + 144),
                    load32(base, 0x20000 + 132), load32(base, 0x20000 + 176));
                return 6;
            }
            ++cases;
        }
    }
    for (uint32_t flag : {0u, 1u, 0xFFFFFFFFu}) {
        for (uint32_t index=0; index<16; ++index) base[0x20000 + 240 + index] = uint8_t(index+1);
        store32(base, 0x20000 + 680, flag);
        PPCContext ctx{};
        ctx.r3.u32 = 0x20000;
        ctx.r4.u32 = 0x30000;
        ctx.r1.u32 = 0x18000;
        ctx.lr = 0x12345678;
        sub_820AEC40(ctx, base);
        if (std::memcmp(base + 0x30000, base + 0x20000 + 240, 8) ||
            load32(base, 0x30008) != load32(base, 0x82000BAC) ||
            std::memcmp(base + 0x3000C, base + 0x20000 + 252, 4) ||
            ctx.r1.u32 != 0x18000 || ctx.lr != 0x12345678) return 7;
        ++cases;
    }
    VirtualFree(base, 0, MEM_RELEASE);
    std::printf("PASS: %d synthetic leaf-boundary cases\n", cases);
    return 0;
}
