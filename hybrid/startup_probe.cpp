#include "context.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

extern "C" void __imp__sub_8209AD18(PPCContext&, uint8_t*);
static std::atomic<uint32_t> startup_worker_handle{0};
static std::atomic<unsigned> barrier_entries{0}, barrier_returns{0};
static std::atomic<uint32_t> barrier_event{0};
static std::atomic<uint32_t> startup_worker_id{0};
static std::atomic<unsigned> startup_entries{0}, startup_returns{0};
extern "C" void __imp__sub_820666F0(PPCContext&, uint8_t*);

extern "C" void sub_820666F0(PPCContext& ctx, uint8_t* base) {
    const bool observe = uint32_t(ctx.lr) == 0x8209B0CCu;
    const auto* state = rex::runtime::ThreadState::Get();
    const bool startup = observe && state && state->thread_id() == startup_worker_id.load();
    if (observe) { barrier_event.store(ctx.r3.u32); ++barrier_entries; }
    if (startup) ++startup_entries;
    __imp__sub_820666F0(ctx, base);
    if (observe) ++barrier_returns;
    if (startup) ++startup_returns;
}

void rrxx_report_startup_progress() {
    std::fprintf(stderr, "Barrier progress: last-event=%08X all-entries=%u all-returns=%u startup-entries=%u startup-returns=%u\n",
        barrier_event.load(), barrier_entries.load(), barrier_returns.load(), startup_entries.load(), startup_returns.load());
}

static bool read_word(uint32_t address, uint32_t& value) {
    uint32_t raw = 0;
    SIZE_T count = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), rex::Runtime::instance()->memory()->TranslateVirtual(address),
        &raw, 4, &count) || count != 4) return false;
    value = _byteswap_ulong(raw);
    return true;
}

// Forward startup/resume unchanged; retain only the handle needed for observation.
extern "C" void sub_8209AD18(PPCContext& ctx, uint8_t* base) {
    if (uint32_t(ctx.lr) == 0x8206525Cu) {
        uint32_t handle = 0, event = 0;
        if (read_word(ctx.r3.u32 + 8, handle) && read_word(ctx.r3.u32 + 20, event)) {
            startup_worker_handle.store(handle);
            const auto worker = rex::Runtime::instance()->kernel_state()->object_table()->LookupObject<rex::system::XThread>(handle);
            if (worker) startup_worker_id.store(worker->thread_id());
            std::fprintf(stderr, "Startup relationship: worker-handle=%08X event-handle=%08X\n", handle, event);
        }
    }
    __imp__sub_8209AD18(ctx, base);
}

void rrxx_sample_startup_worker() {
    const auto worker = rex::Runtime::instance()->kernel_state()->object_table()->LookupObject<rex::system::XThread>(startup_worker_handle.load());
    if (!worker) { std::fputs("Startup worker: handle not resolved\n", stderr); return; }
    auto* handle = worker->thread();
    auto* state = worker->thread_state();
    uint32_t sp = 0, lr = 0, argument = 0;
    if (!handle || !state || WaitForSingleObject(handle->native_handle(), 0) != WAIT_TIMEOUT) return;
    if (SuspendThread(handle->native_handle()) == DWORD(-1)) return;
    sp = state->context()->r1.u32;
    lr = uint32_t(state->context()->lr);
    argument = state->context()->r3.u32;
    if (ResumeThread(handle->native_handle()) == DWORD(-1)) std::_Exit(23);
    std::fprintf(stderr, "Startup worker: id=%u LR=%08X r3=%08X\n", worker->thread_id(), lr, argument);
    for (unsigned frame = 0; frame < 12 && sp; ++frame) {
        uint32_t parent = 0, caller = 0;
        if (!read_word(sp, parent) || parent <= sp || parent - sp > 0x10000u ||
            !read_word(parent - 8, caller) || caller < 0x82060000u || caller >= 0x8248B9E8u) break;
        std::fprintf(stderr, "Startup worker caller: frame=%u return=%08X\n", frame, caller);
        sp = parent;
    }
}
