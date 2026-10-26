#pragma once
#include "context.h"
#include "translated_base.h"
// Keep the external signature unchanged and replace only the local expression
// used by generated scalar, vector, and atomic base-plus-address accesses.
#undef PPC_FUNC_IMPL
#define PPC_FUNC_IMPL(x) extern "C" void x(PPCContext& __restrict ctx, uint8_t* rrxx_raw_base)
#undef PPC_FUNC_PROLOGUE
#ifdef RRXX_FULL_GUEST_TRACE
#include "guest_trace.h"
#define PPC_FUNC_PROLOGUE() RRXXTranslatedBase base{rrxx_raw_base}; __builtin_assume(((size_t)rrxx_raw_base & 0x1F) == 0); if ((++rrxx_guest_trace_counter & 4095u) == 0) rrxx_guest_trace_entry(__func__, uint32_t(ctx.lr), ctx.r3.u32)
#else
#define PPC_FUNC_PROLOGUE() RRXXTranslatedBase base{rrxx_raw_base}; __builtin_assume(((size_t)rrxx_raw_base & 0x1F) == 0)
#endif
