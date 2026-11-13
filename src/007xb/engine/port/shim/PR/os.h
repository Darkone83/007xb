#ifndef GE_XBOX_SHIM_OS_H
#define GE_XBOX_SHIM_OS_H
#include "include/PR/os.h"
#include "ge_native_platform.h"

/* On Xbox, low addresses are real executable/stack/heap pointers. Never
 * encode an arena pointer as an untagged offset: it would be ambiguous. */
#undef OS_K0_TO_PHYSICAL
#undef OS_K1_TO_PHYSICAL
#undef OS_PHYSICAL_TO_K0
#undef OS_PHYSICAL_TO_K1
#define OS_K0_TO_PHYSICAL(p) geNativePointerToPhysical((const void *)(p))
#define OS_K1_TO_PHYSICAL(p) geNativePointerToPhysical((const void *)(p))
#define OS_PHYSICAL_TO_K0(a) geNativeAddressToPointer((uintptr_t)(a))
#define OS_PHYSICAL_TO_K1(a) geNativeAddressToPointer((uintptr_t)(a))
#endif
