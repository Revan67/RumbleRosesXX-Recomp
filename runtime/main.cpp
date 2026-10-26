#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "ppc_recomp_shared.h"
#include "kernel_imports.h"
#include "host_window.h"

namespace
{
constexpr uint32_t kEntryPoint = 0x82066F10;
constexpr uint32_t kStackBase = 0x70000000;
constexpr size_t kStackSize = 16 * 1024 * 1024;
constexpr uint32_t kGpuMmioBase = 0x7FC80000;
constexpr size_t kGpuMmioSize = 0x10000;
uint8_t* gGuestBase;

LONG WINAPI ReportGuestFault(EXCEPTION_POINTERS* info)
{
    const auto code = info->ExceptionRecord->ExceptionCode;
    const auto hostAddress = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "RRXX host: exception 0x%08lX at host PC %p (exe RVA 0x%llX)\n",
        code, info->ExceptionRecord->ExceptionAddress,
        static_cast<unsigned long long>(hostAddress - moduleBase));
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
    {
        const auto fault = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        std::fprintf(stderr, "RRXX host: %s address %p",
            info->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
            reinterpret_cast<void*>(fault));
        if (gGuestBase && fault >= reinterpret_cast<uintptr_t>(gGuestBase) &&
            fault < reinterpret_cast<uintptr_t>(gGuestBase) + PPC_MEMORY_SIZE)
        {
            const auto guestAddress = fault - reinterpret_cast<uintptr_t>(gGuestBase);
            std::fprintf(stderr, " (guest 0x%08llX)", guestAddress);
            std::fprintf(stderr, "\n");
            const auto page = fault & ~uintptr_t(0xFFF);
            if (VirtualAlloc(reinterpret_cast<void*>(page), 0x1000, MEM_COMMIT, PAGE_READWRITE))
            {
                std::fprintf(stderr, "RRXX host: demand-committed guest page 0x%08llX\n",
                    guestAddress & ~uintptr_t(0xFFF));
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        std::fprintf(stderr, "\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool Commit(uint8_t* base, uint32_t address, size_t size)
{
    return VirtualAlloc(base + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
}

int main(int argc, char** argv)
{
    AddVectoredExceptionHandler(1, ReportGuestFault);
    const std::filesystem::path imagePath = argc > 1 ? argv[1] : "game/rrxx_image.bin";
    FILE* input = nullptr;
    if (fopen_s(&input, imagePath.string().c_str(), "rb") != 0)
    {
        std::fprintf(stderr, "Missing guest image: %s\n", imagePath.string().c_str());
        return 1;
    }

    std::vector<uint8_t> image(PPC_IMAGE_SIZE);
    const size_t bytesRead = std::fread(image.data(), 1, image.size(), input);
    std::fclose(input);
    if (bytesRead != image.size())
    {
        std::fprintf(stderr, "Guest image has the wrong size: %zu (expected %llu)\n",
            bytesRead, PPC_IMAGE_SIZE);
        return 2;
    }

    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS));
    if (!base)
    {
        std::fprintf(stderr, "Unable to reserve the 4 GiB guest address space (error %lu)\n", GetLastError());
        return 3;
    }
    gGuestBase = base;
    std::fprintf(stderr, "RRXX host: reserved guest memory at %p\n", base);

    const size_t lookupSize = size_t(PPC_CODE_SIZE) * 2 + sizeof(PPCFunc*);
    if (!Commit(base, PPC_IMAGE_BASE, PPC_IMAGE_SIZE + lookupSize) ||
        !Commit(base, kStackBase, kStackSize) ||
        !Commit(base, kGpuMmioBase, kGpuMmioSize))
    {
        std::fprintf(stderr, "Unable to commit guest memory (error %lu)\n", GetLastError());
        VirtualFree(base, 0, MEM_RELEASE);
        return 4;
    }
    std::fprintf(stderr, "RRXX host: committed image, lookup table, and stack\n");

    std::memcpy(base + PPC_IMAGE_BASE, image.data(), image.size());
    // Minimal Xenos MMIO identity/display state used by titles that poll the
    // GPU registers directly rather than installing an interrupt callback.
    PPC_STORE_U32(kGpuMmioBase + 0x3C00, 0x08100748); // RB_EDRAM_TIMING.
    PPC_STORE_U32(kGpuMmioBase + 0x3C04, 0x0000200E); // RB_BC_CONTROL.
    PPC_STORE_U32(kGpuMmioBase + 0x6530, 0x000002D0); // Vertical counter.
    PPC_STORE_U32(kGpuMmioBase + 0x6544, 1);          // Vblank status.
    PPC_STORE_U32(kGpuMmioBase + 0x6584, 0x050002D0); // 1280x720 viewport.
    size_t mappingCount = 0;
    for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping)
    {
        PPC_LOOKUP_FUNC(base, mapping->guest) = mapping->host;
        ++mappingCount;
    }
    std::fprintf(stderr, "RRXX host: installed %zu function mappings\n", mappingCount);
    StartHostWindow(base);

    auto* entry = PPC_LOOKUP_FUNC(base, kEntryPoint);
    if (!entry)
    {
        std::fprintf(stderr, "No recompiled entry point at 0x%08X\n", kEntryPoint);
        VirtualFree(base, 0, MEM_RELEASE);
        return 5;
    }

    PPCContext ctx{};
    ctx.r1.u64 = kStackBase + kStackSize - 0x1000;
    ctx.r13.u64 = kStackBase;
    PPC_STORE_U32(kStackBase + 0x100, kStackBase + 0x400);
    PPC_STORE_U32(kStackBase + 0x400 + 0x14C, 0x80);
    RegisterGuestThreadClock(kStackBase + 0x400 + 0x58);
    std::printf("RRXX host: entering guest code at 0x%08X\n", kEntryPoint);
    entry(ctx, base);
    WaitForGuestThreads();
    std::printf("RRXX host: guest entry point returned (r3=0x%llX)\n", ctx.r3.u64);
    VirtualFree(base, 0, MEM_RELEASE);
    return 0;
}
