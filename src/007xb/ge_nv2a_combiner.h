#ifndef GE_NV2A_COMBINER_H
#define GE_NV2A_COMBINER_H
#include <stdint.h>
#include "gfx_cc.h"

/* Input numbers are the original Fast3D SHADER_INPUT_1..7 indices.
 * RGB and alpha are allocated independently to the two vertex colours. */
struct GENV2AInputMap {
    uint8_t rgb[2];
    uint8_t alpha[2];
};
struct GENV2AProgram {
    /* Four equation stages plus two alpha-noise stages. NV2A supports eight. */
    uint32_t rgbInputs[6], alphaInputs[6];
    uint32_t rgbOutputs[6], alphaOutputs[6];
    uint32_t finalABCD, finalEFG;
    uint32_t constantMap[2];
    uint32_t textureModes;
    unsigned stages, constantStages, usesNoise;
    /* [stage][C0/C1][RGB/alpha], 0 means unused/zero. */
    uint8_t constantInput[4][2][2];
};

/* Builds the actual NV2A register-combiner words. No D3D or host runtime
 * is needed to inspect/test the compiler. Returns a static error string. */
const char *geNV2ABuild(const CCFeatures&,const GENV2AInputMap&,GENV2AProgram*);
uint32_t geNV2AMapKey(const GENV2AInputMap&);
#endif
