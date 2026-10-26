#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <Windows.h>
#include <rex/system/xthread.h>
#include <rex/system/kernel_state.h>
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <rex/input/input_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/ui/presenter.h>
#include <rex/chrono/clock.h>
#include "gpu_probe.h"
#include "translated_base.h"
#include <rex/system/gpu_plugin.h>
#include <rex/system/function_dispatcher.h>
#include "ppc_recomp_shared.h"
#include "local_boundaries.h"
struct IndirectDiagnostic { uint32_t target, lr, r3, r4, r5; };
static thread_local IndirectDiagnostic last_indirect{};
// Registered last: SDK memory/MMIO handlers get the first opportunity to recover.
// Observe only; do not suppress faults or dump guest memory.
static LONG CALLBACK report_fault(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_FLT_INEXACT_RESULT)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG reports = 0;
    if (InterlockedIncrement(&reports) > 4) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE module = nullptr;
    const auto pc = info->ContextRecord->Rip;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(pc), &module);
    wchar_t path[MAX_PATH] = {};
    if (module) GetModuleFileNameW(module, path, MAX_PATH);
    const auto* name = std::wcsrchr(path, L'\\');
    char message[512] = {};
    const int length = code == EXCEPTION_ACCESS_VIOLATION
        ? std::snprintf(message, sizeof(message), "Access violation: module=%ls RVA=%llX operation=%llu address=%llX\n",
            name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module),
            info->ExceptionRecord->ExceptionInformation[0], info->ExceptionRecord->ExceptionInformation[1])
        : std::snprintf(message, sizeof(message), "Floating-point inexact result: module=%ls RVA=%llX code=%08lX\n",
            name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module), code);
    DWORD written = 0;
    if (length > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
        static_cast<DWORD>(length < sizeof(message) ? length : sizeof(message) - 1), &written, nullptr);
    if (auto* state = rex::runtime::ThreadState::Get()) {
        const auto* context = state->context();
        const int context_length = std::snprintf(message, sizeof(message),
            "Guest fault context: thread=%u LR=%08X SP=%08X r3=%08X r4=%08X r5=%08X r27=%08X r31=%08X\n",
            state->thread_id(), static_cast<unsigned>(context->lr), context->r1.u32,
            context->r3.u32, context->r4.u32, context->r5.u32, context->r27.u32, context->r31.u32);
        if (context_length > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
            static_cast<DWORD>(context_length < sizeof(message) ? context_length : sizeof(message) - 1), &written, nullptr);
        const int call_length = std::snprintf(message, sizeof(message),
            "Last indirect entry: target=%08X caller-LR=%08X r3=%08X r4=%08X r5=%08X\n",
            last_indirect.target, last_indirect.lr, last_indirect.r3, last_indirect.r4, last_indirect.r5);
        if (call_length > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
            static_cast<DWORD>(call_length < sizeof(message) ? call_length : sizeof(message) - 1), &written, nullptr);
        // Small fault-time state sample only; avoid dereferencing unchecked guest pointers.
        uint32_t fields[2] = {};
        SIZE_T read = 0;
        if (ReadProcessMemory(GetCurrentProcess(), state->memory()->TranslateVirtual(context->r27.u32 + 52),
            fields, sizeof(fields), &read) && read == sizeof(fields)) {
            const int field_length = std::snprintf(message, sizeof(message),
                "Fault state sample: record-count=%u destination-storage=%08X working-destination=%08X\n",
                _byteswap_ulong(fields[0]), _byteswap_ulong(fields[1]), context->r30.u32);
            if (field_length > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
                static_cast<DWORD>(field_length < sizeof(message) ? field_length : sizeof(message) - 1), &written, nullptr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void rrxx_observe_scene_call(uint32_t target, uint32_t caller);
void rrxx_hybrid_indirect(PPCContext& ctx, uint8_t* base, uint32_t address) {
    // Guest context activity can replace the cached host control word. Keep
    // guest rounding/flush bits, but reassert the SDK's host exception policy
    // before generated code may write that cache back to MXCSR.
    ctx.fpscr.InitHost();
    rrxx_observe_scene_call(address, uint32_t(ctx.lr));
    last_indirect = {address, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32};
    auto* runtime = rex::Runtime::instance();
    auto* function = runtime && address && !(address & 3)
        ? runtime->function_dispatcher()->GetFunction(address) : nullptr;
    if (!function) {
        std::fprintf(stderr, "Hybrid stopped: unresolved indirect call %08X, LR %08X\n",
            address, static_cast<unsigned>(ctx.lr));
        std::fflush(stderr);
        std::_Exit(20);
    }
    function(ctx, base);
}
// The SDK executable-path helper uses the CRT wide program-name state.
int rrxx_test_generated_fill();
void rrxx_sample_startup_worker();
void rrxx_report_startup_progress();
extern bool rrxx_parser_probe_enabled;
bool rrxx_verbose_probes = true;
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::puts("Hybrid startup: main entered");
    if (argc == 2 && std::wstring_view(argv[1]) == L"--memory-selftest")
        return rrxx_test_generated_fill();
    const bool offscreen_quiet = (argc == 4 || argc == 5) && std::wstring_view(argv[3]) == L"--boot-offscreen-quiet";
    const bool offscreen_clock = (argc == 4 || argc == 5) && std::wstring_view(argv[3]) == L"--boot-offscreen-clock";
    const bool thread_clock_probe = offscreen_clock || (argc == 4 && std::wstring_view(argv[3]) == L"--boot-thread-clock");
    const bool offscreen_probe = offscreen_quiet || offscreen_clock || ((argc == 4 || argc == 5) && std::wstring_view(argv[3]) == L"--boot-offscreen");
    unsigned observation_ticks = 50;
    if (offscreen_probe && argc == 5) {
        wchar_t* end = nullptr;
        const auto seconds = std::wcstoul(argv[4], &end, 10);
        if (!end || *end || seconds < 1 || seconds > 10) return 2;
        observation_ticks = static_cast<unsigned>(seconds) * 10;
    }
    rrxx_parser_probe_enabled = !offscreen_probe;
    rrxx_verbose_probes = !offscreen_quiet;
    const bool boot_probe = offscreen_probe || thread_clock_probe || (argc == 4 && std::wstring_view(argv[3]) == L"--boot-probe");
    const bool load_image = boot_probe || (argc == 4 && std::wstring_view(argv[3]) == L"--load-image");
    if (argc != 3 && !load_image) {
        std::fprintf(stderr, "Usage: rrxx_hybrid <extracted-game-directory> <local-state-directory> [--load-image|--boot-probe|--boot-thread-clock|--boot-offscreen]\n");
        return 2;
    }
    const auto game = std::filesystem::absolute(argv[1]);
    const auto state = std::filesystem::absolute(argv[2]);
    for (const auto* folder : {"user", "update", "cache", "metadata"})
        std::filesystem::create_directories(state / folder);
    std::puts("Hybrid startup: paths ready");
    rex::Runtime runtime(game, state / "user", state / "update", state / "cache", state / "metadata");
    std::puts("Hybrid startup: runtime constructed");
    rex::RuntimeConfig config;
    rex::LogConfig logging;
    logging.log_to_console = true;
    logging.default_level = spdlog::level::info;
    logging.category_levels["apu"] = offscreen_probe ? spdlog::level::info : spdlog::level::debug;
    rex::InitLogging(logging);
    config.kernel_init = rex::kernel::InitializeKernel;
    config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
    auto backend = rex::system::LoadGpuPlugin("xenos", "d3d12");
    if (!backend) return 3;
    if (!rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "2") ||
        rex::cvar::GetFlagByName("d3d12_pipeline_creation_threads") != "2") {
        std::fprintf(stderr, "Cannot enforce GPU pipeline worker limit\n");
        return 12;
    }
    if (offscreen_probe) {
        const auto status = backend->SetupPresentation(nullptr);
        std::printf("Offscreen presentation setup: %08X\n", static_cast<unsigned>(status));
        if (status != 0) return 13;
    }
    auto telemetry = std::make_unique<GpuProbe>(std::move(backend));
    auto* gpu_probe = telemetry.get();
    config.graphics = std::move(telemetry);
    std::puts("Hybrid startup: plugin load returned");
    if (!config.graphics || runtime.Setup(std::move(config)) != 0) return 3;
    std::puts("Hybrid startup: runtime setup complete");
    rrxx_physical_4k_host_offset = runtime.memory()->LookupHeap(0xE0000000u)->host_address_offset();
    auto* dispatcher = runtime.function_dispatcher();
    if (!dispatcher->InitializeFunctionTable(PPC_CODE_BASE, PPC_CODE_SIZE,
        PPC_IMAGE_BASE, PPC_IMAGE_SIZE, true)) return 4;
    std::puts("Hybrid startup: function table initialized");
    size_t count = 0;
    for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping) {
        if (!dispatcher->SetFunction(static_cast<uint32_t>(mapping->guest), mapping->host)) return 5;
        ++count;
    }
    if (!dispatcher->SetFunction(0x8214E948u, sub_8214E948)) return 5;
    std::printf("Registered %zu XenonRecomp functions with SDK context\n", count);
    const auto verify_dispatch = [&]() {
        auto* base = runtime.memory()->virtual_membase();
        for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping) {
            const auto address = static_cast<uint32_t>(mapping->guest);
            if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE ||
                dispatcher->GetFunction(address) != mapping->host ||
                PPC_LOOKUP_FUNC(base, address) != mapping->host) return false;
        }
        return dispatcher->GetFunction(0x8214E948u) == sub_8214E948 &&
               PPC_LOOKUP_FUNC(base, 0x8214E948u) == sub_8214E948;
    };
    if (!verify_dispatch()) {
        std::fprintf(stderr, "Dispatch lookup verification failed before image load\n");
        return 6;
    }
    std::puts("Generated indirect lookup matches SDK dispatch table");
    if (load_image) {
        const auto status = runtime.LoadXexImage("game:/default.xex");
        std::printf("Image load status: %08X\n", static_cast<unsigned>(status));
        if (status != 0) return 7;
        if (!verify_dispatch()) {
            std::fprintf(stderr, "Dispatch lookup verification failed after image load\n");
            return 8;
        }
        std::puts("Image loaded; all registered dispatch entries preserved; no game execution");
    }
    if (boot_probe) {
        const auto host_ticks_before = __rdtsc();
        const auto guest_ticks_before = rex::chrono::Clock::QueryGuestTickCount();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto guest_delta = rex::chrono::Clock::QueryGuestTickCount() - guest_ticks_before;
        const auto host_delta = __rdtsc() - host_ticks_before;
        std::printf("Clock compatibility: host-tick-delta=%llu guest-tick-delta=%llu ratio=%.2f\n",
            host_delta, guest_delta, guest_delta ? double(host_delta) / double(guest_delta) : 0.0);
        if (!AddVectoredExceptionHandler(0, report_fault)) return 11;
        std::puts("Boot probe: preparing guest main thread");
        auto guest = runtime.PrepareModuleLaunch();
        if (!guest) return 9;
        std::printf("Guest main thread: id=%u\n", guest->thread_id());
        std::puts("Boot probe: resuming guest main thread");
        if (guest->Resume() != 0) return 10;
        unsigned readback_samples = 0, readback_changes = 0;
        uint32_t previous = 0, last_address = 0;
        unsigned thread_clock_updates = 0;
        unsigned captures = 0;
        const auto clock_start = std::chrono::steady_clock::now();
        for (unsigned tick = 0; tick < observation_ticks; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (rrxx_verbose_probes && offscreen_probe && tick == 69) {
                rrxx_sample_startup_worker();
                auto* handle = guest->thread();
                auto* state = guest->thread_state();
                uint32_t sp = 0, lr = 0;
                if (handle && state && SuspendThread(handle->native_handle()) != DWORD(-1)) {
                    sp = state->context()->r1.u32;
                    lr = static_cast<uint32_t>(state->context()->lr);
                    if (ResumeThread(handle->native_handle()) == DWORD(-1)) std::_Exit(23);
                }
                std::fprintf(stderr, "Main snapshot: running=%u LR=%08X SP=%08X\n", guest->is_running() ? 1u : 0u, lr, sp);
                for (unsigned frame = 0; frame < 10 && sp; ++frame) {
                    uint32_t parent_raw = 0;
                    uint32_t return_raw = 0;
                    SIZE_T count = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), runtime.memory()->TranslateVirtual(sp), &parent_raw, 4, &count) || count != 4) break;
                    const uint32_t parent = _byteswap_ulong(parent_raw);
                    if (parent <= sp || parent - sp > 0x10000u) break;
                    if (!ReadProcessMemory(GetCurrentProcess(), runtime.memory()->TranslateVirtual(parent - 8), &return_raw, 4, &count) || count != 4) break;
                    const auto caller = _byteswap_ulong(return_raw);
                    if (caller < PPC_CODE_BASE || caller >= PPC_CODE_BASE + PPC_CODE_SIZE) break;
                    std::fprintf(stderr, "Main caller sample: frame=%u return=%08X\n", frame, caller);
                    sp = parent;
                }
            }
            if (offscreen_probe && tick % 10 == 9) {
                if (rrxx_verbose_probes) rrxx_report_startup_progress();
                rex::ui::RawImage image;
                const bool captured = gpu_probe->presenter()->CaptureGuestOutput(image);
                unsigned nonblack = 0;
                uint32_t pixel_hash = 2166136261u;
                if (captured && image.width <= 4096 && image.height <= 4096 &&
                    image.width && image.height && image.stride >= size_t(image.width) * 4 &&
                    image.data.size() >= size_t(image.height - 1) * image.stride + size_t(image.width) * 4) {
                    for (uint32_t y = 0; y < image.height; ++y)
                        for (uint32_t x = 0; x < image.width; ++x) {
                            const size_t offset = size_t(y) * image.stride + size_t(x) * 4;
                            if (offset + 2 < image.data.size() &&
                                (image.data[offset] || image.data[offset + 1] || image.data[offset + 2])) ++nonblack;
                            for (unsigned channel = 0; channel < 3; ++channel)
                                pixel_hash = (pixel_hash ^ image.data[offset + channel]) * 16777619u;
                        }
                    ++captures;
                    if (offscreen_quiet || tick + 1 == observation_ticks) {
                        // Private, ignored diagnostic artifact. BMP stores BGRA top-down.
                        const uint32_t pixels_size = image.width * image.height * 4;
                        char capture_name[32] = {};
                        if (offscreen_quiet)
                            std::snprintf(capture_name, sizeof(capture_name), "offscreen-%02u.bmp", (tick + 1) / 10);
                        else
                            std::snprintf(capture_name, sizeof(capture_name), "offscreen-last.bmp");
                        std::ofstream capture(state / "cache" / capture_name, std::ios::binary);
                        const auto u16 = [&](uint16_t v) { capture.write(reinterpret_cast<const char*>(&v), 2); };
                        const auto u32 = [&](uint32_t v) { capture.write(reinterpret_cast<const char*>(&v), 4); };
                        u16(0x4D42); u32(54 + pixels_size); u32(0); u32(54);
                        u32(40); u32(image.width); u32(0u - image.height);
                        u16(1); u16(32); u32(0); u32(pixels_size);
                        u32(0); u32(0); u32(0); u32(0);
                        for (uint32_t y = 0; y < image.height; ++y)
                            for (uint32_t x = 0; x < image.width; ++x) {
                                const auto* p = image.data.data() + size_t(y) * image.stride + size_t(x) * 4;
                                const char bgra[] = {char(p[2]), char(p[1]), char(p[0]), char(255)};
                                capture.write(bgra, 4);
                            }
                        capture.close();
                        std::printf("Local capture saved=%u\n", capture ? 1u : 0u);
                    }
                }
                std::printf("Offscreen sample: captured=%u width=%u height=%u nonblack=%u RGB-hash=%08X\n",
                    captured ? 1u : 0u, image.width, image.height, nonblack, pixel_hash);
            }
            if (thread_clock_probe) {
                const auto milliseconds = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - clock_start).count());
                auto clock_threads = runtime.kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>();
                for (const auto& thread : clock_threads) {
                    if (!thread->is_guest_thread() || !thread->is_running()) continue;
                    // Existing baseline behavior, tested only by explicit opt-in.
                    auto* field = thread->guest_object<rex::system::X_KTHREAD>()->unk_58;
                    rex::memory::store_and_swap<uint32_t>(field, milliseconds);
                    ++thread_clock_updates;
                }
            }
            const auto address = gpu_probe->writeback_address();
            if (!address || (address & 3)) continue;
            auto* pointer = runtime.memory()->TranslatePhysical(address);
            MEMORY_BASIC_INFORMATION region = {};
            if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
            const auto value = _byteswap_ulong(*reinterpret_cast<volatile uint32_t*>(pointer));
            if (readback_samples && address == last_address && value != previous) ++readback_changes;
            previous = value;
            last_address = address;
            ++readback_samples;
        }
        std::printf("GPU readback: initial=%u samples=%u changes=%u final-index=%u\n",
            gpu_probe->initial_readback(), readback_samples, readback_changes, previous);
        gpu_probe->Report();
        if (offscreen_probe) std::printf("Offscreen captures: %u (images stay in local state/cache)\n", captures);
        gpu_probe->ReportPacketSummary(previous);
        std::printf("Thread-clock experiment: enabled=%u updates=%u\n", thread_clock_probe ? 1u : 0u, thread_clock_updates);
        if (offscreen_quiet) {
            std::puts("Quiet capture complete; process-isolated stop before unverified title teardown");
            std::_Exit(22);
        }
        std::puts("Boot probe: observation window ended; requesting shutdown");
        // Keep handle-backed thread objects alive across SDK registry removal.
        // This snapshot is diagnostic: threads without handles or created later
        // may not be represented, so it cannot authorize runtime destruction.
        auto threads = runtime.kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>();
        runtime.kernel_state()->TerminateTitle();
        unsigned pending = 0, observed = 0;
        for (const auto& thread : threads) {
            if (!thread->is_guest_thread()) continue;
            ++observed;
            auto* handle = thread->thread();
            const auto result = handle ? WaitForSingleObject(handle->native_handle(), 0) : WAIT_FAILED;
            if (result != WAIT_OBJECT_0) {
                ++pending;
                CONTEXT sample = {};
                sample.ContextFlags = CONTEXT_CONTROL;
                bool sampled = false;
                // Suspend only this owned guest thread and resume immediately;
                // no logging, allocation, or symbol lookup while it is paused.
                if (handle && SuspendThread(handle->native_handle()) != DWORD(-1)) {
                    sampled = GetThreadContext(handle->native_handle(), &sample) != FALSE;
                    if (ResumeThread(handle->native_handle()) == DWORD(-1)) std::_Exit(23);
                }
                if (sampled) {
                    HMODULE owner = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(sample.Rip), &owner);
                    std::fprintf(stderr, "Pending thread sample: exe=%u RVA=%llX\n",
                        owner == GetModuleHandleW(nullptr) ? 1u : 0u,
                        sample.Rip - reinterpret_cast<uintptr_t>(owner));
                }
                std::fprintf(stderr, "Guest thread pending: id=%u running=%u host-wait=%lu start=%08X startup=%08X\n",
                    thread->thread_id(), thread->is_running() ? 1u : 0u, result,
                    thread->creation_params()->start_address, thread->creation_params()->xapi_thread_startup);
            }
        }
        std::printf("Guest thread snapshot: %u observed, %u host threads not signaled\n", observed, pending);
        if (guest->is_running()) {
            std::fprintf(stderr, "Boot probe: guest main thread did not drain; refusing live-memory teardown\n");
            std::_Exit(21);
        }
        // The pinned SDK drops guest threads from its registry even if they
        // outlive the cooperative deadline. Main-thread exit is insufficient.
        std::fprintf(stderr, "Boot probe: full guest drain unverified; process-isolated stop, not clean shutdown\n");
        std::_Exit(22);
    }
    runtime.Shutdown();
    std::puts("Hybrid shutdown complete");
    // Keep the diagnostic process alive briefly to expose late thread faults.
    if (boot_probe) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 0;
}
