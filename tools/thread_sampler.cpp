#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    const uintptr_t imageBase = std::strtoull(argv[2], nullptr, 16);
    std::map<uintptr_t, uint32_t> samples;
    for (uint32_t pass = 0; pass < 200; ++pass) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{sizeof(entry)};
        if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != pid) continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                    THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread) continue;
                if (SuspendThread(thread) != DWORD(-1)) {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(thread, &context) && context.Rip >= imageBase)
                        ++samples[static_cast<uintptr_t>(context.Rip) - imageBase];
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }
        if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
        Sleep(10);
    }
    std::vector<std::pair<uintptr_t, uint32_t>> ordered(samples.begin(), samples.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    for (size_t i = 0; i < std::min<size_t>(ordered.size(), 20); ++i)
        std::printf("%08llX %u\n", static_cast<unsigned long long>(ordered[i].first),
            ordered[i].second);
}
