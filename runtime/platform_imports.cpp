#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <intrin.h>
#include <Xinput.h>

#include "ppc_recomp_shared.h"
#include "kernel_imports.h"
#include "host_window.h"

namespace
{
std::atomic<uint32_t> gRingBufferAddress{0};
std::atomic<uint32_t> gRingBufferSizeLog2{0};
std::atomic<uint32_t> gRingReadPointerWriteback{0};
std::atomic<uint64_t> gSwapCount{0};
const auto gRuntimeStart = std::chrono::steady_clock::now();
std::atomic<uint32_t> gGraphicsInterruptCallback{0};
std::atomic<uint32_t> gGraphicsInterruptUserData{0};
std::atomic<bool> gGpuConsumerStarted{false};
std::atomic<uint32_t> gGpuWriteLogCount{0};
std::atomic<uint32_t> gGpuEventLogCount{0};
std::atomic<uint64_t> gGpuOpcodesLow{0};
std::atomic<uint64_t> gGpuOpcodesHigh{0};
std::array<uint32_t, 0x5000> gGpuRegisters{};

uint32_t ReadGpuRegister(uint32_t index)
{
    return index < gGpuRegisters.size() ? gGpuRegisters[index] : 0;
}

void WriteGpuRegister(uint32_t index, uint32_t value)
{
    if (index < gGpuRegisters.size()) gGpuRegisters[index] = value;
}

bool TryAcquireGuestSpinLock(uint8_t* base, uint32_t address)
{
    if (!address) return false;
    auto* lock = reinterpret_cast<volatile long*>(base + address);
    return _InterlockedCompareExchange(lock, 0x01000000L, 0) == 0;
}

void AcquireGuestSpinLock(uint8_t* base, uint32_t address)
{
    for (uint32_t attempt = 0; attempt < 4096; ++attempt)
    {
        if (TryAcquireGuestSpinLock(base, address)) return;
        if ((attempt & 63u) == 63u) std::this_thread::yield();
    }
    static std::atomic<uint32_t> warnings{0};
    if (warnings.fetch_add(1) < 8)
        std::fprintf(stderr,
            "RRXX sync: bounded spinlock timeout at guest 0x%08X; continuing unlocked\n",
            address);
}

void ReleaseGuestSpinLock(uint8_t* base, uint32_t address)
{
    if (!address) return;
    auto* lock = reinterpret_cast<volatile long*>(base + address);
    _InterlockedExchange(lock, 0);
}

void DispatchGpuInterrupt(uint8_t* base, uint32_t source, uint32_t cpu)
{
    const uint32_t callbackAddress = gGraphicsInterruptCallback.load();
    auto* callback = callbackAddress ? PPC_LOOKUP_FUNC(base, callbackAddress) : nullptr;
    if (!callback) return;

    // Dedicated PCR/KTHREAD view and stack for command-processor callbacks.
    constexpr uint32_t kInterruptStack = 0x7E000000u;
    constexpr uint32_t kInterruptStackSize = 1024 * 1024u;
    constexpr uint32_t kInterruptThreadObject = kInterruptStack + 0x400u;
    PPC_STORE_U32(kInterruptStack + 0x100u, kInterruptThreadObject);
    PPC_STORE_U32(kInterruptThreadObject + 0x14Cu, 0x84u);
    PPC_STORE_U32(kInterruptThreadObject + 0x58u, static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gRuntimeStart).count()));
    PPC_STORE_U8(kInterruptStack + 268u, static_cast<uint8_t>(cpu));
    PPCContext interruptContext{};
    interruptContext.r1.u64 = kInterruptStack + kInterruptStackSize - 0x1000u;
    interruptContext.r13.u64 = kInterruptStack;
    interruptContext.r3.u64 = source;
    interruptContext.r4.u64 = gGraphicsInterruptUserData.load();
    if (source == 1 && interruptContext.r4.u32)
    {
        const uint32_t state = PPC_LOAD_U32(interruptContext.r4.u32 + 10388u);
        std::fprintf(stderr,
            "RRXX GPU: interrupt state=%08X pending=%08X handler=%08X context=%08X lock=%08X cpu=%u\n",
            state, state ? PPC_LOAD_U32(state) : 0,
            state ? PPC_LOAD_U32(state + 16u) : 0,
            state ? PPC_LOAD_U32(state + 20u) : 0,
            PPC_LOAD_U32(interruptContext.r4.u32 + 10392u), cpu);
    }
    callback(interruptContext, base);
    const uint32_t state = interruptContext.r4.u32
        ? PPC_LOAD_U32(interruptContext.r4.u32 + 10388u) : 0;
    std::fprintf(stderr, "RRXX GPU: interrupt callback returned source=%u pending=%08X\n",
        source, state ? PPC_LOAD_U32(state) : 0);
}

uint32_t GpuPhysicalToGuest(uint32_t address)
{
    // Physical allocations used by this title are visible to the CPU through
    // the 0x2xxxxxxx cached virtual alias.
    // Low physical allocations use the cached 0x2xxxxxxx CPU alias. GPU
    // addresses already in a CPU-visible segment (such as 0xCxxxxxxx fence
    // storage) must retain that segment.
    return address < 0x20000000u ? address | 0x20000000u : address;
}

uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3u)
    {
    case 1: return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
    case 2: return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
                   ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
    case 3: return (value << 16) | (value >> 16);
    default: return value;
    }
}

uint32_t GpuLoad32(uint8_t* base, uint32_t address)
{
    const uint32_t raw = *reinterpret_cast<volatile uint32_t*>(
        base + GpuPhysicalToGuest(address & ~3u));
    return GpuSwap(raw, address);
}

void GpuStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t*>(base + GpuPhysicalToGuest(address & ~3u)) =
        GpuSwap(value, address);
}

void ExecuteGpuBuffer(uint8_t* base, uint32_t guestAddress, uint32_t dwordCount,
    uint32_t depth = 0)
{
    if (!guestAddress || !dwordCount || dwordCount > (1u << 20) || depth > 8) return;

    uint32_t cursor = 0;
    while (cursor < dwordCount)
    {
        const uint32_t header = PPC_LOAD_U32(guestAddress + cursor * 4);
        ++cursor;
        if (header == 0) continue;

        const uint32_t type = header >> 30;
        uint32_t count = 0;
        if (type == 0) count = ((header >> 16) & 0x3FFFu) + 1;
        else if (type == 1) count = 2;
        else if (type == 2) continue;
        else count = ((header >> 16) & 0x3FFFu) + 1;
        if (count > dwordCount - cursor) break;

        if (type == 0)
        {
            const uint32_t baseRegister = header & 0x7FFFu;
            const bool writeOneRegister = (header & 0x8000u) != 0;
            for (uint32_t i = 0; i < count; ++i)
                WriteGpuRegister(writeOneRegister ? baseRegister : baseRegister + i,
                    PPC_LOAD_U32(guestAddress + (cursor + i) * 4));
        }
        else if (type == 1)
        {
            WriteGpuRegister(header & 0x7FFu, PPC_LOAD_U32(guestAddress + cursor * 4));
            WriteGpuRegister((header >> 11) & 0x7FFu,
                PPC_LOAD_U32(guestAddress + (cursor + 1) * 4));
        }
        else if (type == 3)
        {
            const uint32_t opcode = (header >> 8) & 0x7Fu;
            auto& opcodeSet = opcode < 64 ? gGpuOpcodesLow : gGpuOpcodesHigh;
            const uint64_t opcodeBit = uint64_t{1} << (opcode & 63u);
            if (!(opcodeSet.fetch_or(opcodeBit) & opcodeBit))
                std::fprintf(stderr, "RRXX GPU: first opcode=%02X count=%u\n", opcode, count);
            if ((opcode == 0x3Fu || opcode == 0x37u) && count >= 2)
            {
                const uint32_t physical = PPC_LOAD_U32(guestAddress + cursor * 4) & ~3u;
                const uint32_t length =
                    PPC_LOAD_U32(guestAddress + (cursor + 1) * 4) & 0xFFFFFu;
                ExecuteGpuBuffer(base, GpuPhysicalToGuest(physical), length, depth + 1);
            }
            else if (opcode == 0x3Du && count >= 2) // PM4_MEM_WRITE.
            {
                uint32_t address = PPC_LOAD_U32(guestAddress + cursor * 4);
                for (uint32_t i = 1; i < count; ++i, address += 4)
                {
                    const uint32_t value = GpuSwap(
                        PPC_LOAD_U32(guestAddress + (cursor + i) * 4), address);
                    *reinterpret_cast<volatile uint32_t*>(
                        base + GpuPhysicalToGuest(address & ~3u)) = value;
                }
            }
            else if (opcode == 0x21u && count >= 3) // PM4_REG_RMW.
            {
                const uint32_t info = PPC_LOAD_U32(guestAddress + cursor * 4);
                const uint32_t andOperand = PPC_LOAD_U32(guestAddress + (cursor + 1) * 4);
                const uint32_t orOperand = PPC_LOAD_U32(guestAddress + (cursor + 2) * 4);
                uint32_t value = ReadGpuRegister(info & 0x1FFFu);
                value &= (info & 0x80000000u)
                    ? ReadGpuRegister(andOperand & 0x1FFFu) : andOperand;
                value |= (info & 0x40000000u)
                    ? ReadGpuRegister(orOperand & 0x1FFFu) : orOperand;
                WriteGpuRegister(info & 0x1FFFu, value);
            }
            else if (opcode == 0x45u && count >= 6) // PM4_COND_WRITE.
            {
                const uint32_t waitInfo = PPC_LOAD_U32(guestAddress + cursor * 4);
                const uint32_t pollAddress = PPC_LOAD_U32(guestAddress + (cursor + 1) * 4);
                const uint32_t reference = PPC_LOAD_U32(guestAddress + (cursor + 2) * 4);
                const uint32_t mask = PPC_LOAD_U32(guestAddress + (cursor + 3) * 4);
                const uint32_t writeAddress = PPC_LOAD_U32(guestAddress + (cursor + 4) * 4);
                const uint32_t writeValue = PPC_LOAD_U32(guestAddress + (cursor + 5) * 4);
                uint32_t value = 0;
                if (waitInfo & 0x10u)
                    value = GpuLoad32(base, pollAddress);
                else
                    value = ReadGpuRegister(pollAddress);
                bool matched = false;
                switch (waitInfo & 7u)
                {
                case 1: matched = (value & mask) < reference; break;
                case 2: matched = (value & mask) <= reference; break;
                case 3: matched = (value & mask) == reference; break;
                case 4: matched = (value & mask) != reference; break;
                case 5: matched = (value & mask) >= reference; break;
                case 6: matched = (value & mask) > reference; break;
                case 7: matched = true; break;
                }
                if (matched && (waitInfo & 0x100u))
                    GpuStore32(base, writeAddress, writeValue);
                else if (matched)
                    WriteGpuRegister(writeAddress, writeValue);
                const uint32_t logIndex = gGpuWriteLogCount.fetch_add(1);
                if (logIndex < 32)
                    std::fprintf(stderr,
                        "RRXX GPU: cond-write info=%08X poll=%08X ref=%08X mask=%08X dst=%08X value=%08X match=%u\n",
                        waitInfo, pollAddress, reference, mask, writeAddress, writeValue,
                        matched ? 1u : 0u);
            }
            else if (opcode == 0x54u && count >= 1) // PM4_INTERRUPT.
            {
                const uint32_t cpuMask = PPC_LOAD_U32(guestAddress + cursor * 4);
                std::fprintf(stderr, "RRXX GPU: command interrupt cpu-mask=%08X\n", cpuMask);
                for (uint32_t cpu = 0; cpu < 6; ++cpu)
                    if (cpuMask & (1u << cpu)) DispatchGpuInterrupt(base, 1, cpu);
            }
            else if ((opcode == 0x58u || opcode == 0x59u) && count >= 3)
            {
                const uint32_t initiator = PPC_LOAD_U32(guestAddress + cursor * 4);
                const uint32_t address = PPC_LOAD_U32(guestAddress + (cursor + 1) * 4);
                const uint32_t supplied = PPC_LOAD_U32(guestAddress + (cursor + 2) * 4);
                const uint32_t value = (initiator & 0x80000000u)
                    ? static_cast<uint32_t>(gSwapCount.load()) : supplied;
                GpuStore32(base, address, value);
                const uint32_t logIndex = gGpuEventLogCount.fetch_add(1);
                if (logIndex < 32)
                    std::fprintf(stderr,
                        "RRXX GPU: event-write op=%02X init=%08X addr=%08X value=%08X\n",
                        opcode, initiator, address, value);
            }
        }
        cursor += count;
    }
}

void StartGpuConsumer(uint8_t* base)
{
    if (gGpuConsumerStarted.exchange(true)) return;
    std::thread([base]
    {
        uint32_t readPointer = 0;
        for (;;)
        {
            UpdateGuestThreadClocks(base, static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - gRuntimeStart).count()));
            const uint32_t writePointer = PPC_LOAD_U32(0x7FC80714);
            if (writePointer != readPointer)
            {
                const uint32_t ring = gRingBufferAddress.load();
                const uint32_t sizeLog2 = gRingBufferSizeLog2.load();
                // Xenos encodes ring size as 2^(size_log2 + 3) bytes.
                const uint32_t capacity = sizeLog2 < 27 ? (1u << (sizeLog2 + 1)) : 0;
                if (ring && capacity)
                {
                    const uint32_t start = readPointer % capacity;
                    const uint32_t end = writePointer % capacity;
                    if (end >= start)
                        ExecuteGpuBuffer(base, GpuPhysicalToGuest(ring) + start * 4, end - start);
                    else
                    {
                        ExecuteGpuBuffer(base, GpuPhysicalToGuest(ring) + start * 4, capacity - start);
                        ExecuteGpuBuffer(base, GpuPhysicalToGuest(ring), end);
                    }
                }
                readPointer = writePointer;
                PPC_STORE_U32(0x7FC80708, writePointer);
                const uint32_t writeback = gRingReadPointerWriteback.load();
                if (writeback)
                    PPC_STORE_U32(GpuPhysicalToGuest(writeback), writePointer);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }).detach();
}

void WriteVideoMode(uint8_t* base, uint32_t address)
{
    if (!address) return;
    for (uint32_t i = 0; i < 48; i += 4) PPC_STORE_U32(address + i, 0);
    PPC_STORE_U32(address + 0, 1280);
    PPC_STORE_U32(address + 4, 720);
    PPC_STORE_U32(address + 8, 0);
    PPC_STORE_U32(address + 12, 1);
    PPC_STORE_U32(address + 16, 1);
    float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, sizeof(refreshBits));
    PPC_STORE_U32(address + 20, refreshBits);
    PPC_STORE_U32(address + 24, 1);
    PPC_STORE_U32(address + 28, 0x4A);
    PPC_STORE_U32(address + 32, 1);
}
}

PPC_FUNC(__imp__XGetLanguage) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__XGetGameRegion) { ctx.r3.u64 = 0xFF; }
PPC_FUNC(__imp__XGetVideoMode) { WriteVideoMode(base, ctx.r3.u32); }
PPC_FUNC(__imp__VdQueryVideoMode) { WriteVideoMode(base, ctx.r3.u32); }
PPC_FUNC(__imp__VdQueryVideoFlags) { ctx.r3.u64 = 3; }
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32) PPC_STORE_U32(ctx.r3.u32, 1); // sRGB.
    if (ctx.r4.u32)
    {
        const float power = 2.2222223f;
        uint32_t bits;
        std::memcpy(&bits, &power, sizeof(bits));
        PPC_STORE_U32(ctx.r4.u32, bits);
    }
}
PPC_FUNC(__imp__VdSetDisplayMode) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    const uint32_t info = ctx.r3.u32;
    if (!info) return;
    std::memset(base + info, 0, 0x58);
    PPC_STORE_U16(info + 0x00, 1280);
    PPC_STORE_U16(info + 0x02, 720);
    PPC_STORE_U32(info + 0x10, 1280); // Scaler source x2.
    PPC_STORE_U32(info + 0x14, 720);  // Scaler source y2.
    PPC_STORE_U32(info + 0x18, 1280); // Scaled output width.
    PPC_STORE_U32(info + 0x1C, 720);  // Scaled output height.
    PPC_STORE_U32(info + 0x20, 1);    // Vertical filter type.
    PPC_STORE_U32(info + 0x30, 1);    // Horizontal filter type.
    PPC_STORE_U16(info + 0x40, 320);
    PPC_STORE_U16(info + 0x42, 180);
    PPC_STORE_U16(info + 0x44, 320);
    PPC_STORE_U16(info + 0x46, 180);
    PPC_STORE_U16(info + 0x48, 1280);
    PPC_STORE_U16(info + 0x4A, 720);
    const float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, sizeof(refreshBits));
    PPC_STORE_U32(info + 0x4C, refreshBits);
    PPC_STORE_U16(info + 0x56, 1280);
}
PPC_FUNC(__imp__VdGetGraphicsAsicID) { ctx.r3.u64 = 0x11; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdShutdownEngines) {}
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    gGraphicsInterruptCallback.store(ctx.r3.u32);
    gGraphicsInterruptUserData.store(ctx.r4.u32);
    std::fprintf(stderr, "RRXX GPU: interrupt callback=0x%08X user=0x%08X\n",
        ctx.r3.u32, ctx.r4.u32);
}
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {}
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    gRingBufferAddress.store(ctx.r3.u32);
    gRingBufferSizeLog2.store(ctx.r4.u32);
    PPC_STORE_U32(0x7FC80700, ctx.r3.u32 >> 12); // CP_RB_BASE.
    PPC_STORE_U32(0x7FC80704, ctx.r4.u32);       // CP_RB_CNTL (bootstrap view).
    PPC_STORE_U32(0x7FC80708, 0);                // CP_RB_RPTR.
    PPC_STORE_U32(0x7FC80714, 0);                // CP_RB_WPTR.
    std::fprintf(stderr, "RRXX GPU: ring buffer guest=0x%08X size=2^%u bytes\n",
        ctx.r3.u32, ctx.r4.u32);
}
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    gRingReadPointerWriteback.store(ctx.r3.u32);
    if (ctx.r3.u32) PPC_STORE_U32(GpuPhysicalToGuest(ctx.r3.u32), 0);
    PPC_STORE_U32(0x7FC8070C, ctx.r3.u32); // CP_RB_RPTR_ADDR.
    StartGpuConsumer(base);
    std::fprintf(stderr, "RRXX GPU: read-pointer writeback guest=0x%08X block=2^%u\n",
        ctx.r3.u32, ctx.r4.u32);
}
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    if (ctx.r3.u32)
    {
        std::memset(base + ctx.r3.u32, 0, 0x94);
        PPC_STORE_U32(ctx.r3.u32, 0xBEEF0000u);
    }
    if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, 0xBEEF0001u);
}
PPC_FUNC(__imp__VdSwap)
{
    // Reserve the 64-dword swap packet region. Rendering will be connected to
    // a host graphics backend later; zero is safe while bootstrapping.
    if (ctx.r3.u32) std::memset(base + ctx.r3.u32, 0, 64 * sizeof(uint32_t));
    const uint64_t frame = gSwapCount.fetch_add(1) + 1;
    if (ctx.r8.u32) UpdateHostFrontBuffer(PPC_LOAD_U32(ctx.r8.u32), 1280, 720);
    if (frame <= 3 || frame % 60 == 0)
    {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gRuntimeStart).count();
        std::fprintf(stderr,
            "RRXX GPU: swap frame %llu at %lldms command=0x%08X fetch=0x%08X\n",
            static_cast<unsigned long long>(frame), static_cast<long long>(elapsedMs),
            ctx.r3.u32, ctx.r4.u32);
        if (frame % 60 == 0 && ctx.r8.u32)
        {
            const uint32_t frontBuffer = PPC_LOAD_U32(ctx.r8.u32);
            uint32_t nonzero = 0;
            uint32_t checksum = 2166136261u;
            for (uint32_t i = 0; i < 4096; ++i)
            {
                const uint8_t value = PPC_LOAD_U8(frontBuffer + i);
                nonzero += value != 0;
                checksum = (checksum ^ value) * 16777619u;
            }
            std::fprintf(stderr,
                "RRXX GPU: frontbuffer sample address=%08X nonzero=%u/4096 checksum=%08X\n",
                frontBuffer, nonzero, checksum);
        }
        if (frame == 1)
        {
            std::fprintf(stderr,
                "RRXX GPU: swap args r5=%08X r6=%08X r7=%08X front=%08X format=%08X color=%08X width_ptr=%08X height_ptr=%08X\n",
                ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                PPC_LOAD_U32(ctx.r1.u32 + 0x54), PPC_LOAD_U32(ctx.r1.u32 + 0x5C));
            if (ctx.r8.u32)
                std::fprintf(stderr, "RRXX GPU: front-buffer guest address=0x%08X\n", PPC_LOAD_U32(ctx.r8.u32));
            if (ctx.r9.u32)
                std::fprintf(stderr, "RRXX GPU: texture format=%u\n", PPC_LOAD_U32(ctx.r9.u32));
            const uint32_t widthPtr = PPC_LOAD_U32(ctx.r1.u32 + 0x54);
            const uint32_t heightPtr = PPC_LOAD_U32(ctx.r1.u32 + 0x5C);
            if (widthPtr && heightPtr)
                std::fprintf(stderr, "RRXX GPU: front-buffer dimensions=%ux%u color-space=%u\n",
                    PPC_LOAD_U32(widthPtr), PPC_LOAD_U32(heightPtr),
                    ctx.r10.u32 ? PPC_LOAD_U32(ctx.r10.u32) : 0);
            if (ctx.r4.u32)
                std::fprintf(stderr, "RRXX GPU: fetch words=%08X %08X %08X %08X %08X %08X\n",
                    PPC_LOAD_U32(ctx.r4.u32 + 0), PPC_LOAD_U32(ctx.r4.u32 + 4),
                    PPC_LOAD_U32(ctx.r4.u32 + 8), PPC_LOAD_U32(ctx.r4.u32 + 12),
                    PPC_LOAD_U32(ctx.r4.u32 + 16), PPC_LOAD_U32(ctx.r4.u32 + 20));
            std::fprintf(stderr,
                "RRXX GPU: CP_RB_BASE=%08X CNTL=%08X RPTR=%08X RPTR_ADDR=%08X WPTR=%08X\n",
                PPC_LOAD_U32(0x7FC80700), PPC_LOAD_U32(0x7FC80704),
                PPC_LOAD_U32(0x7FC80708), PPC_LOAD_U32(0x7FC8070C),
                PPC_LOAD_U32(0x7FC80714));
            const uint32_t ring = gRingBufferAddress.load();
            const uint32_t writePointer = PPC_LOAD_U32(0x7FC80714);
            if (ring)
            {
                const uint32_t dumpCount = std::min<uint32_t>(writePointer, 64);
                std::fprintf(stderr, "RRXX GPU: primary ring (%u dwords):", dumpCount);
                for (uint32_t i = 0; i < dumpCount; ++i)
                {
                    if ((i & 7) == 0) std::fprintf(stderr, "\n  %04X:", i);
                    std::fprintf(stderr, " %08X",
                        PPC_LOAD_U32(GpuPhysicalToGuest(ring) + i * 4));
                }
                std::fputc('\n', stderr);
                for (uint32_t i = 0; i + 2 < dumpCount; ++i)
                {
                    const uint32_t header = PPC_LOAD_U32(GpuPhysicalToGuest(ring) + i * 4);
                    if ((header >> 30) != 3 || ((header >> 8) & 0x7F) != 0x3F) continue;
                    const uint32_t physical =
                        PPC_LOAD_U32(GpuPhysicalToGuest(ring) + (i + 1) * 4) & ~3u;
                    const uint32_t count =
                        PPC_LOAD_U32(GpuPhysicalToGuest(ring) + (i + 2) * 4) & 0xFFFFFu;
                    const uint32_t shown = std::min<uint32_t>(count, 64);
                    std::fprintf(stderr, "RRXX GPU: IB phys=%08X count=%u:", physical, count);
                    for (uint32_t j = 0; j < shown; ++j)
                    {
                        if ((j & 7) == 0) std::fprintf(stderr, "\n  %04X:", j);
                        std::fprintf(stderr, " %08X",
                            PPC_LOAD_U32(GpuPhysicalToGuest(physical) + j * 4));
                    }
                    std::fputc('\n', stderr);
                    i += 2;
                }
            }
        }
    }

    // VdSwap is also the title's vblank synchronization point. Until a host
    // presentation backend supplies real timing, pace it to the reported 60 Hz.
    using Clock = std::chrono::steady_clock;
    static thread_local auto nextVblank = Clock::now();
    nextVblank += std::chrono::microseconds(16667);
    const auto now = Clock::now();
    if (nextVblank > now)
        std::this_thread::sleep_until(nextVblank);
    else
        nextVblank = now;

    // Until a real command processor is connected, consume everything the
    // title has submitted. This mirrors the hardware read pointer advancing
    // and prevents D3D from blocking forever while waiting for ring space.
    const uint32_t writePointer = PPC_LOAD_U32(0x7FC80714);
    PPC_STORE_U32(0x7FC80708, writePointer);
    const uint32_t readPointerWriteback = gRingReadPointerWriteback.load();
    if (readPointerWriteback)
        PPC_STORE_U32(GpuPhysicalToGuest(readPointerWriteback), writePointer);

    const uint32_t callbackAddress = gGraphicsInterruptCallback.load();
    if (callbackAddress)
    {
        if (auto* callback = PPC_LOOKUP_FUNC(base, callbackAddress))
        {
            const PPCContext saved = ctx;
            ctx.r3.u64 = 0;
            ctx.r4.u64 = gGraphicsInterruptUserData.load();
            callback(ctx, base);
            ctx = saved;
        }
    }
}
PPC_FUNC(__imp__VdRetrainEDRAM) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    const uint32_t destination = PPC_LOAD_U32(ctx.r1.u32 + 0x64);
    const uint32_t count = PPC_LOAD_U32(ctx.r1.u32 + 0x6C);
    if (destination)
        for (uint32_t i = 0; i < count; ++i)
            PPC_STORE_U32(destination + i * 4, 0x80000000u);
    ctx.r3.u64 = count;
}
PPC_FUNC(__imp__VdPersistDisplay)
{
    if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, 0);
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__XamInputGetCapabilities)
{
    const uint32_t user = ctx.r3.u32 & 0xFFu;
    XINPUT_CAPABILITIES capabilities{};
    const DWORD result = XInputGetCapabilities(user == 0xFFu ? 0u : user, 0, &capabilities);
    if (ctx.r5.u32)
    {
        std::memset(base + ctx.r5.u32, 0, 20);
        if (result == ERROR_SUCCESS)
        {
            PPC_STORE_U8(ctx.r5.u32 + 0, capabilities.Type);
            PPC_STORE_U8(ctx.r5.u32 + 1, capabilities.SubType);
            PPC_STORE_U16(ctx.r5.u32 + 2, capabilities.Flags);
            PPC_STORE_U16(ctx.r5.u32 + 4, capabilities.Gamepad.wButtons);
        }
    }
    ctx.r3.u64 = result == ERROR_SUCCESS ? 0 : 1167;
}
PPC_FUNC(__imp__XamInputGetState)
{
    const uint32_t user = ctx.r3.u32 & 0xFFu;
    XINPUT_STATE state{};
    const DWORD result = XInputGetState(user == 0xFFu ? 0u : user, &state);
    if (ctx.r5.u32)
    {
        std::memset(base + ctx.r5.u32, 0, 16);
        if (result == ERROR_SUCCESS)
        {
            PPC_STORE_U32(ctx.r5.u32 + 0, state.dwPacketNumber);
            PPC_STORE_U16(ctx.r5.u32 + 4, state.Gamepad.wButtons);
            PPC_STORE_U8(ctx.r5.u32 + 6, state.Gamepad.bLeftTrigger);
            PPC_STORE_U8(ctx.r5.u32 + 7, state.Gamepad.bRightTrigger);
            PPC_STORE_U16(ctx.r5.u32 + 8, static_cast<uint16_t>(state.Gamepad.sThumbLX));
            PPC_STORE_U16(ctx.r5.u32 + 10, static_cast<uint16_t>(state.Gamepad.sThumbLY));
            PPC_STORE_U16(ctx.r5.u32 + 12, static_cast<uint16_t>(state.Gamepad.sThumbRX));
            PPC_STORE_U16(ctx.r5.u32 + 14, static_cast<uint16_t>(state.Gamepad.sThumbRY));
        }
    }
    ctx.r3.u64 = result == ERROR_SUCCESS ? 0 : 1167;
}
PPC_FUNC(__imp__XamInputSetState)
{
    XINPUT_VIBRATION vibration{};
    if (ctx.r5.u32)
    {
        vibration.wLeftMotorSpeed = PPC_LOAD_U16(ctx.r5.u32 + 0);
        vibration.wRightMotorSpeed = PPC_LOAD_U16(ctx.r5.u32 + 2);
    }
    const uint32_t user = ctx.r3.u32 & 0xFFu;
    ctx.r3.u64 = XInputSetState(user == 0xFFu ? 0u : user, &vibration) == ERROR_SUCCESS
        ? 0 : 1167;
}
PPC_FUNC(__imp__DbgBreakPoint) {}
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__NetDll_XNetStartup)
{
    // Offline host: accept the title's XNet configuration without starting a
    // real network service. The Xbox API returns zero on success.
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_WSAStartup)
{
    // r3 is the calling module, r4 is the requested version, r5 is X_WSADATA.
    const uint32_t data = ctx.r5.u32;
    if (data)
    {
        std::memset(base + data, 0, 0x194);
        PPC_STORE_U16(data + 0x000, ctx.r4.u16);
        PPC_STORE_U16(data + 0x002, ctx.r4.u16);
        PPC_STORE_U16(data + 0x186, 100);
        PPC_STORE_U16(data + 0x188, 1024);
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    if (ctx.r4.u32) std::memset(base + ctx.r4.u32, 0, 36);
    ctx.r3.u64 = 0; // XNET_GET_XNADDR_NONE (offline).
}
PPC_FUNC(__imp__NetDll_XNetXnAddrToMachineId)
{
    if (ctx.r5.u32) PPC_STORE_U64(ctx.r5.u32, 0);
    ctx.r3.u64 = 1; // Not signed in to Xbox Live.
}

PPC_FUNC(__imp__XamNotifyCreateListener) { ctx.r3.u64 = 0x70000001u; }
PPC_FUNC(__imp__XNotifyGetNext)
{
    static std::atomic<uint32_t> initialNotification{0};
    const uint32_t sequence = initialNotification.fetch_add(1);
    uint32_t id = 0;
    uint32_t parameter = 0;
    if (sequence == 0) { id = 0x0000000A; parameter = 1; } // Sign-in changed.
    if (sequence == 1) { id = 0x00000009; parameter = 0; } // Guide UI closed.
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, id);
    if (ctx.r6.u32) PPC_STORE_U32(ctx.r6.u32, parameter);
    ctx.r3.u64 = id != 0;
}
PPC_FUNC(__imp__XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; }
PPC_FUNC(__imp__XamUserGetXUID)
{
    if (ctx.r3.u32 || !ctx.r5.u32) { ctx.r3.u64 = 0x80070525u; return; }
    PPC_STORE_U64(ctx.r5.u32, 0xE000000000000001ull);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamUserGetName)
{
    if (ctx.r3.u32 || !ctx.r4.u32 || !ctx.r5.u32) { ctx.r3.u64 = 87; return; }
    const char name[] = "Player";
    const size_t count = (std::min)(sizeof(name), size_t(ctx.r5.u32));
    std::memcpy(base + ctx.r4.u32, name, count);
    if (count == ctx.r5.u32) base[ctx.r4.u32 + count - 1] = 0;
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    const uint32_t capacity = ctx.r4.u32 / 2;
    const uint32_t count = (std::min)(capacity, ctx.r7.u32);
    for (uint32_t i = 0; i < count; ++i)
        PPC_STORE_U16(ctx.r3.u32 + i * 2, base[ctx.r6.u32 + i]);
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, count * 2);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMsgInProcessCall) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XMsgStartIORequest) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeSetAffinityThread) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeEnterCriticalRegion) {}
PPC_FUNC(__imp__KeLeaveCriticalRegion) {}
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    if (ctx.r3.u32) AcquireGuestSpinLock(base, ctx.r3.u32);
}
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    ReleaseGuestSpinLock(base, ctx.r3.u32);
}
PPC_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql)
{
    ctx.r3.u64 = TryAcquireGuestSpinLock(base, ctx.r3.u32) ? 1 : 0;
}
PPC_FUNC(__imp__KfAcquireSpinLock)
{
    const uint32_t address = ctx.r3.u32;
    if (address) AcquireGuestSpinLock(base, address);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KfReleaseSpinLock)
{
    ReleaseGuestSpinLock(base, ctx.r3.u32);
}
