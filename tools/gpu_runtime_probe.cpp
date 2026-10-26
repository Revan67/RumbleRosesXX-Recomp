#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path root(argv[2]);
    for (const auto* name : {"game", "user", "update", "cache", "metadata"})
        std::filesystem::create_directories(root / name);
    HMODULE module = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return 3;
    auto version = reinterpret_cast<rex::system::GpuAbiVersionFn>(
        GetProcAddress(module, rex::system::kGpuAbiVersionSymbol));
    auto create = reinterpret_cast<rex::system::GpuCreateFn>(
        GetProcAddress(module, rex::system::kGpuCreateSymbol));
    if (!version || !create || version() != rex::system::kGpuPluginAbiVersion) return 4;
    rex::system::GpuCreateInfo info{sizeof(info), "d3d12"};
    rex::RuntimeConfig config;
    config.graphics.reset(create(version(), &info));
    if (!config.graphics) return 5;
    rex::Runtime runtime(root / "game", root / "user", root / "update",
                         root / "cache", root / "metadata");
    const auto status = runtime.Setup(std::move(config));
    std::printf("Guest GPU runtime initialization: %08X\n", status);
    std::fflush(stdout);
    bool packet_ok = false;
    if (status == 0) {
        constexpr uint32_t guest = 0xA1000000;
        auto* memory = runtime.memory();
        auto* heap = memory->LookupHeap(guest);
        if (heap->AllocFixed(guest, 0x10000, 0x10000, 3, 3)) {
            const auto physical = memory->GetPhysicalAddress(guest);
            auto* ring = memory->TranslatePhysical(physical);
            auto* result = reinterpret_cast<volatile uint32_t*>(ring + 0x1000);
            *result = 0;
            rex::memory::store_and_swap<uint32_t>(ring, 0xC0013D00);
            rex::memory::store_and_swap<uint32_t>(ring + 4, physical + 0x1000);
            rex::memory::store_and_swap<uint32_t>(ring + 8, 0x12345678);
            auto* gpu = runtime.graphics_system();
            gpu->InitializeRingBuffer(physical, 9);
            // Submit through the guest MMIO handler, as recompiled code does.
            *reinterpret_cast<volatile uint32_t*>(memory->virtual_membase() + 0x7FC80714) = _byteswap_ulong(3);
            for (unsigned attempt = 0; attempt < 2000; ++attempt) {
                if (*result == 0x12345678) { packet_ok = true; break; }
                Sleep(1);
            }
            std::printf("Synthetic PM4 MEM_WRITE: %s\n", packet_ok ? "PASS" : "FAIL");
        }
    }
    runtime.Shutdown();
    std::puts("Runtime shutdown completed");
    return status == 0 && packet_ok ? 0 : 6;
}
