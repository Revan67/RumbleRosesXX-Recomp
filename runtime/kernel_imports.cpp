#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "ppc_recomp_shared.h"
#include "kernel_imports.h"

namespace
{
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kInvalidParameter = 0xC000000Du;
constexpr uint32_t kNoMemory = 0xC0000017u;
constexpr uint32_t kHeapStart = 0x10000000u;
constexpr uint32_t kHeapEnd = 0x60000000u;
constexpr uint32_t kSystemHeapStart = 0x60000000u;
constexpr uint32_t kSystemHeapEnd = 0x70000000u;
constexpr uint32_t kStackArenaStart = 0x71000000u;
constexpr uint32_t kStackArenaEnd = 0x78000000u;
constexpr uint32_t kPageMask = 0xFFFu;

std::mutex gMemoryMutex;
uint32_t gHeapCursor = kHeapStart;
uint32_t gSystemHeapCursor = kSystemHeapStart;
uint32_t gStackCursor = kStackArenaStart;
std::unordered_map<uint32_t, uint32_t> gAllocations;
std::mutex gThreadClockMutex;
std::vector<uint32_t> gThreadClocks;
std::atomic<uint32_t> gNextTls{0};
thread_local std::array<uint32_t, 256> gTls{};

enum class HandleKind { Event, Thread };
struct GuestHandle
{
    HandleKind kind;
    bool signaled;
    uint32_t apiStartup{};
    uint32_t startAddress{};
    uint32_t startContext{};
    bool autoReset{};
};
std::mutex gHandleMutex;
std::condition_variable gHandleCondition;
uint32_t gNextHandle = 0x100;
std::unordered_map<uint32_t, GuestHandle> gHandles;
std::unordered_map<uint32_t, std::thread> gThreads;
std::unordered_map<uint32_t, bool> gKernelEvents;
std::atomic<uint32_t> gEventTraceCount{0};
std::mutex gCriticalSectionMapMutex;
std::unordered_map<uint32_t, std::shared_ptr<std::recursive_mutex>> gCriticalSections;

std::shared_ptr<std::recursive_mutex> GetCriticalSection(uint32_t address)
{
    std::lock_guard lock(gCriticalSectionMapMutex);
    auto& criticalSection = gCriticalSections[address];
    if (!criticalSection) criticalSection = std::make_shared<std::recursive_mutex>();
    return criticalSection;
}

uint32_t CreateHandle(GuestHandle object)
{
    std::lock_guard lock(gHandleMutex);
    const uint32_t handle = gNextHandle;
    gNextHandle += 4;
    gHandles.emplace(handle, object);
    return handle;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t AllocateGuest(uint8_t* base, uint32_t requestedAddress, uint32_t size, uint32_t alignment = 0x1000)
{
    if (!size)
        return 0;
    std::lock_guard lock(gMemoryMutex);
    const uint32_t committedSize = AlignUp(size, 0x1000);
    uint32_t address = requestedAddress ? requestedAddress : AlignUp(gHeapCursor, alignment);
    if (address < kHeapStart || uint64_t(address) + committedSize > kHeapEnd)
        return 0;
    if (!VirtualAlloc(base + address, committedSize, MEM_COMMIT, PAGE_READWRITE))
    {
        std::fprintf(stderr, "RRXX memory: commit failed address=0x%08X size=0x%X error=%lu\n",
            address, committedSize, GetLastError());
        return 0;
    }
    gAllocations[address] = committedSize;
    if (!requestedAddress)
        gHeapCursor = address + committedSize;
    return address;
}

uint32_t AllocateGuestStack(uint8_t* base, uint32_t size)
{
    std::lock_guard lock(gMemoryMutex);
    const uint32_t committedSize = AlignUp(size, 0x1000);
    const uint32_t address = AlignUp(gStackCursor, 0x1000);
    if (uint64_t(address) + committedSize > kStackArenaEnd)
        return 0;
    if (!VirtualAlloc(base + address, committedSize, MEM_COMMIT, PAGE_READWRITE))
    {
        std::fprintf(stderr, "RRXX memory: stack commit failed address=0x%08X size=0x%X error=%lu\n",
            address, committedSize, GetLastError());
        return 0;
    }
    gAllocations[address] = committedSize;
    gStackCursor = address + committedSize;
    return address;
}

uint32_t AllocateSystemHeap(uint8_t* base, uint32_t size, uint32_t alignment = 16)
{
    if (!size) return 0;
    std::lock_guard lock(gMemoryMutex);
    const uint32_t committedSize = AlignUp(size, 0x1000);
    const uint32_t address = AlignUp(gSystemHeapCursor, alignment);
    if (uint64_t(address) + committedSize > kSystemHeapEnd)
        return 0;
    if (!VirtualAlloc(base + address, committedSize, MEM_COMMIT, PAGE_READWRITE))
        return 0;
    gAllocations[address] = committedSize;
    gSystemHeapCursor = address + committedSize;
    return address;
}

void FreeGuest(uint8_t* base, uint32_t address)
{
    std::lock_guard lock(gMemoryMutex);
    const auto allocation = gAllocations.find(address);
    if (allocation != gAllocations.end())
    {
        VirtualFree(base + address, allocation->second, MEM_DECOMMIT);
        gAllocations.erase(allocation);
    }
}
}

void RegisterGuestThreadClock(unsigned int address)
{
    std::lock_guard lock(gThreadClockMutex);
    gThreadClocks.push_back(address);
}

void UpdateGuestThreadClocks(unsigned char* base, unsigned int tickCount)
{
    std::lock_guard lock(gThreadClockMutex);
    for (const uint32_t address : gThreadClocks) PPC_STORE_U32(address, tickCount);
}

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    const uint32_t addressPtr = ctx.r4.u32;
    const uint32_t sizePtr = ctx.r6.u32;
    if (!addressPtr || !sizePtr)
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    const uint32_t requested = PPC_LOAD_U32(addressPtr);
    const uint32_t requestedSize = PPC_LOAD_U32(sizePtr);
    const uint32_t address = AllocateGuest(base, requested, requestedSize);
    if (!address)
    {
        ctx.r3.u64 = kNoMemory;
        return;
    }
    PPC_STORE_U32(addressPtr, address);
    PPC_STORE_U32(sizePtr, AlignUp(requestedSize, 0x1000));
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    if (ctx.r4.u32)
        FreeGuest(base, PPC_LOAD_U32(ctx.r4.u32));
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    const uint32_t size = ctx.r4.u32;
    const uint32_t alignment = ctx.r7.u32 && (ctx.r7.u32 & (ctx.r7.u32 - 1)) == 0 ? ctx.r7.u32 : 0x1000;
    ctx.r3.u64 = AllocateGuest(base, 0, size, alignment > 0x1000u ? alignment : 0x1000u);
}

PPC_FUNC(__imp__MmFreePhysicalMemory) { FreeGuest(base, ctx.r3.u32); }
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    // Xbox 360 GPU-visible physical addresses use the low 29 address bits.
    ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFFu;
}
PPC_FUNC(__imp__ExAllocatePool) { ctx.r3.u64 = AllocateSystemHeap(base, ctx.r4.u32); }
PPC_FUNC(__imp__ExFreePool) { FreeGuest(base, ctx.r3.u32); }
PPC_FUNC(__imp__XamAlloc) { ctx.r3.u64 = AllocateSystemHeap(base, ctx.r3.u32); }
PPC_FUNC(__imp__XamFree) { FreeGuest(base, ctx.r3.u32); ctx.r3.u64 = 0; }

PPC_FUNC(__imp__MmQueryAllocationSize)
{
    std::lock_guard lock(gMemoryMutex);
    const auto allocation = gAllocations.find(ctx.r3.u32);
    ctx.r3.u64 = allocation == gAllocations.end() ? 0 : allocation->second;
}

PPC_FUNC(__imp__MmQueryAddressProtect) { ctx.r3.u64 = PAGE_READWRITE; }

PPC_FUNC(__imp__KeTlsAlloc) { ctx.r3.u64 = gNextTls.fetch_add(1); }
PPC_FUNC(__imp__KeTlsFree) { ctx.r3.u64 = ctx.r3.u32 < gTls.size(); }
PPC_FUNC(__imp__KeTlsSetValue)
{
    if (ctx.r3.u32 < gTls.size()) gTls[ctx.r3.u32] = ctx.r4.u32;
    ctx.r3.u64 = ctx.r3.u32 < gTls.size();
}
PPC_FUNC(__imp__KeTlsGetValue) { ctx.r3.u64 = ctx.r3.u32 < gTls.size() ? gTls[ctx.r3.u32] : 0; }

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    GetCriticalSection(ctx.r3.u32);
    ctx.r3.u64 = kSuccess;
}
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    GetCriticalSection(ctx.r3.u32);
    ctx.r3.u64 = kSuccess;
}
PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    GetCriticalSection(ctx.r3.u32)->lock();
    ctx.r3.u64 = kSuccess;
}
PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = GetCriticalSection(ctx.r3.u32)->try_lock() ? 1 : 0;
}
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    GetCriticalSection(ctx.r3.u32)->unlock();
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__RtlInitAnsiString)
{
    if (!ctx.r3.u32) return;
    uint16_t length = 0;
    if (ctx.r4.u32)
        while (PPC_LOAD_U8(ctx.r4.u32 + length) && length != 0xFFFE) ++length;
    PPC_STORE_U16(ctx.r3.u32, length);
    PPC_STORE_U16(ctx.r3.u32 + 2, uint16_t(length + 1));
    PPC_STORE_U32(ctx.r3.u32 + 4, ctx.r4.u32);
}

PPC_FUNC(__imp__KeQuerySystemTime)
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (ctx.r3.u32) PPC_STORE_U64(ctx.r3.u32, ticks);
}
PPC_FUNC(__imp__KeQueryPerformanceFrequency) { ctx.r3.u64 = 10'000'000; }
PPC_FUNC(__imp__KeGetCurrentProcessType) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    // The retail kernel copies the requested configuration payload to r5 and
    // reports its required size through r7.  Zero-valued defaults are valid
    // for the video/AV settings queried during D3D device construction.
    if (ctx.r7.u32) PPC_STORE_U32(ctx.r7.u32, ctx.r6.u32);
    if (ctx.r5.u32 && ctx.r6.u32)
        std::memset(base + ctx.r5.u32, 0, ctx.r6.u32);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtCreateEvent)
{
    if (!ctx.r3.u32) { ctx.r3.u64 = kInvalidParameter; return; }
    GuestHandle event{HandleKind::Event, ctx.r6.u32 != 0};
    event.autoReset = ctx.r5.u32 != 0;
    const uint32_t handle = CreateHandle(event);
    if (gEventTraceCount.fetch_add(1) < 40 || handle >= 0x500)
        std::fprintf(stderr,
            "RRXX sync: NtCreateEvent handle=0x%X type=%u initial=%u LR=0x%08X\n",
            handle, ctx.r5.u32, ctx.r6.u32, ctx.lr);
    PPC_STORE_U32(ctx.r3.u32, handle);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtSetEvent)
{
    static std::atomic<uint32_t> setTraceCount{0};
    if (setTraceCount.fetch_add(1) < 24)
        std::fprintf(stderr, "RRXX sync: NtSetEvent handle=0x%X LR=0x%08X\n", ctx.r3.u32, ctx.lr);
    std::lock_guard lock(gHandleMutex);
    const auto object = gHandles.find(ctx.r3.u32);
    if (object == gHandles.end()) { ctx.r3.u64 = kInvalidParameter; return; }
    const bool previous = object->second.signaled;
    object->second.signaled = true;
    gHandleCondition.notify_all();
    if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, previous);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtClearEvent)
{
    std::lock_guard lock(gHandleMutex);
    const auto object = gHandles.find(ctx.r3.u32);
    if (object == gHandles.end()) { ctx.r3.u64 = kInvalidParameter; return; }
    object->second.signaled = false;
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__KeResetEvent)
{
    if (gEventTraceCount.fetch_add(1) < 24)
        std::fprintf(stderr, "RRXX sync: KeResetEvent object=0x%08X\n", ctx.r3.u32);
    std::lock_guard lock(gHandleMutex);
    const auto handle = gHandles.find(ctx.r3.u32);
    if (handle != gHandles.end() && handle->second.kind == HandleKind::Event)
    {
        const bool previous = handle->second.signaled;
        handle->second.signaled = false;
        ctx.r3.u64 = previous;
        return;
    }
    const bool previous = gKernelEvents[ctx.r3.u32];
    gKernelEvents[ctx.r3.u32] = false;
    ctx.r3.u64 = previous;
}
PPC_FUNC(__imp__KeSetEvent)
{
    static std::atomic<uint32_t> setTraceCount{0};
    if (setTraceCount.fetch_add(1) < 24)
        std::fprintf(stderr, "RRXX sync: KeSetEvent object=0x%08X\n", ctx.r3.u32);
    std::lock_guard lock(gHandleMutex);
    const auto handle = gHandles.find(ctx.r3.u32);
    if (handle != gHandles.end() && handle->second.kind == HandleKind::Event)
    {
        const bool previous = handle->second.signaled;
        handle->second.signaled = true;
        gHandleCondition.notify_all();
        ctx.r3.u64 = previous;
        return;
    }
    const bool previous = gKernelEvents[ctx.r3.u32];
    gKernelEvents[ctx.r3.u32] = true;
    gHandleCondition.notify_all();
    ctx.r3.u64 = previous;
}

PPC_FUNC(__imp__KeWaitForSingleObject)
{
    if (gEventTraceCount.fetch_add(1) < 24)
        std::fprintf(stderr, "RRXX sync: KeWait object=0x%08X timeout=0x%08X\n", ctx.r3.u32, ctx.r7.u32);
    std::unique_lock lock(gHandleMutex);
    const uint32_t objectAddress = ctx.r3.u32;
    const auto ready = [&]
    {
        const auto handle = gHandles.find(objectAddress);
        if (handle != gHandles.end()) return handle->second.signaled;
        return gKernelEvents[objectAddress];
    };
    bool signaled = false;
    if (!ctx.r7.u32)
    {
        gHandleCondition.wait(lock, ready);
        signaled = true;
    }
    else
    {
        const int64_t interval = int64_t(PPC_LOAD_U64(ctx.r7.u32));
        if (interval < 0)
            signaled = gHandleCondition.wait_for(lock,
                std::chrono::nanoseconds((-interval) * 100), ready);
        else
            signaled = ready();
    }
    ctx.r3.u64 = signaled ? kSuccess : 0x00000102u;
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    static std::atomic<uint32_t> waitTraceCount{0};
    static std::atomic<uint32_t> waitDetailTraceCount{0};
    if (waitTraceCount.fetch_add(1) < 32)
        std::fprintf(stderr, "RRXX sync: NtWait handle=0x%X timeout=0x%08X LR=0x%08X caller=0x%08X\n",
            ctx.r3.u32, ctx.r6.u32, ctx.lr, PPC_LOAD_U32(ctx.r1.u32 + 120));
    std::thread worker;
    {
        std::unique_lock lock(gHandleMutex);
        const auto object = gHandles.find(ctx.r3.u32);
        if (object == gHandles.end()) { ctx.r3.u64 = kInvalidParameter; return; }
        if (ctx.r3.u32 >= 0x500 && waitDetailTraceCount.fetch_add(1) < 64)
            std::fprintf(stderr, "RRXX sync: wait object 0x%X kind=%s signaled=%u target=0x%08X\n",
                ctx.r3.u32, object->second.kind == HandleKind::Thread ? "thread" : "event",
                object->second.signaled, object->second.startAddress);
        if (object->second.kind == HandleKind::Event)
        {
            const auto ready = [&] { return object->second.signaled; };
            bool signaled = false;
            if (!ctx.r6.u32)
            {
                gHandleCondition.wait(lock, ready);
                signaled = true;
            }
            else
            {
                const int64_t interval = int64_t(PPC_LOAD_U64(ctx.r6.u32));
                if (interval < 0)
                    signaled = gHandleCondition.wait_for(lock,
                        std::chrono::nanoseconds((-interval) * 100), ready);
                else
                    signaled = ready();
            }
            if (signaled && object->second.autoReset) object->second.signaled = false;
            ctx.r3.u64 = signaled ? kSuccess : 0x00000102u; // STATUS_TIMEOUT.
            return;
        }
        const auto thread = gThreads.find(ctx.r3.u32);
        if (thread != gThreads.end())
        {
            worker = std::move(thread->second);
            gThreads.erase(thread);
        }
    }
    if (worker.joinable()) worker.join();
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtClose)
{
    if (CloseGuestFile(ctx.r3.u32)) { ctx.r3.u64 = kSuccess; return; }
    std::lock_guard lock(gHandleMutex);
    const auto thread = gThreads.find(ctx.r3.u32);
    if (thread != gThreads.end())
    {
        if (thread->second.joinable()) thread->second.detach();
        gThreads.erase(thread);
    }
    gHandles.erase(ctx.r3.u32);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__ExCreateThread)
{
    if (!ctx.r3.u32) { ctx.r3.u64 = kInvalidParameter; return; }
    const uint32_t handle = CreateHandle({HandleKind::Thread, false, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32});
    std::fprintf(stderr,
        "RRXX sync: ExCreateThread handle=0x%X api=0x%08X start=0x%08X context=0x%08X flags=0x%08X\n",
        handle, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
    PPC_STORE_U32(ctx.r3.u32, handle);
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, handle);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtResumeThread)
{
    const uint32_t handle = ctx.r3.u32;
    GuestHandle thread{};
    {
        std::lock_guard lock(gHandleMutex);
        const auto object = gHandles.find(handle);
        if (object == gHandles.end() || object->second.kind != HandleKind::Thread)
        { ctx.r3.u64 = kInvalidParameter; return; }
        thread = object->second;
    }
    if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, 1);
    std::fprintf(stderr, "RRXX sync: NtResumeThread handle=0x%X\n", handle);
    ctx.r3.u64 = kSuccess;

    const uint32_t stack = AllocateGuestStack(base, 1024 * 1024);
    const uint32_t entryAddress = thread.apiStartup ? thread.apiStartup : thread.startAddress;
    auto* entry = PPC_LOOKUP_FUNC(base, entryAddress);
    if (!stack || !entry)
    {
        std::fprintf(stderr, "RRXX host: unable to start guest thread at 0x%08X\n", entryAddress);
        return;
    }

    {
        std::lock_guard lock(gHandleMutex);
        gThreads.emplace(handle, std::thread([=]
        {
            std::fprintf(stderr, "RRXX host: starting guest thread 0x%X at 0x%08X (target 0x%08X)\n",
                handle, entryAddress, thread.startAddress);
            PPCContext threadContext{};
            threadContext.r1.u64 = stack + 1024 * 1024 - 0x1000;
            // Xbox code obtains the current thread ID through the per-thread
            // processor control region at r13+0x100 -> thread+0x14C.  Give
            // every recompiled host thread its own minimal PCR/KTHREAD view.
            const uint32_t threadObject = stack + 0x400;
            threadContext.r13.u64 = stack;
            PPC_STORE_U32(stack + 0x100, threadObject);
            PPC_STORE_U32(threadObject + 0x14C, handle);
            RegisterGuestThreadClock(threadObject + 0x58);
            if (thread.apiStartup)
            {
                threadContext.r3.u64 = thread.startAddress;
                threadContext.r4.u64 = thread.startContext;
            }
            else
            {
                threadContext.r3.u64 = thread.startContext;
            }
            entry(threadContext, base);
            std::lock_guard completedLock(gHandleMutex);
            const auto object = gHandles.find(handle);
            if (object != gHandles.end()) object->second.signaled = true;
            gHandleCondition.notify_all();
            std::fprintf(stderr, "RRXX host: guest thread 0x%X returned\n", handle);
        }));
    }
}

PPC_FUNC(__imp__NtSuspendThread)
{
    std::lock_guard lock(gHandleMutex);
    const auto object = gHandles.find(ctx.r3.u32);
    if (object == gHandles.end() || object->second.kind != HandleKind::Thread)
    { ctx.r3.u64 = kInvalidParameter; return; }
    object->second.signaled = false;
    if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, 0);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    std::lock_guard lock(gHandleMutex);
    if (gHandles.find(ctx.r3.u32) == gHandles.end())
    { ctx.r3.u64 = kInvalidParameter; return; }
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, ctx.r3.u32);
    ctx.r3.u64 = kSuccess;
}
PPC_FUNC(__imp__ObDereferenceObject) { ctx.r3.u64 = kSuccess; }
PPC_FUNC(__imp__RtlNtStatusToDosError) { ctx.r3.u64 = (ctx.r3.u32 & 0x80000000u) ? 1 : ctx.r3.u32; }
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    for (uint32_t offset = 0; offset + 4 <= ctx.r4.u32; offset += 4)
        PPC_STORE_U32(ctx.r3.u32 + offset, ctx.r5.u32);
}
PPC_FUNC(__imp__KiApcNormalRoutineNop) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__DbgPrint)
{
    static std::atomic<uint32_t> reportCount{0};
    const uint32_t report = reportCount.fetch_add(1);
    if (report >= 64) { ctx.r3.u64 = 0; return; }
    std::fprintf(stderr, "GUEST: ");
    if (ctx.r3.u32)
        for (uint32_t i = 0; i < 256; ++i)
        {
            const char c = char(PPC_LOAD_U8(ctx.r3.u32 + i));
            if (!c) break;
            std::fputc((c == '\n' || c == '\r' || c == '\t' ||
                (static_cast<unsigned char>(c) >= 0x20 &&
                 static_cast<unsigned char>(c) < 0x7F)) ? c : '.', stderr);
        }
    std::fprintf(stderr, " [r4=%lld/0x%08X r5=0x%08X r6=0x%08X r7=0x%08X]",
        static_cast<long long>(ctx.r4.s64), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    std::fputc('\n', stderr);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeDelayExecutionThread)
{
    if (!ctx.r5.u32) { std::this_thread::yield(); ctx.r3.u64 = kSuccess; return; }
    const int64_t interval = int64_t(PPC_LOAD_U64(ctx.r5.u32));
    if (interval < 0)
    {
        const auto duration = std::chrono::nanoseconds((-interval) * 100);
        std::this_thread::sleep_for(duration > std::chrono::seconds(1) ? std::chrono::seconds(1) : duration);
    }
    else
        std::this_thread::yield();
    ctx.r3.u64 = kSuccess;
}

void WaitForGuestThreads()
{
    for (;;)
    {
        std::thread worker;
        {
            std::lock_guard lock(gHandleMutex);
            if (gThreads.empty()) break;
            auto thread = gThreads.begin();
            worker = std::move(thread->second);
            gThreads.erase(thread);
        }
        if (worker.joinable()) worker.join();
    }
}
