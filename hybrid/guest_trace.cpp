#include "guest_trace.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <Windows.h>

namespace {
constexpr std::uint32_t kPrefixCapacity = 1u << 18;
constexpr std::uint32_t kTailCapacity = 1u << 21;
constexpr std::uint32_t kSampleInterval = 1u << 12;
constexpr std::uint32_t kMagic = 0x52525447u;  // GTRR, little-endian.

struct alignas(8) TraceHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t entry_size;
    std::uint32_t prefix_capacity;
    std::uint32_t tail_capacity;
    std::uint32_t reserved;
    alignas(8) volatile LONG64 next_sequence;
};

struct alignas(8) TraceEntry {
    std::uint64_t sequence;
    std::uint64_t milliseconds;
    std::uint32_t guest_function;
    std::uint32_t guest_lr;
    std::uint32_t guest_r3;
    std::uint32_t host_thread;
};

static_assert(sizeof(TraceEntry) == 32);
std::atomic<TraceHeader*> g_header{nullptr};
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_mapping = nullptr;

std::uint32_t guest_address(const char* name) noexcept {
    const char* marker = std::strstr(name, "sub_");
    if (!marker) return 0;
    marker += 4;
    std::uint32_t address = 0;
    for (int i = 0; i != 8; ++i) {
        const char c = marker[i];
        const std::uint32_t digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
            : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0xFFFFFFFFu;
        if (digit > 15) return 0;
        address = (address << 4) | digit;
    }
    return address;
}
}  // namespace

thread_local std::uint32_t rrxx_guest_trace_counter = 0;

void rrxx_guest_trace_start_from_env() {
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"RRXX_GUEST_TRACE_FILE", path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) return;

    const auto size = sizeof(TraceHeader) +
        std::uint64_t(kPrefixCapacity + kTailCapacity) * sizeof(TraceEntry);
    g_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                         nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER end{};
    end.QuadPart = size;
    if (!SetFilePointerEx(g_file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(g_file))
        return;
    g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE,
                                   DWORD(size >> 32), DWORD(size), nullptr);
    if (!g_mapping) return;
    auto* header = static_cast<TraceHeader*>(
        MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, size));
    if (!header) return;
    header->magic = kMagic;
    header->version = 1;
    header->entry_size = sizeof(TraceEntry);
    header->prefix_capacity = kPrefixCapacity;
    header->tail_capacity = kTailCapacity;
    header->reserved = kSampleInterval;
    header->next_sequence = 0;
    g_header.store(header, std::memory_order_release);
}

extern "C" void rrxx_guest_trace_entry(const char* function_name,
                                       std::uint32_t guest_lr,
                                       std::uint32_t guest_r3) noexcept {
    auto* header = g_header.load(std::memory_order_relaxed);
    if (!header) return;
    const auto sequence = std::uint64_t(InterlockedIncrement64(
        &header->next_sequence) - 1);
    const auto index = sequence < kPrefixCapacity ? sequence
        : kPrefixCapacity + ((sequence - kPrefixCapacity) % kTailCapacity);
    auto* entries = reinterpret_cast<TraceEntry*>(header + 1);
    auto& entry = entries[index];
    entry.milliseconds = GetTickCount64();
    entry.guest_function = guest_address(function_name);
    entry.guest_lr = guest_lr;
    entry.guest_r3 = guest_r3;
    entry.host_thread = GetCurrentThreadId();
    entry.sequence = sequence;
}
