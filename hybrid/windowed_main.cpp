#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>
#include <chrono>
#include <Windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/memory.h>

#include "ppc_recomp_shared.h"
#include "local_boundaries.h"
#include "translated_base.h"
void rrxx_hybrid_indirect(PPCContext& ctx, uint8_t* base, uint32_t address) {
    ctx.fpscr.InitHost();
    ctx.last_indirect_target = address;
    auto* runtime = rex::Runtime::instance();
    auto* function = runtime && address && !(address & 3)
        ? runtime->function_dispatcher()->GetFunction(address) : nullptr;
    if (!function) {
        REXLOG_ERROR("Windowed host stopped: unresolved indirect call {:08X}, LR {:08X}",
                     address, static_cast<unsigned>(ctx.lr));
        rex::FlushLogging();
        std::fprintf(stderr, "Windowed host stopped: unresolved indirect call %08X, LR %08X\n",
            address, static_cast<unsigned>(ctx.lr));
        std::fflush(stderr);
        std::_Exit(20);
    }
    function(ctx, base);
}

namespace {

constexpr rex::PPCImageInfo kImageInfo{
    static_cast<uint32_t>(PPC_CODE_BASE), static_cast<uint32_t>(PPC_CODE_SIZE),
    static_cast<uint32_t>(PPC_IMAGE_BASE), static_cast<uint32_t>(PPC_IMAGE_SIZE),
    PPCFuncMappings};

class RRXXWindowedApp final : public rex::ReXApp {
 public:
    RRXXWindowedApp(rex::ui::WindowedAppContext& context)
        : ReXApp(context, "Rumble Roses XX", kImageInfo,
                 "<extracted-game-directory> <local-state-directory>") {
        AddPositionalOption("game");
        AddPositionalOption("state");
    }

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& context) {
        return std::make_unique<RRXXWindowedApp>(context);
    }

 protected:
    void OnPreSetup(rex::RuntimeConfig& config) override {
        config.graphics = rex::system::LoadGpuPlugin("xenos", "d3d12");
        if (!config.graphics) {
            std::fprintf(stderr, "Cannot load the Xenos D3D12 plugin\n");
            std::_Exit(3);
        }
        if (!rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "2") ||
            rex::cvar::GetFlagByName("d3d12_pipeline_creation_threads") != "2") {
            std::fprintf(stderr, "Cannot enforce GPU pipeline worker limit\n");
            std::_Exit(12);
        }
    }

    void OnConfigurePaths(rex::PathConfig& paths) override {
        const auto game = GetArgument("game");
        const auto state = GetArgument("state");
        if (!game || !state) return;
        const auto state_root = std::filesystem::absolute(*state);
        paths.game_data_root = std::filesystem::absolute(*game);
        paths.user_data_root = state_root / "user";
        paths.update_data_root = state_root / "update";
        paths.cache_root = state_root / "cache";
        paths.metadata_root = state_root / "metadata";
        paths.config_path = state_root / "rrxx-windowed.toml";
    }

    void OnPostSetup() override {
        auto* heap = runtime()->memory()->LookupHeap(0xE0000000u);
        if (!heap) {
            std::fprintf(stderr, "Physical guest heap is unavailable\n");
            std::_Exit(14);
        }
        rrxx_physical_4k_host_offset = heap->host_address_offset();
        const auto photo_root = runtime()->user_data_root().parent_path() / "photo";
        auto photo_device = std::make_unique<rex::filesystem::HostPathDevice>(
            "\\Device\\RrxxPhoto", photo_root, false, true);
        if (!photo_device->Initialize() ||
            !runtime()->file_system()->RegisterDevice(std::move(photo_device)) ||
            !runtime()->file_system()->RegisterSymbolicLink("PHOTO:",
                                                            "\\Device\\RrxxPhoto")) {
            std::fprintf(stderr, "Cannot mount local PHOTO volume\n");
            std::_Exit(17);
        }
        REXLOG_INFO("Mounted writable PHOTO volume at {}", photo_root.string());
        if (!runtime()->function_dispatcher()->SetFunction(0x8214E948u, sub_8214E948)) {
            std::fprintf(stderr, "Cannot register New Game boundary overlay\n");
            std::_Exit(15);
        }
    }

    bool OnWindowCloseRequested() override {
        rex::FlushLogging();
        std::_Exit(22);
    }
};

}  // namespace

REX_DEFINE_APP(rrxx_windowed, RRXXWindowedApp::Create)
