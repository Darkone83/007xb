#ifndef PORT_PORTADDR_H
#define PORT_PORTADDR_H
#include "ge_native_platform.h"
/* CPU arithmetic uses the actual allocation. GPU/audio transport addresses
 * use the tagged conversions in PR/os.h. Neither requires a fixed VMA. */
#define PORT_DRAM_V1_BASE ((uintptr_t)geNativeDramBase())
#define PORT_DRAM_K0_BASE ((uintptr_t)geNativeDramBase())
#define PORT_DRAM_SIZE GE_NATIVE_DRAM_SIZE
#endif
