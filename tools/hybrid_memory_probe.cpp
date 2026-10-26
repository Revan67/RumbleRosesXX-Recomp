#include "../hybrid/generated_memory.h"
#include <rex/system/xmemory.h>
#include <cstdio>
#include <cstring>

PPC_FUNC_IMPL(rrxx_memory_access_probe) {
    PPC_FUNC_PROLOGUE();
    const auto address = ctx.r3.u32;
    PPC_STORE_U8(address, 0x12);
    PPC_STORE_U16(address + 2, 0x3456);
    PPC_STORE_U32(address + 4, 0x789ABCDE);
    PPC_STORE_U64(address + 8, UINT64_C(0x1122334455667788));
    auto v = simde_mm_set1_epi32(0x12345678);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(base + (address + 16)), v);
    auto loaded = simde_mm_load_si128(reinterpret_cast<simde__m128i*>(base + (address + 16)));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(base + (address + 32)), loaded);
    auto* atomic = reinterpret_cast<uint32_t*>(base + (address + 48));
    *atomic = 7;
    const bool exchanged = __sync_bool_compare_and_swap(atomic, 7, 9);
    auto* atomic64 = reinterpret_cast<uint64_t*>(base + (address + 56));
    *atomic64 = 17;
    const bool exchanged64 = __sync_bool_compare_and_swap(atomic64, UINT64_C(17), UINT64_C(19));
    ctx.r3.u64 = exchanged && exchanged64 && PPC_LOAD_U8(address) == 0x12 &&
        PPC_LOAD_U16(address + 2) == 0x3456 && PPC_LOAD_U32(address + 4) == 0x789ABCDE &&
        PPC_LOAD_U64(address + 8) == UINT64_C(0x1122334455667788);
}
int main() {
    rex::memory::Memory memory;
    if (!memory.Initialize()) return 2;
    rrxx_physical_4k_host_offset = memory.LookupHeap(0xE0000000)->host_address_offset();
    PPCFunc* callback = rrxx_memory_access_probe; // Compile-time raw-base ABI check.
    RRXXTranslatedBase view{memory.virtual_membase()};
    if (static_cast<uint8_t*>(view) != memory.virtual_membase()) return 6;
    for (uint32_t boundary : {0u, 0x7FC80714u, 0xDFFFFFFFu, 0xE0000000u, 0xFFCFFFFFu, 0xFFD00000u, 0xFFFFFFF0u})
        if (view + boundary != memory.TranslateVirtual(boundary)) return 7;
    for (uint32_t address : {0x10000000u, 0xA1000000u, 0xE2000000u}) {
        auto* heap = memory.LookupHeap(address);
        if (!heap->AllocFixed(address, 0x10000, 0x10000, 3, 3)) return 3;
        PPCContext ctx{};
        ctx.r3.u32 = address;
        callback(ctx, memory.virtual_membase());
        auto* expected = memory.TranslateVirtual(address);
        if (ctx.r3.u64 != 1 || expected[0] != 0x12 || expected[4] != 0x78 ||
            expected[8] != 0x11 || std::memcmp(expected + 16, expected + 32, 16) ||
            *reinterpret_cast<uint32_t*>(expected + 48) != 9 ||
            *reinterpret_cast<uint64_t*>(expected + 56) != 19) return 4;
        if (address >= 0xA0000000 && expected != memory.TranslatePhysical(memory.GetPhysicalAddress(address))) {
            if (std::memcmp(expected, memory.TranslatePhysical(memory.GetPhysicalAddress(address)), 64)) return 5;
        }
        std::printf("PASS scalar/vector/atomic and SDK view: %08X\n", address);
    }
    std::printf("PASS raw-base ABI; physical-4K host offset=%u\n", rrxx_physical_4k_host_offset);
    return 0;
}
