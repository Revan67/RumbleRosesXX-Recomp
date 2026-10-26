#include <cstdio>
#include <rex/memory.h>

int main() {
    rex::memory::Memory memory;
    if (!memory.Initialize()) return 2;
    constexpr uint32_t guest = 0xA1000000;
    auto* heap = memory.LookupHeap(guest);
    if (!heap || !heap->AllocFixed(guest, 0x10000, 0x10000,
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) return 3;
    auto* cpu = memory.virtual_membase() + guest;
    const uint32_t physical = memory.GetPhysicalAddress(guest);
    if (physical == UINT32_MAX) return 4;
    auto* gpu = memory.TranslatePhysical(physical);
    cpu[0] = 0x12;
    cpu[1] = 0x34;
    if (gpu[0] != 0x12 || gpu[1] != 0x34) return 5;
    gpu[2] = 0x56;
    if (cpu[2] != 0x56) return 6;
    std::puts("PASS: CPU and physical GPU views share bytes in both directions.");
    return 0;
}
