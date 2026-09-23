#pragma once
#include "legacy_context_compat.h"
// SDK kernel exports have C linkage; generated direct-call declarations must match.
#undef PPC_EXTERN_FUNC
#define PPC_EXTERN_FUNC(x) extern "C" PPC_FUNC(x)

// Resolve through the SDK so invalid addresses never index an unchecked table.
void rrxx_hybrid_indirect(PPCContext& ctx, uint8_t* base, uint32_t address);
#undef PPC_CALL_INDIRECT_FUNC
#define PPC_CALL_INDIRECT_FUNC(x) rrxx_hybrid_indirect(ctx, base, uint32_t(x))
