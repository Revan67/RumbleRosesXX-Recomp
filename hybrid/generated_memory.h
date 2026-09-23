#pragma once
#include "context.h"
#include "translated_base.h"
// Keep the external signature unchanged and replace only the local expression
// used by generated scalar, vector, and atomic base-plus-address accesses.
#undef PPC_FUNC_IMPL
#define PPC_FUNC_IMPL(x) extern "C" void x(PPCContext& __restrict ctx, uint8_t* rrxx_raw_base)
#undef PPC_FUNC_PROLOGUE
#define PPC_FUNC_PROLOGUE() RRXXTranslatedBase base{rrxx_raw_base}; __builtin_assume(((size_t)rrxx_raw_base & 0x1F) == 0)
