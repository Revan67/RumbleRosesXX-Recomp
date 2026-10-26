#include <cstdio>
#include <rex/system/function_dispatcher.h>
extern "C" void rrxx_probe_callback(PPCContext&, uint8_t*);
int main() {
    rex::memory::Memory memory;
    if (!memory.Initialize()) return 2;
    constexpr uint32_t data = 0x10000000;
    auto* heap = memory.LookupHeap(data);
    if (!heap->AllocFixed(data, 0x10000, 0x10000, 3, 3)) return 3;
    rex::runtime::ExportResolver exports;
    rex::runtime::FunctionDispatcher dispatcher(&memory, &exports);
    constexpr uint32_t function = 0x82000000;
    if (!dispatcher.InitializeFunctionTable(function, 0x10000, function, 0x10000, true)) return 4;
    if (!dispatcher.SetFunction(function, rrxx_probe_callback)) return 5;
    rex::runtime::ThreadState thread(1, data + 0xF000, data, &memory);
    auto* ctx = thread.context();
    for (bool interrupt : {false, true}) {
        ctx->f0.f64 = 1.25; ctx->f31.f64 = 31.5;
        ctx->lr = 0x11223344;
        uint64_t args[] = {0, data + 0x100};
        const auto result = interrupt
            ? dispatcher.ExecuteInterrupt(&thread, function, args, 2)
            : dispatcher.Execute(&thread, function, args, 2);
        auto* bytes = memory.virtual_membase() + data + 0x100;
        if (result != 42 || ctx->f0.f64 != 3.25 || ctx->f31.f64 != 35.5 ||
            ctx->r1.u64 != data + 0xF000 || ctx->lr != 0x11223344 ||
            bytes[0] != 0x12 || bytes[1] != 0x34 || bytes[2] != 0x56 || bytes[3] != 0x78) return 6;
        std::puts(interrupt ? "PASS: interrupt dispatch" : "PASS: normal dispatch");
    }
    return 0;
}
