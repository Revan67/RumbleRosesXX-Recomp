#include <cstddef>
#include <cstdio>
#ifdef RRXX_SDK_CONTEXT
#include <rex/ppc/context.h>
#else
#include "ppc_context.h"
#endif
#define FIELD(name) std::printf(#name "=%zu\n", offsetof(PPCContext, name))
int main() {
    std::printf("size=%zu alignment=%zu\n", sizeof(PPCContext), alignof(PPCContext));
    FIELD(r3); FIELD(r0); FIELD(r1); FIELD(r31);
    FIELD(lr); FIELD(ctr); FIELD(xer); FIELD(reserved); FIELD(msr);
    FIELD(cr0); FIELD(cr7); FIELD(fpscr); FIELD(f0); FIELD(f31);
    FIELD(v0); FIELD(v127);
}
