/* Original fast3d combiner feature parser.  No Direct3D approximations here. */
#include "gfx_cc.h"
void gfx_cc_get_features(uint64_t shader_id0,uint32_t shader_id1,struct CCFeatures* f){
 for(int i=0;i<2;i++)for(int j=0;j<2;j++)for(int k=0;k<4;k++)
  f->c[i][j][k]=(uint8_t)((shader_id0>>(i*32+j*16+k*4))&0xf);
 f->opt_alpha=(shader_id1&SHADER_OPT_ALPHA)!=0;
 f->opt_fog=(shader_id1&SHADER_OPT_FOG)!=0;
 f->opt_texture_edge=(shader_id1&SHADER_OPT_TEXTURE_EDGE)!=0;
 f->opt_noise=(shader_id1&SHADER_OPT_NOISE)!=0;
 f->opt_2cyc=(shader_id1&SHADER_OPT_2CYC)!=0;
 f->opt_alpha_threshold=(shader_id1&SHADER_OPT_ALPHA_THRESHOLD)!=0;
 f->opt_invisible=(shader_id1&SHADER_OPT_INVISIBLE)!=0;
 f->opt_grayscale=(shader_id1&SHADER_OPT_GRAYSCALE)!=0;
 f->opt_blur=(shader_id1&SHADER_OPT_BLUR)!=0;
 f->clamp[0][0]=(shader_id1&SHADER_OPT_TEXEL0_CLAMP_S)!=0;
 f->clamp[0][1]=(shader_id1&SHADER_OPT_TEXEL0_CLAMP_T)!=0;
 f->clamp[1][0]=(shader_id1&SHADER_OPT_TEXEL1_CLAMP_S)!=0;
 f->clamp[1][1]=(shader_id1&SHADER_OPT_TEXEL1_CLAMP_T)!=0;
 f->used_textures[0]=false;f->used_textures[1]=false;f->num_inputs=0;
 for(int c=0;c<2;c++)for(int i=0;i<2;i++)for(int j=0;j<4;j++){
  uint8_t x=f->c[c][i][j];
  if(x>=SHADER_INPUT_1 && x<=SHADER_INPUT_7 && x>f->num_inputs)f->num_inputs=x;
  if(x==SHADER_TEXEL0 || x==SHADER_TEXEL0A)f->used_textures[0]=true;
  if(x==SHADER_TEXEL1 || x==SHADER_TEXEL1A)f->used_textures[1]=true;
 }
 for(int c=0;c<2;c++){
  f->do_single[c][0]=f->c[c][0][2]==SHADER_0;
  f->do_single[c][1]=f->c[c][1][2]==SHADER_0;
  f->do_multiply[c][0]=f->c[c][0][1]==SHADER_0 && f->c[c][0][3]==SHADER_0;
  f->do_multiply[c][1]=f->c[c][1][1]==SHADER_0 && f->c[c][1][3]==SHADER_0;
  f->do_mix[c][0]=f->c[c][0][1]==f->c[c][0][3];
  f->do_mix[c][1]=f->c[c][1][1]==f->c[c][1][3];
  f->color_alpha_same[c]=((shader_id0>>(c*32))&0xffff)==((shader_id0>>(c*32+16))&0xffff);
 }
}
