#include <Windows.h>
#include <cstdint>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE plugin = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!plugin) {
        std::printf("plugin load failed: %lu\n", GetLastError());
        return 3;
    }
    auto version = reinterpret_cast<uint32_t (*)()>(GetProcAddress(plugin, "rex_gpu_abi_version"));
    auto factory = GetProcAddress(plugin, "rex_gpu_create");
    if (!version || !factory) return 4;
    const uint32_t abi = version();
    std::printf("Plugin loaded; ABI=%u; factory export present\n", abi);
    return abi == 1 ? 0 : 5;
}
