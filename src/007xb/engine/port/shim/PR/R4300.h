#ifndef GE_XBOX_SHIM_R4300_H
#define GE_XBOX_SHIM_R4300_H
#include "include/PR/R4300.h"
#include "ge_native_platform.h"
#undef PHYS_TO_K0
#undef PHYS_TO_K1
#undef K0_TO_PHYS
#undef K1_TO_PHYS
#undef K0_TO_K1
#undef K1_TO_K0
#define PHYS_TO_K0(a) ((uintptr_t)geNativeAddressToPointer((uintptr_t)(a)))
#define PHYS_TO_K1(a) ((uintptr_t)geNativeAddressToPointer((uintptr_t)(a)))
#define K0_TO_PHYS(p) geNativePointerToPhysical((const void *)(p))
#define K1_TO_PHYS(p) geNativePointerToPhysical((const void *)(p))
#define K0_TO_K1(p) ((uintptr_t)geNativeAddressToPointer((uintptr_t)(p)))
#define K1_TO_K0(p) ((uintptr_t)geNativeAddressToPointer((uintptr_t)(p)))
#endif
