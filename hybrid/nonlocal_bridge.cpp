#include "context.h"
#include "ppc_recomp_shared.h"
#include "nonlocal_transfer.h"
#include <rex/logging.h>
#include <atomic>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_82472470);
extern "C" PPC_FUNC(sub_82472470) {
    const uint32_t buffer=ctx.r3.u32;
    // Preserve the guest's normalization and register restoration. Only the
    // native continuation mechanism is replaced after the original path runs.
    __imp__sub_82472470(ctx,base);
    const auto* target=rrxx::nonlocal::Frame::Find(buffer,uint32_t(ctx.lr),ctx.r1.u32);
    if(!target) {
        REXLOG_ERROR("Nonlocal transfer has no active target: buffer={:08X} LR={:08X} SP={:08X}",buffer,uint32_t(ctx.lr),ctx.r1.u32);
        rex::FlushLogging();
        std::_Exit(23);
    }
    static std::atomic<unsigned> transfers{0};
    if(transfers.fetch_add(1,std::memory_order_relaxed)<32) {
        REXLOG_INFO("Nonlocal transfer unwind: buffer={:08X} continuation={:08X} result={:08X}",buffer,uint32_t(ctx.lr),ctx.r3.u32);
        rex::FlushLogging();
    }
    throw rrxx::nonlocal::Transfer{target};
}

void rrxx_nonlocal_resumed(uint32_t buffer,uint32_t continuation) {
    static std::atomic<unsigned> resumptions{0};
    if(resumptions.fetch_add(1,std::memory_order_relaxed)<32) {
        REXLOG_INFO("Nonlocal transfer resumed: buffer={:08X} continuation={:08X}",buffer,continuation);
        rex::FlushLogging();
    }
}
