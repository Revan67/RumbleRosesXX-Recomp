// Bounded scalar-only diagnostics around unchanged generated implementations.
// Observe context drift; do not recover, skip callbacks, or change registers.
#include "context.h"
#include "ppc_recomp_shared.h"
#include <rex/logging.h>
#include <Windows.h>
#include <atomic>

namespace {
bool context_trace_enabled() {
    static const bool enabled = GetEnvironmentVariableW(L"RRXX_CONTEXT_TRACE", nullptr, 0) != 0;
    return enabled;
}
}

#define CONTEXT_OBSERVER(name, limit) \
extern "C" PPC_FUNC(__imp__##name); \
extern "C" PPC_FUNC(name) { \
    if (!context_trace_enabled()) { __imp__##name(ctx, base); return; } \
    static std::atomic<unsigned> calls{0}, drifts{0}; \
    const unsigned sequence = calls.fetch_add(1, std::memory_order_relaxed); \
    const uint32_t before_sp = ctx.r1.u32, before_r31 = ctx.r31.u32; \
    const uint32_t before_lr = uint32_t(ctx.lr); \
    if (sequence < limit) { \
        REXLOG_INFO("Context trace " #name " enter n={} LR={:08X} r1={:08X} r3={:08X} r4={:08X} r31={:08X}", \
            sequence, before_lr, before_sp, ctx.r3.u32, ctx.r4.u32, before_r31); \
        rex::FlushLogging(); \
    } \
    __imp__##name(ctx, base); \
    const bool drift = before_sp != ctx.r1.u32 || before_r31 != ctx.r31.u32; \
    const bool report_drift = drift && drifts.fetch_add(1, std::memory_order_relaxed) < 16; \
    if (sequence < limit || report_drift) { \
        REXLOG_INFO("Context trace " #name " returned n={} result={:08X} LR={:08X} r1={:08X}->{:08X} r31={:08X}->{:08X} state_changed={}", \
            sequence, ctx.r3.u32, uint32_t(ctx.lr), before_sp, ctx.r1.u32, before_r31, ctx.r31.u32, drift); \
        rex::FlushLogging(); \
    } \
}

CONTEXT_OBSERVER(sub_822FD9F8, 4)
CONTEXT_OBSERVER(sub_822FD910, 8)
CONTEXT_OBSERVER(sub_822FD788, 8)
CONTEXT_OBSERVER(sub_822E5320, 16)
#ifndef RRXX_NONLOCAL_BRIDGE
CONTEXT_OBSERVER(sub_822E7FD0, 8)
#endif
CONTEXT_OBSERVER(sub_824726C0, 16)
#ifndef RRXX_NONLOCAL_BRIDGE
CONTEXT_OBSERVER(sub_82472470, 16)
#endif

#undef CONTEXT_OBSERVER
