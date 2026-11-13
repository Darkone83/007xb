/* Native NV2A compiler for Fast3D's (A-B)*C+D combiner equations.
 * Encodings follow the Xbox D3DPIXELSHADERDEF input/output fields, matching
 * NVIDIA NV_register_combiners and abaire's NV2A hardware test notes:
 * https://registry.khronos.org/OpenGL/extensions/NV/NV_register_combiners.txt
 * https://github.com/abaire/nxdk_pgraph_tests/wiki/nv2a-pixel-shaders-(combiner-stages)
 * No shader bytecode, SDK implementation or third-party source is copied.
 */
#include "ge_nv2a_combiner.h"
#include <string.h>

enum {
    NV_ZERO=0, NV_C0=1, NV_C1=2, NV_FOG=3, NV_V0=4, NV_V1=5,
    NV_T0=8, NV_T1=9, NV_T2=10, NV_R0=12, NV_R1=13,
    NV_ALPHA=16, NV_UNSIGNED=0, NV_INVERT=32,
    NV_HALFBIAS_NEGATE=160, NV_SIGNED=192, NV_NEGATE=224,
    NV_MUX=1<<14
};
static uint32_t inputs(unsigned a,unsigned b,unsigned c,unsigned d) {
    return (a<<24)|(b<<16)|(c<<8)|d;
}
static unsigned one(unsigned channel) { return NV_ZERO|NV_INVERT|(channel?NV_ALPHA:0); }
static unsigned zero(unsigned channel) { return NV_ZERO|(channel?NV_ALPHA:0); }
static unsigned source(uint8_t x,unsigned channel,unsigned stage,unsigned cycle,
                       bool negative,const GENV2AInputMap& map,GENV2AProgram *p,
                       const char **error) {
    unsigned reg=NV_ZERO, component=channel?NV_ALPHA:0;
    if(x==SHADER_0)return zero(channel);
    if(x==SHADER_1) {
        /* 2*0-1 gives -1; unsigned inversion gives +1. */
        return NV_ZERO|component|(negative?64:NV_INVERT);
    }
    if(x>=SHADER_INPUT_1 && x<=SHADER_INPUT_7) {
        const uint8_t *varying=channel?map.alpha:map.rgb;
        if(varying[0]==x)reg=NV_V0;
        else if(varying[1]==x)reg=NV_V1;
        else {
            unsigned slot=0;
            for(;slot<2;++slot) {
                uint8_t n=p->constantInput[stage][slot][channel];
                if(!n || n==x)break;
            }
            if(slot==2) { *error="NV2A stage exceeds two constant sources";return 0; }
            p->constantInput[stage][slot][channel]=x;
            reg=slot?NV_C1:NV_C0;
        }
    } else if(x==SHADER_TEXEL0 || x==SHADER_TEXEL0A) {
        reg=NV_T0;if(x==SHADER_TEXEL0A)component=NV_ALPHA;
    } else if(x==SHADER_TEXEL1 || x==SHADER_TEXEL1A) {
        reg=NV_T1;if(x==SHADER_TEXEL1A)component=NV_ALPHA;
    } else if(x==SHADER_NOISE) {
        /* T2.rgb contains the PC shader's (random + 1) / 2. Alpha input
         * component zero selects BLUE, leaving T2.a for the other cutoff. */
        reg=NV_T2;component=0;p->usesNoise=1;
    } else if(x==SHADER_COMBINED) {
        /* The previous-cycle accumulator is uninitialized in cycle zero;
         * original materials use it only in cycle one. */
        if(!cycle) { *error="Combiner reads COMBINED before the first cycle";return 0; }
        reg=NV_R0;
    } else {
        *error="Unsupported direct N64 LOD combiner input";
        return 0;
    }
    return reg|component|(negative?NV_NEGATE:NV_SIGNED);
}
uint32_t geNV2AMapKey(const GENV2AInputMap& map) {
    return map.rgb[0]|((uint32_t)map.rgb[1]<<4)|
        ((uint32_t)map.alpha[0]<<8)|((uint32_t)map.alpha[1]<<12);
}
const char *geNV2ABuild(const CCFeatures& f,const GENV2AInputMap& map,GENV2AProgram *p) {
    memset(p,0,sizeof(*p));
    if(f.opt_blur)return "Framebuffer blur sampling is not implemented";
    if(f.opt_grayscale)return "Desktop grayscale shader extension is not implemented";
    if(f.opt_fog && map.alpha[1])return "Fog and a second varying alpha need an additional interpolator";
    p->stages=p->constantStages=f.opt_2cyc?4u:2u;
    p->constantMap[0]=p->constantMap[1]=0xffffffffu;
    for(unsigned stage=0;stage<p->constantStages;++stage) {
        p->constantMap[0]=(p->constantMap[0]&~(15u<<(stage*4)))|((stage*2u)<<(stage*4));
        p->constantMap[1]=(p->constantMap[1]&~(15u<<(stage*4)))|((stage*2u+1u)<<(stage*4));
    }
    const char *error=0;
    for(unsigned cycle=0;cycle<(f.opt_2cyc?2u:1u);++cycle) {
        const unsigned sub=cycle*2u,mad=sub+1u;
        for(unsigned channel=0;channel<2;++channel) {
            uint8_t a=f.c[cycle][channel][0],b=f.c[cycle][channel][1];
            uint8_t c=f.c[cycle][channel][2],d=f.c[cycle][channel][3];
            if(channel && !f.opt_alpha)a=b=c=d=SHADER_0;
            /* A/B are immaterial for C=0. This also avoids manufacturing
             * reads of unused COMBINED or constants in a single-input CC. */
            if(c==SHADER_0 || a==b)a=b=c=SHADER_0;
            unsigned sa=source(a,channel,sub,cycle,false,map,p,&error);
            unsigned sb=source(b,channel,sub,cycle,true,map,p,&error);
            unsigned sc=source(c,channel,mad,cycle,false,map,p,&error);
            unsigned sd=source(d,channel,mad,cycle,false,map,p,&error);
            if(error)return error;
            uint32_t *in=channel?p->alphaInputs:p->rgbInputs;
            uint32_t *out=channel?p->alphaOutputs:p->rgbOutputs;
            in[sub]=inputs(sa,one(channel),sb,one(channel));
            out[sub]=NV_R1<<8; /* AB+CD -> R1, individual products discarded. */
            in[mad]=inputs(NV_R1|NV_SIGNED|(channel?NV_ALPHA:0),sc,sd,one(channel));
            out[mad]=NV_R0<<8;
        }
    }
    if(f.opt_alpha && f.opt_noise) {
        /* Upstream non-edge noise keeps alpha a when a >= 1-r, otherwise
         * it writes zero (no discard). Edge noise instead compares against
         * (r+1)/2; the backend discards the resulting zero-alpha marker.
         * T2.a stores 1-r and T2.b stores (r+1)/2. Both use the same sample.
         *
         * Save a in R1.a while computing a + 0.5 - cutoff in R0.a. The
         * following MSB mux selects the saved a iff that sum is >= 0.5.
         * No RGB writes, extra constants or additional draw pass are needed. */
        const unsigned gate=p->stages++, select=p->stages++;
        const unsigned cutoff=NV_T2|(f.opt_texture_edge?0:NV_ALPHA);
        p->usesNoise=1;
        p->alphaInputs[gate]=inputs(NV_R0|NV_ALPHA,one(1),
                                    cutoff|NV_HALFBIAS_NEGATE,one(1));
        p->alphaOutputs[gate]=(NV_R1<<4)|(NV_R0<<8); /* AB -> R1; sum -> R0. */
        p->alphaInputs[select]=inputs(zero(1),zero(1),NV_R1|NV_ALPHA,one(1));
        p->alphaOutputs[select]=NV_MUX|(NV_R0<<8); /* MSB set chooses CD. */
    }
    /* A*B+(1-A)*C+D. Specular alpha holds one minus the RSP fog amount;
     * FOG.rgb is D3DRS_FOGCOLOR. Preserve the equation's own alpha. */
    p->finalABCD=f.opt_fog?inputs(NV_V1|NV_ALPHA,NV_R0,NV_FOG,NV_ZERO):
                                  inputs(NV_ZERO,NV_ZERO,NV_ZERO,NV_R0);
    unsigned alpha=f.opt_alpha?(NV_R0|NV_ALPHA):(NV_ZERO|NV_ALPHA|NV_INVERT);
    if(f.opt_invisible)alpha=NV_ZERO|NV_ALPHA;
    p->finalEFG=inputs(NV_ZERO,NV_ZERO,alpha,0);
    p->textureModes=(f.used_textures[0]?1u:0u)|(f.used_textures[1]?(1u<<5):0u)|
                    (p->usesNoise?(1u<<10):0u);
    return 0;
}
