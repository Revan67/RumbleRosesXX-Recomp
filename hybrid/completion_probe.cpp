#include "context.h"
#include <Windows.h>
#include <cstdio>
#include <rex/runtime.h>
extern "C" void __imp__sub_82072168(PPCContext&, uint8_t*);
extern bool rrxx_verbose_probes;

// Observe the wait interface without changing its return value or completion data.
extern "C" void sub_82072168(PPCContext& ctx, uint8_t* base) {
    const uint32_t wait = ctx.r3.u32;
    __imp__sub_82072168(ctx, base);
    if (!rrxx_verbose_probes) return;
    static thread_local ULONGLONG next_sample = 0;
    const auto now = GetTickCount64();
    if (now < next_sample) return;
    next_sample = now + 1000;
    auto* memory = rex::Runtime::instance()->memory();
    const auto read = [&](uint32_t address, uint32_t& out) {
        uint32_t raw = 0;
        SIZE_T bytes = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), memory->TranslateVirtual(address),
            &raw, sizeof(raw), &bytes) || bytes != sizeof(raw)) return false;
        out = _byteswap_ulong(raw);
        return true;
    };
    uint32_t device = 0, start = 0, saved = 0, pointer = 0, completed = 0, issued = 0;
    const bool valid = read(wait, device) && read(wait + 4, start) && read(wait + 8, saved) &&
        read(device + 10384, pointer) && read(pointer, completed) && read(device + 10396, issued);
    char report[256]{};
    const int length = std::snprintf(report, sizeof(report),
        "Completion wait: valid=%u result=%u pointer=%08X completed=%u issued=%u saved=%u start=%u\n",
        valid ? 1u : 0u, ctx.r3.u32, pointer, completed, issued, saved, start);
    DWORD written = 0;
    if (length > 0 && length < sizeof(report)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), report, length, &written, nullptr);
}
