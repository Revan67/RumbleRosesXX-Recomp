#include "sdk_context_compile_probe.h"
extern "C" PPC_FUNC(rrxx_probe_callback) {
    const bool valid = ctx.f0.f64 == 1.25 && ctx.f31.f64 == 31.5;
    ctx.f0.f64 += 2.0;
    ctx.f31.f64 += 4.0;
    PPC_STORE_U32(ctx.r4.u32, 0x12345678);
    ctx.r3.u64 = valid ? 42 : 0;
}
