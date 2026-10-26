#include "context.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <rex/runtime.h>
extern "C" void __imp__sub_82099CE8(PPCContext&, uint8_t*);
bool rrxx_parser_probe_enabled = true;

// Forward the iterator unchanged; observe only the two passes under investigation.
extern "C" void sub_82099CE8(PPCContext& ctx, uint8_t* base) {
    const auto caller = static_cast<uint32_t>(ctx.lr);
    const auto iterator = ctx.r3.u32;
    __imp__sub_82099CE8(ctx, base);
    if (!rrxx_parser_probe_enabled) return;
    const bool counting = caller == 0x820874D4u || caller == 0x8208753Cu;
    const bool filling = caller == 0x820876BCu || caller == 0x82087B6Cu;
    if (!counting && !filling) return;
    static std::atomic<unsigned> samples{0};
    const bool emit_sample = samples.fetch_add(1) < 12;
    auto* memory = rex::Runtime::instance()->memory();
    uint32_t fields[3] = {};
    SIZE_T bytes = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), memory->TranslateVirtual(iterator + 8), fields,
        sizeof(fields), &bytes) || bytes != sizeof(fields)) return;
    const auto name_address = _byteswap_ulong(fields[1]);
    char name[64] = {}, expected[64] = {};
    // Factual marker address from the compared instruction; never emit string contents.
    bool readable = ReadProcessMemory(GetCurrentProcess(), memory->TranslateVirtual(name_address),
        name, sizeof(name), &bytes) && bytes == sizeof(name);
    readable = readable && ReadProcessMemory(GetCurrentProcess(), memory->TranslateVirtual(0x820063FCu),
        expected, sizeof(expected), &bytes) && bytes == sizeof(expected);
    unsigned length = 0;
    uint32_t hash = 2166136261u;
    bool same = readable;
    if (readable) for (; length < 64 && name[length]; ++length) {
        hash = (hash ^ static_cast<unsigned char>(name[length])) * 16777619u;
        if (name[length] != expected[length]) same = false;
    }
    if (length == 64 || (readable && expected[length])) same = false;
    static thread_local unsigned visited = 0, markers = 0;
    static thread_local unsigned best_prefix = 0, best_length = 0, mismatch_byte = 0;
    unsigned expected_length = 0;
    uint32_t expected_hash = 2166136261u;
    for (; readable && expected_length < 64 && expected[expected_length]; ++expected_length)
        expected_hash = (expected_hash ^ static_cast<unsigned char>(expected[expected_length])) * 16777619u;
    if (counting) {
        ++visited;
        if (readable && ctx.r3.u32 && _byteswap_ulong(fields[0]) == 6) {
            unsigned prefix = 0;
            while (prefix < length && prefix < expected_length && name[prefix] == expected[prefix]) ++prefix;
            if (prefix > best_prefix) {
                best_prefix = prefix; best_length = length;
                mismatch_byte = prefix < 64 ? static_cast<unsigned char>(name[prefix]) : 0;
            }
        }
        if (ctx.r3.u32 && _byteswap_ulong(fields[0]) == 6 && same) ++markers;
        if (!ctx.r3.u32) {
            char summary[256] = {};
            const int n = std::snprintf(summary, sizeof(summary),
                "Parser count completed: visits=%u markers=%u expected-length=%u expected-hash=%08X closest-prefix=%u closest-length=%u mismatch-byte=%u\n",
                visited, markers, expected_length, expected_hash, best_prefix, best_length, mismatch_byte);
            DWORD written = 0;
            if (n > 0 && n < sizeof(summary)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), summary, n, &written, nullptr);
            visited = markers = 0;
            best_prefix = best_length = mismatch_byte = 0;
        }
    }
    if (!emit_sample && ctx.r3.u32) return;
    char report[256] = {};
    const auto size = std::snprintf(report, sizeof(report), "Parser iterator: pass=%s result=%u type=%u name-readable=%u name-length=%u name-hash=%08X marker-match=%u\n",
        counting ? "count" : "fill", ctx.r3.u32, _byteswap_ulong(fields[0]), readable ? 1u : 0u,
        length, hash, same ? 1u : 0u);
    DWORD written = 0;
    if (size > 0 && size < sizeof(report)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), report, size, &written, nullptr);
}
