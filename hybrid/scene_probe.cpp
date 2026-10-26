#include "context.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <rex/runtime.h>

extern bool rrxx_verbose_probes;
namespace {
struct Counter { uint32_t target = 0, caller = 0; unsigned calls = 0; };
struct Sample {
    std::array<Counter, 64> calls{};
    unsigned used = 0, overflow = 0, entries = 0;
    ULONGLONG next_report = 0;
};
thread_local Sample* active = nullptr;
void invoke(PPCContext& ctx, uint8_t* base, PPCFunc* function, Sample& sample, unsigned stage) {
    if (!rrxx_verbose_probes) { function(ctx, base); return; }
    auto* previous = active;
    active = &sample;
    ++sample.entries;
    function(ctx, base);
    active = previous;
    const auto now = GetTickCount64();
    if (now < sample.next_report) return;
    sample.next_report = now + 1000;
    char report[8192]{};
    int size = std::snprintf(report, sizeof(report), "Update scope: stage=%u returns=%u unique=%u overflow=%u\n",
        stage, sample.entries, sample.used, sample.overflow);
    for (unsigned i = 0; i < sample.used; ++i)
        size += std::snprintf(report + size, sizeof(report) - size,
            "Update dispatch: stage=%u target=%08X caller=%08X calls=%u\n",
            stage, sample.calls[i].target, sample.calls[i].caller, sample.calls[i].calls);
    DWORD written = 0;
    if (size > 0 && size < sizeof(report)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), report, size, &written, nullptr);
    sample.calls = {};
    sample.used = sample.overflow = sample.entries = 0;
}
}

// Fixed storage; at most one report per second per scope. No guest payloads.
void rrxx_observe_scene_call(uint32_t target, uint32_t caller) {
    if (!active) return;
    for (unsigned i = 0; i < active->used; ++i) {
        auto& counter = active->calls[i];
        if (counter.target == target && counter.caller == caller) { ++counter.calls; return; }
    }
    if (active->used == active->calls.size()) { ++active->overflow; return; }
    active->calls[active->used++] = {target, caller, 1};
}

extern "C" void __imp__sub_8216FFE8(PPCContext&, uint8_t*);
extern "C" void __imp__sub_8208E9E0(PPCContext&, uint8_t*);
extern "C" void sub_8216FFE8(PPCContext& ctx, uint8_t* base) {
    static thread_local Sample sample;
    invoke(ctx, base, __imp__sub_8216FFE8, sample, 1);
}
extern "C" void sub_8208E9E0(PPCContext& ctx, uint8_t* base) {
    static thread_local Sample sample;
    invoke(ctx, base, __imp__sub_8208E9E0, sample, 2);
}

extern "C" void __imp__sub_82062088(PPCContext&, uint8_t*);
extern "C" void sub_82062088(PPCContext& ctx, uint8_t* base) {
    if (!rrxx_verbose_probes) { __imp__sub_82062088(ctx, base); return; }
    const uint32_t object = ctx.r3.u32;
    static thread_local ULONGLONG next_sample = 0;
    const auto now = GetTickCount64();
    if (now >= next_sample) {
        next_sample = now + 1000;
        unsigned present[5]{};
        bool readable = true;
        unsigned index = 0;
        for (const uint32_t offset : {16u, 20u, 52u, 28u, 32u}) {
            uint32_t value = 0;
            SIZE_T count = 0;
            readable &= ReadProcessMemory(GetCurrentProcess(),
                rex::Runtime::instance()->memory()->TranslateVirtual(object + offset),
                &value, 4, &count) && count == 4;
            present[index] = index < 3 ? unsigned(value != 0) : _byteswap_ulong(value);
            ++index;
        }
        char report[224]{};
        const int size = std::snprintf(report, sizeof(report),
            "Draw gate sample: readable=%u field16-nonzero=%u field20-nonzero=%u field52-nonzero=%u phase=%u state=%u\n",
            readable ? 1u : 0u, present[0], present[1], present[2], present[3], present[4]);
        DWORD written = 0;
        if (size > 0 && size < sizeof(report)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), report, size, &written, nullptr);
    }
    __imp__sub_82062088(ctx, base);
}
