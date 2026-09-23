#include "context.h"
#include "translated_base.h"
#include <cstring>

// Standard byte-fill ABI: r3=destination, r4=byte, r5=count; return destination.
// The analyzed leaf boundary omits tail stores. Supply the standard operation
// independently, without copying generated instructions or changing game data.
extern "C" void sub_822CB6A0(PPCContext& ctx, uint8_t* raw_base) {
    const auto count = ctx.r5.u32;
    if (count) std::memset(RRXXTranslatedBase{raw_base} + ctx.r3.u32,
        ctx.r4.u8, count);
}
