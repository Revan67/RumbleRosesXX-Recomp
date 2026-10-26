#pragma once
// Experimental forced include: retain legacy instruction helpers while using
// SDK register types for the generated function bodies. Compile test only.
#define PPCContext RRXXLegacyContext
#define PPCRegister RRXXLegacyRegister
#define PPCXERRegister RRXXLegacyXER
#define PPCCRRegister RRXXLegacyCR
#define PPCVRegister RRXXLegacyVector
#define PPCFPSCRRegister RRXXLegacyFPSCR
#define PPCFunc RRXXLegacyFunc
#define PPCFuncMapping RRXXLegacyMapping
#define PPCFuncMappings RRXXLegacyMappings
#include "ppc_context.h"
#undef PPCContext
#undef PPCRegister
#undef PPCXERRegister
#undef PPCCRRegister
#undef PPCVRegister
#undef PPCFPSCRRegister
#undef PPCFunc
#undef PPCFuncMapping
#undef PPCFuncMappings
#include <rex/ppc/context.h>
