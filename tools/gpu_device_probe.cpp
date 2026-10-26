#include <Windows.h>
#include <cstdio>
#include <memory>
#include <rex/system/gpu_plugin.h>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return 3;
    auto version = reinterpret_cast<rex::system::GpuAbiVersionFn>(
        GetProcAddress(module, rex::system::kGpuAbiVersionSymbol));
    auto create = reinterpret_cast<rex::system::GpuCreateFn>(
        GetProcAddress(module, rex::system::kGpuCreateSymbol));
    if (!version || !create || version() != rex::system::kGpuPluginAbiVersion) return 4;
    rex::system::GpuCreateInfo info{sizeof(info), "d3d12"};
    std::unique_ptr<rex::system::IGraphicsSystem> gpu(create(version(), &info));
    if (!gpu) return 5;
    const auto status = gpu->SetupPresentation(nullptr);
    const bool ready = status == 0 && gpu->has_presentation();
    std::printf("D3D12 initialization status=%08X presenter=%s\n", status, ready ? "ready" : "failed");
    gpu->Shutdown();
    return ready ? 0 : 6;
}
