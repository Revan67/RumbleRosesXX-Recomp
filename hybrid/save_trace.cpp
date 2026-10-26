#include "context.h"
#include "ppc_recomp_shared.h"
#include <rex/logging.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_set>
#include <Windows.h>
#include <rex/runtime.h>
#include <rex/memory.h>

namespace {
std::atomic<unsigned> a_press_generation{0};
std::atomic<uint64_t> main_dispatch_calls{0};
}

// Strong diagnostic wrappers catch both generated direct calls and dispatch.
// Each delegates to the unchanged, locally generated implementation.
// Log only scalar arguments/results, never save contents or player names.
#define TRACE_SAVE(name) \
extern "C" PPC_FUNC(__imp__##name); \
extern "C" PPC_FUNC(name) { \
    static std::atomic<unsigned> calls{0}; \
    const auto sequence = calls.fetch_add(1, std::memory_order_relaxed); \
    const bool trace = sequence < 32; \
    if (trace) { \
        REXLOG_INFO("Save trace " #name " enter n={} LR={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}", \
            sequence, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); \
        rex::FlushLogging(); \
    } \
    __imp__##name(ctx, base); \
    if (trace) { \
        REXLOG_INFO("Save trace " #name " return n={} result={:08X}", sequence, ctx.r3.u32); \
        rex::FlushLogging(); \
    } \
}

TRACE_SAVE(sub_82066028)
TRACE_SAVE(sub_820666A0)
TRACE_SAVE(sub_82066130)
TRACE_SAVE(sub_82065FF8)
TRACE_SAVE(sub_82119B38)
TRACE_SAVE(sub_82119D30)
TRACE_SAVE(sub_820FDB18)
TRACE_SAVE(sub_820FDD28)

// Record gate transitions even after a long menu dwell; avoid per-frame logs.
#define TRACE_GATE(name) \
extern "C" PPC_FUNC(__imp__##name); \
extern "C" PPC_FUNC(name) { \
    const auto caller = uint32_t(ctx.lr); \
    __imp__##name(ctx, base); \
    static thread_local uint32_t previous = 0xFFFFFFFFu; \
    static thread_local unsigned changes = 0; \
    static thread_local unsigned previous_generation = 0; \
    const unsigned generation = a_press_generation.load(std::memory_order_relaxed); \
    const bool save_caller = caller == 0x82101428 || caller == 0x82101434 || \
        caller == 0x82101444 || caller == 0x82101470 || caller == 0x821014A4; \
    const bool main_caller = caller >= 0x821042C8 && caller < 0x82104500; \
    if (((save_caller && ctx.r3.u32 != previous) || \
         (main_caller && generation != previous_generation)) && changes < 64) { \
        previous = ctx.r3.u32; previous_generation = generation; ++changes; \
        REXLOG_INFO("Save gate " #name " LR={:08X} result={:08X}", caller, previous); \
        rex::FlushLogging(); \
    } \
}

TRACE_GATE(sub_820EE2E8)
TRACE_GATE(sub_82064F28)
TRACE_GATE(sub_820EEED8)
TRACE_GATE(sub_8211C688)
TRACE_GATE(sub_820FDA00)
TRACE_GATE(sub_820EE758)
TRACE_GATE(sub_82092958)
TRACE_GATE(sub_82127430)
TRACE_GATE(sub_820E7BC8)

namespace {
std::once_flag input_trace_once;
std::mutex input_trace_mutex;
HANDLE input_trace_file = INVALID_HANDLE_VALUE;
std::atomic<uint64_t> indirect_trace_deadline{0};
std::mutex indirect_trace_mutex;
std::unordered_set<uint64_t> indirect_trace_seen;
void input_trace_write(const char* line) {
    std::call_once(input_trace_once, [] {
        wchar_t path[32768]{};
        const DWORD length = GetEnvironmentVariableW(
            L"RRXX_INPUT_TRACE_FILE", path, DWORD(std::size(path)));
        if (!length || length >= std::size(path)) return;
        input_trace_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                                       nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input_trace_file == INVALID_HANDLE_VALUE) return;
        const char* header = "RRXX input boundary trace\r\n";
        DWORD written = 0;
        WriteFile(input_trace_file, header, DWORD(strlen(header)), &written, nullptr);
        FlushFileBuffers(input_trace_file);
    });
    if (input_trace_file == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lock(input_trace_mutex);
    DWORD written = 0;
    WriteFile(input_trace_file, line, DWORD(strlen(line)), &written, nullptr);
    FlushFileBuffers(input_trace_file);
}
// Diagnostic reads only: invalid/unmapped memory is reported, never repaired.
uint32_t sample_word(uint32_t address) {
    uint32_t raw = 0;
    SIZE_T read = 0;
    auto* memory = rex::Runtime::instance()->memory();
    if (!ReadProcessMemory(GetCurrentProcess(), memory->TranslateVirtual(address),
                           &raw, sizeof(raw), &read) || read != sizeof(raw))
        return 0xFFFFFFFFu;
    return _byteswap_ulong(raw);
}
void report_save_state(uint32_t object, const char* phase) {
    const auto state = sample_word(object + 100);
    const auto entered = sample_word(object + 104);
    const auto device = sample_word(object + 208);
    static thread_local uint32_t last_object = 0, last_state = ~0u,
        last_entered = ~0u, last_device = ~0u;
    static thread_local unsigned reports = 0;
    if (reports >= 128 || (object == last_object && state == last_state &&
        entered == last_entered && device == last_device)) return;
    last_object = object; last_state = state;
    last_entered = entered; last_device = device; ++reports;
    REXLOG_INFO("Save dispatcher {} object={:08X} state={} entered={} device={:08X}",
                phase, object, state, entered, device);
    rex::FlushLogging();
}
}

// Capture each distinct indirect route only once across the run, immediately
// after an A press. This does not cover generated direct calls.
extern "C" void rrxx_trace_indirect(uint32_t caller, uint32_t target) {
    const auto deadline = indirect_trace_deadline.load(std::memory_order_relaxed);
    if (!deadline || GetTickCount64() > deadline) return;
    const uint64_t route = (uint64_t(caller) << 32) | target;
    std::lock_guard<std::mutex> lock(indirect_trace_mutex);
    if (GetTickCount64() > indirect_trace_deadline.load(std::memory_order_relaxed) ||
        indirect_trace_seen.size() >= 1024 ||
        !indirect_trace_seen.insert(route).second) return;
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "indirect after A tick=%llu thread=%lu caller=%08X target=%08X\r\n",
                  GetTickCount64(), GetCurrentThreadId(), caller, target);
    input_trace_write(line);
}

// Observe the controller boundary before the menu dispatchers. Only A-button
// transitions are logged; routine polling and all other input remain private.
extern "C" PPC_FUNC(__imp__sub_822DD6D8);
extern "C" PPC_FUNC(sub_822DD6D8) {
    const auto caller = uint32_t(ctx.lr);
    const auto user = ctx.r3.u32;
    const auto state_ptr = ctx.r4.u32;
    __imp__sub_822DD6D8(ctx, base);
    static thread_local bool first_poll = true;
    if (first_poll) {
        first_poll = false;
        char line[160]{};
        std::snprintf(line, sizeof(line), "poll thread=%lu caller=%08X user=%u result=%08X\r\n",
                      GetCurrentThreadId(), caller, user, ctx.r3.u32);
        input_trace_write(line);
    }
    static thread_local bool previous_a[4]{};
    static thread_local unsigned reports = 0;
    if (user >= 4 || !state_ptr || reports >= 96) return;
    const auto result = ctx.r3.u32;
    if (result != 0) return;
    const auto button_word = sample_word(state_ptr + 4);
    if (button_word == 0xFFFFFFFFu) return;
    const auto buttons = button_word >> 16;
    const bool pressed = (buttons & 0x1000u) != 0;
    if (pressed == previous_a[user]) return;
    previous_a[user] = pressed;
    ++reports;
    if (pressed) {
        a_press_generation.fetch_add(1, std::memory_order_relaxed);
        indirect_trace_deadline.store(GetTickCount64() + 300,
                                      std::memory_order_relaxed);
    }
    char line[160]{};
    std::snprintf(line, sizeof(line), "A %s tick=%llu thread=%lu user=%u caller=%08X packet=%08X buttons=%04X main_calls=%llu\r\n",
                  pressed ? "down" : "up", GetTickCount64(), GetCurrentThreadId(),
                  user, caller, sample_word(state_ptr), buttons,
                  main_dispatch_calls.load(std::memory_order_relaxed));
    input_trace_write(line);
    REXLOG_INFO("Input A {} user={} caller={:08X} packet={:08X} buttons={:04X}",
                pressed ? "down" : "up", user, caller,
                sample_word(state_ptr), buttons);
    rex::FlushLogging();
}

extern "C" PPC_FUNC(__imp__sub_8237CF08);
extern "C" PPC_FUNC(sub_8237CF08) {
    const auto caller = uint32_t(ctx.lr);
    const bool input_dispatch = caller == 0x821A0474u || caller == 0x820F893Cu;
    static thread_local unsigned reports = 0;
    const bool trace = input_dispatch && reports < 64;
    const auto object = ctx.r3.u32;
    const auto vtable = trace ? sample_word(object) : 0;
    const auto target = trace && vtable != 0xFFFFFFFFu
        ? sample_word(vtable + 96) : 0xFFFFFFFFu;
    if (trace) {
        ++reports;
        char line[192]{};
        std::snprintf(line, sizeof(line),
                      "menu dispatch enter tick=%llu thread=%lu caller=%08X object=%08X vtable=%08X target=%08X\r\n",
                      GetTickCount64(), GetCurrentThreadId(), caller, object, vtable, target);
        input_trace_write(line);
    }
    __imp__sub_8237CF08(ctx, base);
    if (trace) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
                      "menu dispatch return tick=%llu thread=%lu target=%08X result=%08X\r\n",
                      GetTickCount64(), GetCurrentThreadId(), target, ctx.r3.u32);
        input_trace_write(line);
    }
}

extern "C" PPC_FUNC(__imp__sub_82101410);
extern "C" PPC_FUNC(sub_82101410) {
    const auto object = ctx.r3.u32;
    report_save_state(object, "enter");
    __imp__sub_82101410(ctx, base);
    report_save_state(object, "return");
}

extern "C" PPC_FUNC(__imp__sub_821042C8);
extern "C" PPC_FUNC(sub_821042C8) {
    main_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned generation = a_press_generation.load(std::memory_order_relaxed);
    const uint32_t object = ctx.r3.u32;
    __imp__sub_821042C8(ctx, base);
    static thread_local unsigned last_reported_generation = 0;
    if (!generation || generation == last_reported_generation || !object) return;
    last_reported_generation = generation;
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "main menu state A=%u object=%08X busy=%08X ready=%08X flags=%08X,%08X,%08X,%08X screen=%08X pending=%08X\r\n",
        generation, object, sample_word(object + 0x333A80),
        sample_word(object + 0x333A84), sample_word(object + 0x333820),
        sample_word(object + 0x333900), sample_word(object + 0x3339E0),
        sample_word(object + 0x333A20), sample_word(object + 0x333760),
        sample_word(object + 0x333764));
    input_trace_write(line);
}
