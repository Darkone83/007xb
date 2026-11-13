#include "ge_asset32.h"
#include <stdlib.h>
#include <string.h>

/* A validation/mark pass precedes all writes. Byte marks make shared nodes,
 * shared audio structures and overlapping GDL tails convert exactly once. */
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef char AssertU32[sizeof(U32)==4?1:-1];
static const char *s_error = "";
const char *GEAsset32_Error(void) { return s_error; }
static int fail(const char *message) { s_error=message; return 0; }
typedef struct Convert {
    U8 *image, *marks, *seen;
    U32 bytes;
    U32 *stack, *zeros, *boundaries;
    U32 stack_n, stack_cap, zeros_n, bounds_n;
    GEAsset32Raw16Range raw16;
    void *raw16_user;
} Convert;
static int span(const Convert *c,U32 off,U32 count) {
    return off<=c->bytes && count<=c->bytes-off;
}
static U16 rd16(const Convert *c,U32 o) {
    return (U16)(((U16)c->image[o]<<8)|c->image[o+1]);
}
static U32 rd32(const Convert *c,U32 o) {
    return ((U32)c->image[o]<<24)|((U32)c->image[o+1]<<16)|
           ((U32)c->image[o+2]<<8)|c->image[o+3];
}
static int mark(Convert *c,U32 off,U32 width) {
    U32 i;
    if(!span(c,off,width))return fail("scalar outside resource");
    if(c->marks[off]==width)return 1;
    for(i=0;i<width;i++)if(c->marks[off+i])return fail("overlapping scalar layouts");
    c->marks[off]=(U8)width;
    for(i=1;i<width;i++)c->marks[off+i]=255;
    return 1;
}
static int words(Convert *c,U32 off,U32 count,U32 width) {
    U32 i;
    if(count>c->bytes/width || !span(c,off,count*width))return fail("array outside resource");
    for(i=0;i<count;i++)if(!mark(c,off+i*width,width))return 0;
    return 1;
}
static int begin(Convert *c,void *image,U32 bytes) {
    memset(c,0,sizeof(*c));s_error="";
    if(!image||!bytes||bytes>2u*1024u*1024u)return fail("invalid resource length");
    c->image=(U8*)image;c->bytes=bytes;
    c->marks=(U8*)calloc(bytes,1);
    c->seen=(U8*)calloc(bytes,1);
    if(!c->marks||!c->seen){free(c->marks);free(c->seen);return fail("resource conversion allocation failed");}
    return 1;
}
static int finish(Convert *c,int success) {
    U32 i,j,n;
    if(success)for(i=0;i<c->bytes;i++){
        n=c->marks[i];
        if(n==2||n==4){
            for(j=0;j<n/2;j++){U8 t=c->image[i+j];c->image[i+j]=c->image[i+n-1-j];c->image[i+n-1-j]=t;}
            i+=n-1;
        }
    }
    free(c->marks);free(c->seen);free(c->stack);free(c->zeros);free(c->boundaries);
    return success;
}
static int pointer(Convert *c,U32 value,U32 required,U32 *off) {
    if((value>>24)!=5u)return fail("model pointer has wrong segment");
    *off=value&0xffffffu;
    return span(c,*off,required)?1:fail("model pointer outside image");
}
static int boundary(Convert *c,U32 off) {
    if(c->bounds_n>=c->stack_cap)return fail("too many model regions");
    c->boundaries[c->bounds_n++]=off;return 1;
}
static int pushnode(Convert *c,U32 value) {
    U32 o;
    if(!value)return 1;
    if(!pointer(c,value,24,&o))return 0;
    if(c->seen[o]&1)return 1;
    if(c->stack_n>=c->stack_cap)return fail("too many model nodes");
    c->seen[o]|=1;c->stack[c->stack_n++]=o;
    return boundary(c,o);
}
static int vertex(Convert *c,U32 off,U32 count,int collision) {
    U32 i;
    if(count>c->bytes/16u || !span(c,off,count*16u))return fail("vertex array outside model");
    for(i=0;i<count;i++){
        U32 o=off+16*i;
        if(!words(c,o,collision?4u:6u,2))return 0;
        if(collision && (!mark(c,o+8,4)||!words(c,o+12,2,2)))return 0;
    }
    return 1;
}
static int vertices(Convert *c,U32 value,U32 count,int collision) {
    U32 o;
    if(!value)return count?fail("missing model vertex array"):1;
    if(!pointer(c,value,0,&o)||!boundary(c,o))return 0;
    if(!count){
        if(c->zeros_n>=c->stack_cap)return fail("too many empty vertex regions");
        c->zeros[c->zeros_n++]=o;return 1;
    }
    return vertex(c,o,count,collision);
}
static int gdl(Convert *c,U32 value,unsigned depth) {
    U32 o,w0,w1,op,vo,count;
    if(!value)return 1;
    if(depth>32)return fail("display list nesting too deep");
    if(!pointer(c,value,8,&o)||!boundary(c,o))return 0;
    for(;;o+=8){
        if(!span(c,o,8))return fail("unterminated display list");
        if(c->seen[o]&2)return 1;
        c->seen[o]|=2;
        w0=rd32(c,o);w1=rd32(c,o+4);op=w0>>24;
        if(!words(c,o,2,4))return 0;
        if(op==0xb8)return 1;
        if(op==0x04 && (w1>>24)==5){
            count=(w0&0xffffu)/16u;
            if(!pointer(c,w1,0,&vo)||!vertex(c,vo,count,0))return 0;
        }else if(op==0x06 && (w1>>24)==5){
            if(!gdl(c,w1,depth+1))return 0;
            if(w0&0x00ff0000u)return 1;
        }
    }
}
static int record(Convert *c,U32 op,U32 d) {
    U32 n,nc,v,po;
    static const U8 sizes[25]={0,16,28,28,20,0,0,0,16,36,28,0,40,32,16,28,24,32,8,0,0,20,16,2,32};
    if(op>24||!sizes[op]||!span(c,d,sizes[op]))return fail("unsupported or truncated model record");
    if(!boundary(c,d))return 0;
    switch(op){
    case 1:return words(c,d,2,2)&&mark(c,d+4,4)&&words(c,d+8,4,2);
    case 2:case 3:return words(c,d,3,4)&&words(c,d+12,4,2)&&words(c,d+20,2,4);
    case 4:
        if(!gdl(c,rd32(c,d),0)||!gdl(c,rd32(c,d+4),0))return 0;
        n=rd16(c,d+16);
        return vertices(c,rd32(c,d+12),n,0)&&words(c,d,4,4)&&mark(c,d+16,2);
    case 8:return pushnode(c,rd32(c,d+8))&&words(c,d,3,4)&&words(c,d+12,2,2);
    case 9:return pushnode(c,rd32(c,d+24))&&pushnode(c,rd32(c,d+28))&&words(c,d,8,4)&&words(c,d+32,2,2);
    case 10:return words(c,d,7,4);
    case 12:return words(c,d,8,4)&&words(c,d+32,2,2)&&mark(c,d+36,4);
    case 13:return words(c,d,8,4);
    case 14:return words(c,d,4,4);
    case 15:return words(c,d,7,4);
    case 16:return words(c,d,3,4)&&words(c,d+12,4,2)&&mark(c,d+20,4);
    case 17:return pushnode(c,rd32(c,d+20))&&words(c,d,8,4);
    case 18:return pushnode(c,rd32(c,d))&&mark(c,d,4)&&words(c,d+4,2,2);
    case 21:return words(c,d,3,4)&&words(c,d+12,2,2)&&mark(c,d+16,4);
    case 22:
        n=rd32(c,d);
        if(n>c->bytes/64u)return fail("star-gunfire vertex count invalid");
        return vertices(c,rd32(c,d+4),4*n,0)&&gdl(c,rd32(c,d+8),0)&&words(c,d,4,4);
    case 23:return mark(c,d,2);
    case 24:
        n=rd16(c,d+12);nc=rd16(c,d+14);
        if(!gdl(c,rd32(c,d),0)||!gdl(c,rd32(c,d+4),0)||
           !vertices(c,rd32(c,d+8),n,0)||!vertices(c,rd32(c,d+16),nc,1))return 0;
        v=rd32(c,d+20);
        if(v){if(!pointer(c,v,0,&po)||!words(c,po,n,2)||!boundary(c,po))return 0;}
        return words(c,d,3,4)&&words(c,d+12,2,2)&&words(c,d+16,2,4)&&words(c,d+24,2,2)&&mark(c,d+28,4);
    }
    return fail("unknown model opcode");
}
int GEAsset32_ConvertModel(void *image,unsigned bytes,unsigned switches,unsigned textures) {
    Convert c;U32 i,o,d,op,root,j,end;
    if(switches>256||textures>256)return fail("invalid model header counts");
    if(!begin(&c,image,bytes))return 0;
    c.stack_cap=bytes/4u+1u;
    c.stack=(U32*)malloc(c.stack_cap*sizeof(U32));
    c.zeros=(U32*)malloc(c.stack_cap*sizeof(U32));
    c.boundaries=(U32*)malloc(c.stack_cap*sizeof(U32));
    if(!c.stack||!c.zeros||!c.boundaries){fail("model traversal allocation failed");return finish(&c,0);}
    root=switches*4u+textures*12u;
    if(!span(&c,root,24)||!words(&c,0,switches,4)||!pushnode(&c,0x05000000u|root))return finish(&c,0);
    for(i=0;i<switches;i++)if(!pushnode(&c,rd32(&c,i*4)))return finish(&c,0);
    for(i=0;i<textures;i++)if(!mark(&c,switches*4u+i*12u,4))return finish(&c,0);
    while(c.stack_n){
        o=c.stack[--c.stack_n];op=rd16(&c,o)&255u;
        if(!pointer(&c,rd32(&c,o+4),0,&d))return finish(&c,0);
        if(!mark(&c,o,2)||!words(&c,o+4,5,4)||!pushnode(&c,rd32(&c,o+12)))return finish(&c,0);
        if(op!=8&&op!=18&&!pushnode(&c,rd32(&c,o+20)))return finish(&c,0);
        if(!record(&c,op,d))return finish(&c,0);
    }
    for(i=0;i<c.zeros_n;i++){
        o=c.zeros[i];end=bytes;
        for(j=0;j<c.bounds_n;j++)if(c.boundaries[j]>o&&c.boundaries[j]<end)end=c.boundaries[j];
        if((end-o)&15u){fail("zero-count vertex extent misaligned");return finish(&c,0);}
        if(!vertex(&c,o,(end-o)/16u,0))return finish(&c,0);
    }
    return finish(&c,1);
}

/* Native BG metadata has the original record widths. Only the desktop
 * sidecar needs to enlarge portal pointers and move the compressed tail. */
static int bg_offset(U32 value,U32 end,U32 *off) {
    if((value>>24)!=15u)return fail("BG pointer has wrong segment");
    *off=value&0xffffffu;
    return *off<=end?1:fail("BG pointer outside file");
}
static int bg_portal_points(Convert *c,U32 value,U32 first,U32 end) {
    U32 o,n;
    if(!bg_offset(value,end,&o)||o<first||(o&3u)||!span(c,o,4))
        return fail("BG portal points outside metadata");
    if(c->seen[o])return 1;
    n=c->image[o];
    if(!n||n>20||o>end||4u+12u*n>end-o)
        return fail("BG portal point count invalid");
    c->seen[o]=1;
    return words(c,o,4,1)&&words(c,o+4,3*n,4);
}
int GEAsset32_ConvertBg(void *image,unsigned bytes,unsigned file_bytes) {
    Convert c;
    U32 room,portal,env,extra,room_end,first,prefix,o,v,t,portal_end,limit;
    int terminated;
    if(bytes<64u||file_bytes<bytes||file_bytes>0x01000000u)
        return fail("invalid BG read length");
    if(!begin(&c,image,bytes))return 0;
    if(rd32(&c,0)!=0||!bg_offset(rd32(&c,4),file_bytes,&room)||
       !bg_offset(rd32(&c,8),file_bytes,&portal)){
        fail("invalid BG header");return finish(&c,0);
    }
    env=rd32(&c,12);extra=rd32(&c,16);
    if(extra){fail("unsupported BG auxiliary float table");return finish(&c,0);}
    if(env&&!bg_offset(env,file_bytes,&env))return finish(&c,0);
    room_end=env?env:portal;
    if(room<20u||(room&3u)||(portal&3u)||(env&3u)||
       room_end<=room||room_end>portal||(room_end-room)%24u||
       room_end-room<48u||!span(&c,room+24u,4u)||
       !bg_offset(rd32(&c,room+24u),file_bytes,&first)){
        fail("invalid BG room table");return finish(&c,0);
    }
    prefix=(first+15u)&~15u;
    if(first<portal+8u||prefix>file_bytes){
        fail("invalid BG metadata extent");return finish(&c,0);
    }
    if(bytes!=64u&&bytes<prefix){
        fail("truncated BG metadata read");return finish(&c,0);
    }
    if(!words(&c,0,5,4))return finish(&c,0);

    /* The 64-byte probe contains the first point offset needed by the
     * caller to size its full read. Convert only whole available fields. */
    limit=room_end<bytes?room_end:bytes;
    for(o=room;o+4u<=limit;o+=4u){
        if((o-room)%24u<12u){
            v=rd32(&c,o);
            if(v&&(!bg_offset(v,file_bytes,&t)||t<first)){
                fail("BG room binary offset invalid");return finish(&c,0);
            }
        }
        if(!mark(&c,o,4))return finish(&c,0);
    }
    if(bytes<prefix)return finish(&c,1);

    if(portal>=first||room_end>first||
       (env&&(portal-env)%8u)){
        fail("BG table extent invalid");return finish(&c,0);
    }
    /* All room records, including the zero sentinel, must fit before the
     * portal table. The reader's room-count loop relies on that sentinel. */
    terminated=0;
    for(o=room+24u;o<room_end;o+=24u){
        if(!rd32(&c,o+4u))terminated=1;
        else if(terminated){fail("BG room records follow sentinel");return finish(&c,0);}
    }
    if(!terminated){fail("BG room sentinel missing");return finish(&c,0);}

    portal_end=0;
    for(o=portal;o+8u<=first;o+=8u){
        if(!mark(&c,o,4)||!words(&c,o+4u,4,1))return finish(&c,0);
        if(!rd32(&c,o)){portal_end=o+8u;break;}
    }
    if(!portal_end){fail("BG portal sentinel missing");return finish(&c,0);}
    for(o=portal;o+8u<portal_end;o+=8u)
        if(!bg_portal_points(&c,rd32(&c,o),portal_end,first))return finish(&c,0);

    if(env){
        terminated=0;
        for(o=env;o+8u<=portal;o+=8u){
            v=c.image[o];
            if(!words(&c,o,4,1)||!mark(&c,o+4u,4))return finish(&c,0);
            if(v==100u&&rd32(&c,o+4u)&&
               !bg_portal_points(&c,rd32(&c,o+4u),portal_end,first))return finish(&c,0);
            if(!v){terminated=1;break;}
        }
        if(!terminated){fail("BG visibility sentinel missing");return finish(&c,0);}
    }
    return finish(&c,1);
}

int GEAsset32_ConvertStan(void *image,unsigned bytes) {
    Convert c;
    U32 o,regions_end,first,tile_end,n,tail,i,v,target;
    int terminated;
    if(!begin(&c,image,bytes))return 0;
    if(!span(&c,0,12u)||rd32(&c,0)!=0){
        fail("invalid STAN header");return finish(&c,0);
    }
    first=rd32(&c,4);regions_end=0;
    for(o=4u;span(&c,o,4u);o+=4u){
        if(!mark(&c,o,4))return finish(&c,0);
        if(!rd32(&c,o)){regions_end=o+4u;break;}
    }
    if(!regions_end||first!=regions_end||!span(&c,first,8u)||!mark(&c,0,4)){
        fail("invalid STAN region offsets");return finish(&c,0);
    }
    terminated=0;tile_end=first;
    for(o=first;span(&c,o,4u);o+=8u+8u*n){
        if(!rd32(&c,o)){terminated=1;tile_end=o;break;}
        if(!span(&c,o,8u)){fail("truncated STAN tile");return finish(&c,0);}
        tail=rd16(&c,o+6u);n=tail>>12;
        if(n<3u||n>10u||!span(&c,o,8u+8u*n)){
            fail("unsupported or truncated STAN tile");return finish(&c,0);
        }
        if((tail&15u)>=n||((tail>>4)&15u)>=n||((tail>>8)&15u)>=n){
            fail("STAN tile point index invalid");return finish(&c,0);
        }
        c.seen[o]=1;
        /* stanMatchTileName aliases id[0..1] as a native u16, while id[2]
         * and room at byte3 remain byte fields. */
        if(!mark(&c,o,2)||!words(&c,o+4u,2,2)||!words(&c,o+8u,4*n,2))
            return finish(&c,0);
    }
    if(!terminated){fail("STAN tile sentinel missing");return finish(&c,0);}
    for(o=4u;o+4u<regions_end;o+=4u){
        v=rd32(&c,o);
        if(v<first||v>=tile_end||!c.seen[v]){
            fail("STAN region does not reference a tile");return finish(&c,0);
        }
    }
    for(o=first;o<tile_end;o+=8u+8u*n){
        n=rd16(&c,o+6u)>>12;
        for(i=0;i<n;i++){
            v=rd16(&c,o+8u+8u*i+6u);
            if(!v)continue;
            /* Links are eight-byte offsets from (first tile - 0x80).
             * Adding before subtracting avoids a negative base pointer. */
            target=first+8u*v;
            if(target<128u){fail("STAN tile link underflow");return finish(&c,0);}
            target-=128u;
            if(target<first||target>=tile_end||!c.seen[target]){
                fail("STAN link does not reference a tile");return finish(&c,0);
            }
        }
    }
    return finish(&c,1);
}

/* ALBankFile graph: offsets keep their original 32-bit native locations. */
enum { AB_FILE=1,AB_BANK,AB_INST,AB_SOUND,AB_WAVE,AB_ENV,AB_KEY,AB_ADPCM_LOOP,AB_RAW_LOOP,AB_BOOK };
static int audio_visit(Convert *c,U32 o,int kind,unsigned depth) {
    U32 i,n,v,p,a,b;U8 type;
    if(depth>64||!span(c,o,1))return fail("audio-bank reference outside image");
    if(c->seen[o])return c->seen[o]==kind?1:fail("audio-bank structure types overlap");
    c->seen[o]=(U8)kind;
    switch(kind){
    case AB_FILE:
        if(!span(c,0,4)||rd16(c,0)!=0x4231)return fail("audio-bank revision invalid");
        n=rd16(c,2);if(n>64||!span(c,4,4*n))return fail("audio-bank count invalid");
        if(!words(c,0,2,2)||!words(c,4,n,4))return 0;
        for(i=0;i<n;i++){v=rd32(c,4+4*i);if(v&&!audio_visit(c,v,AB_BANK,depth+1))return 0;}return 1;
    case AB_BANK:
        if(!span(c,o,12))return fail("truncated audio bank");
        n=rd16(c,o);if(n>32767||!span(c,o+12,4*n))return fail("instrument count invalid");
        if(!mark(c,o,2)||!words(c,o+4,2+n,4))return 0;
        p=rd32(c,o+8);if(p&&!audio_visit(c,p,AB_INST,depth+1))return 0;
        for(i=0;i<n;i++){v=rd32(c,o+12+4*i);if(v&&!audio_visit(c,v,AB_INST,depth+1))return 0;}return 1;
    case AB_INST:
        if(!span(c,o,16))return fail("truncated instrument");
        n=rd16(c,o+14);if(n>32767||!span(c,o+16,4*n))return fail("sound count invalid");
        if(!words(c,o+12,2,2)||!words(c,o+16,n,4))return 0;
        for(i=0;i<n;i++){v=rd32(c,o+16+4*i);if(v&&!audio_visit(c,v,AB_SOUND,depth+1))return 0;}return 1;
    case AB_SOUND:
        if(!span(c,o,16)||!words(c,o,3,4))return fail("truncated sound");
        a=rd32(c,o);b=rd32(c,o+4);p=rd32(c,o+8);
        return (!a||audio_visit(c,a,AB_ENV,depth+1))&&(!b||audio_visit(c,b,AB_KEY,depth+1))&&(!p||audio_visit(c,p,AB_WAVE,depth+1));
    case AB_WAVE:
        if(!span(c,o,20))return fail("truncated wavetable");
        type=c->image[o+8];p=rd32(c,o+12);
        if(type>1||!words(c,o,2,4)||!mark(c,o+12,4))return fail("invalid wavetable type");
        if(type==0){v=rd32(c,o+16);return mark(c,o+16,4)&&(!p||audio_visit(c,p,AB_ADPCM_LOOP,depth+1))&&(!v||audio_visit(c,v,AB_BOOK,depth+1));}
        if(c->raw16&&!c->raw16(c->raw16_user,rd32(c,o),rd32(c,o+4)))return fail("RAW16 range callback rejected span");
        return !p||audio_visit(c,p,AB_RAW_LOOP,depth+1);
    case AB_ENV:return span(c,o,16)&&words(c,o,3,4);
    case AB_KEY:return span(c,o,6)?1:fail("truncated keymap");
    case AB_ADPCM_LOOP:return span(c,o,44)&&words(c,o,3,4)&&words(c,o+12,16,2);
    case AB_RAW_LOOP:return words(c,o,3,4);
    case AB_BOOK:
        if(!span(c,o,8))return fail("truncated ADPCM book");
        a=rd32(c,o);b=rd32(c,o+4);
        if(!a||a>16||!b||b>16)return fail("invalid ADPCM predictor count");
        return words(c,o,2,4)&&words(c,o+8,8*a*b,2);
    }
    return fail("unknown audio-bank structure");
}
int GEAsset32_ConvertAudioBank(void *image,unsigned bytes) {
    Convert c;if(!begin(&c,image,bytes))return 0;
    return finish(&c,audio_visit(&c,0,AB_FILE,0));
}
int GEAsset32_ScanRaw16(const void *image,unsigned bytes,GEAsset32Raw16Range cb,void *user) {
    Convert c;int ok;
    if(!cb)return fail("missing RAW16 callback");
    if(!begin(&c,(void*)image,bytes))return 0;
    c.raw16=cb;c.raw16_user=user;ok=audio_visit(&c,0,AB_FILE,0);
    finish(&c,0);return ok;
}

/* The original two banks contain just one RAW16 span (the remaining audio
 * is ADPCM). Keep a bounded registry so cartridge streaming needs no ROM
 * image or cached wavetable allocation. Registration runs before audio
 * synthesis starts; the audio thread only reads this immutable registry. */
#define RAW16_RANGES 512u
typedef struct RawRange { U32 start,end; } RawRange;
static RawRange s_raw_ranges[RAW16_RANGES];
static U32 s_raw_count;
typedef struct RawRegistration {
    RawRange ranges[RAW16_RANGES];
    U32 count,base,bytes;
} RawRegistration;
void GEAsset32_ResetAudioTables(void) { s_raw_count=0; }
static int raw_range(void *user,unsigned offset,unsigned bytes) {
    RawRegistration *r=(RawRegistration*)user;
    U32 start,end,i,j;
    if((offset|bytes)&1u || offset>r->bytes || bytes>r->bytes-offset)
        return fail("RAW16 samples outside wavetable or not sample aligned");
    if(!bytes)return 1;
    start=r->base+offset;end=start+bytes;
    for(i=0;i<r->count;i++){
        if(end<r->ranges[i].start || start>r->ranges[i].end)continue;
        if(r->ranges[i].start<start)start=r->ranges[i].start;
        if(r->ranges[i].end>end)end=r->ranges[i].end;
        for(j=i+1;j<r->count;j++)r->ranges[j-1]=r->ranges[j];
        --r->count;--i;
    }
    if(r->count>=RAW16_RANGES)return fail("too many RAW16 sample ranges");
    r->ranges[r->count].start=start;r->ranges[r->count].end=end;++r->count;
    return 1;
}
int GEAsset32_RegisterAudioTable(const void *bank,unsigned bank_bytes,
                                unsigned table_cart_address,unsigned table_bytes) {
    RawRegistration *r;int ok;
    if((table_cart_address&1u)||table_bytes>0xffffffffu-table_cart_address)
        return fail("invalid wavetable cartridge span");
    r=(RawRegistration*)malloc(sizeof(*r));
    if(!r)return fail("RAW16 registration allocation failed");
    memcpy(r->ranges,s_raw_ranges,s_raw_count*sizeof(RawRange));
    r->count=s_raw_count;r->base=table_cart_address;r->bytes=table_bytes;
    ok=GEAsset32_ScanRaw16(bank,bank_bytes,raw_range,r);
    if(ok){memcpy(s_raw_ranges,r->ranges,r->count*sizeof(RawRange));s_raw_count=r->count;}
    free(r);return ok;
}
int GEAsset32_SwapRaw16Dma(unsigned cart_address,void *destination,unsigned bytes) {
    U32 i,start,end,dma_end,p;U8 *dst=(U8*)destination;
    if((!dst&&bytes)||bytes>0xffffffffu-cart_address)return fail("invalid cartridge DMA span");
    dma_end=cart_address+bytes;
    /* Check every intersection before the first write, including malformed
     * reads that clip a sample at either end. Original libaudio reads are
     * eight-byte aligned and cannot clip a 16-bit sample. */
    for(i=0;i<s_raw_count;i++){
        start=cart_address>s_raw_ranges[i].start?cart_address:s_raw_ranges[i].start;
        end=dma_end<s_raw_ranges[i].end?dma_end:s_raw_ranges[i].end;
        if(start<end && ((start|end)&1u))return fail("cartridge DMA clips a RAW16 sample");
    }
    for(i=0;i<s_raw_count;i++){
        start=cart_address>s_raw_ranges[i].start?cart_address:s_raw_ranges[i].start;
        end=dma_end<s_raw_ranges[i].end?dma_end:s_raw_ranges[i].end;
        for(p=start;p<end;p+=2){U32 o=p-cart_address;U8 t=dst[o];dst[o]=dst[o+1];dst[o+1]=t;}
    }
    return 1;
}
int GEAsset32_ConvertFont(void *image,unsigned bytes) {
    Convert c;U32 i,p;
    if(bytes<169u*4u+94u*24u)return fail("font image truncated");
    if(!begin(&c,image,bytes))return 0;
    if(!words(&c,0,169,4))return finish(&c,0);
    for(i=0;i<94;i++){
        p=rd32(&c,676u+24u*i+20u);
        if(p&&p>=bytes){fail("glyph pixel offset outside font");return finish(&c,0);}
        if(!words(&c,676u+24u*i,6,4))return finish(&c,0);
    }
    return finish(&c,1);
}
int GEAsset32_ConvertMusicTable(void *image,unsigned bytes) {
    Convert c;U32 i,n,available;
    if(bytes<4)return fail("music table truncated");
    if(!begin(&c,image,bytes))return 0;
    n=rd16(&c,0);available=(bytes-4)/8;
    if(n>available)n=available; /* Original music init first reads only the header. */
    if(!mark(&c,0,2))return finish(&c,0);
    for(i=0;i<n;i++)if(!mark(&c,4+8*i,4)||!words(&c,8+8*i,2,2))return finish(&c,0);
    return finish(&c,1);
}
int GEAsset32_ConvertCseq(void *image,unsigned bytes) {
    Convert c;if(bytes<68)return fail("compact sequence header truncated");
    if(!begin(&c,image,bytes))return 0;
    return finish(&c,words(&c,0,17,4));
}
int GEAsset32_ConvertLanguage(void *image,unsigned bytes) {
    Convert c;U32 o=0,limit=bytes,v;
    if(!begin(&c,image,bytes))return 0;
    while(o+4<=limit){
        v=rd32(&c,o);
        if(v&&(v<o+4||v>=bytes))break;
        if(v&&v<limit)limit=v;
        if(!mark(&c,o,4))return finish(&c,0);
        o+=4;
    }
    return finish(&c,1);
}
int GEAsset32_ConvertBriefing(void *image,unsigned bytes) {
    Convert c;if(bytes<48)return fail("briefing truncated");
    if(!begin(&c,image,bytes))return 0;
    return finish(&c,words(&c,0,24,2));
}
static int animation_table(Convert *c,const int *table) {
    U32 i,o,bd,bs,p;
    if(!table)return 1;
    for(i=0;i<4096&&table[i];i++){
        if(table[i]==1)continue;
        o=(U32)table[i];
        if(!span(c,o,20))return fail("animation record offset outside blob");
        bd=rd32(c,o+8);bs=rd32(c,o+16);
        if(bd>bs||bs>c->bytes||(bs-bd)%6)return fail("animation descriptor range invalid");
        if(!mark(c,o,4)||!mark(c,o+4,2)||!mark(c,o+8,4)||!words(c,o+12,2,2)||!mark(c,o+16,4))return 0;
        for(p=bd;p<bs;p+=6)if(!mark(c,p,2)||!mark(c,p+4,2))return 0;
    }
    return i<4096?1:fail("unterminated animation offset table");
}
int GEAsset32_ConvertAnimations(void *image,unsigned bytes,const int *a,const int *b) {
    Convert c;if(!begin(&c,image,bytes))return 0;
    return finish(&c,animation_table(&c,a)&&animation_table(&c,b));
}

/* Native32 Usetup/Ump_setup images retain all original four-byte offsets.
 * Visit typed records rather than swapping whole files: AI bytecode, strings,
 * RGBA bytes, type tags and packed u8 fields must remain byte-identical.
 * Like the other converters, no resource byte changes before validation ends. */
enum {
    SETUP32_HEADER=1, SETUP32_WAYPOINT, SETUP32_WAYGROUP,
    SETUP32_PATROL, SETUP32_AILIST, SETUP32_PAD, SETUP32_BOUNDPAD,
    SETUP32_NAMES, SETUP32_INTRO, SETUP32_PROP, SETUP32_IDS,
    SETUP32_STRING, SETUP32_AICODE
};
static int setup32Claim(Convert *c,U32 off,U32 bytes,U8 kind)
{
    U32 i;
    if(!span(c,off,bytes))return fail("setup record outside image");
    for(i=0;i<bytes;i++)
        if(c->seen[off+i] && c->seen[off+i]!=kind)
            return fail("setup record types overlap");
    for(i=0;i<bytes;i++)c->seen[off+i]=kind;
    return 1;
}
static int setup32Offset(Convert *c,U32 off,U32 required,int aligned)
{
    if(off<40 || (aligned && (off&3)) || !span(c,off,required))
        return fail("setup offset outside image or misaligned");
    return 1;
}
static int setup32String(Convert *c,U32 off)
{
    U32 end=off;
    if(!setup32Offset(c,off,1,0))return 0;
    while(end<c->bytes && c->image[end])++end;
    if(end==c->bytes)return fail("unterminated setup string");
    return setup32Claim(c,off,end-off+1,SETUP32_STRING);
}
static int setup32Ids(Convert *c,U32 off)
{
    U32 o,value;
    if(!setup32Offset(c,off,4,1))return 0;
    for(o=off;;o+=4){
        if(!span(c,o,4))return fail("unterminated setup ID list");
        value=rd32(c,o);
        if(!setup32Claim(c,o,4,SETUP32_IDS)||!mark(c,o,4))return 0;
        if(value&0x80000000u)return 1;
    }
}
static int setup32Table(Convert *c,U32 off,unsigned table)
{
    U32 o,size,term,value,other;
    U8 kind;
    if(!off)return 1;
    switch(table){
    case 0:size=16;term=0;kind=SETUP32_WAYPOINT;break;
    case 1:size=12;term=0;kind=SETUP32_WAYGROUP;break;
    case 4:size=8;term=0;kind=SETUP32_PATROL;break;
    case 5:size=8;term=0;kind=SETUP32_AILIST;break;
    case 6:size=44;term=36;kind=SETUP32_PAD;break;
    case 7:size=68;term=36;kind=SETUP32_BOUNDPAD;break;
    case 8:case 9:size=4;term=0;kind=SETUP32_NAMES;break;
    default:return fail("invalid setup table kind");
    }
    if(!setup32Offset(c,off,size,1))return 0;
    for(o=off;;o+=size){
        int last;
        if(!span(c,o,size))return fail("unterminated setup table");
        value=rd32(c,o+term);
        last=table==0 ? (value&0x80000000u)!=0 : value==0;
        if(!setup32Claim(c,o,size,kind))return 0;
        if(table==4){
            if(!mark(c,o,4)||!mark(c,o+6,2))return 0;
        }else if(!words(c,o,size/4,4))return 0;
        if(last)return 1;
        switch(table){
        case 0:
            if(!setup32Ids(c,rd32(c,o+4)))return 0;
            break;
        case 1:
            other=rd32(c,o+4);
            if(!setup32Ids(c,value)||!setup32Ids(c,other))return 0;
            break;
        case 4:
            if(!setup32Ids(c,value))return 0;
            break;
        case 5:
            /* Bytecode is validated against typed-region overlap after all
             * tables have been visited; its byte operands are never swapped. */
            if(!setup32Offset(c,value,1,0))return 0;
            break;
        case 6:case 7:case 8:case 9:
            if(!setup32String(c,value))return 0;
            break;
        }
    }
}
static int setup32Intro(Convert *c,U32 off)
{
    static const U8 sizes[10]={12,16,16,32,8,8,40,12,8,4};
    U32 o,type,size;
    if(!off)return 1;
    for(o=off;;o+=size){
        if(!span(c,o,4))return fail("unterminated setup intro");
        type=rd32(c,o);
        if(type>=10)return fail("unknown setup intro record");
        size=sizes[type];
        if(!setup32Claim(c,o,size,SETUP32_INTRO))return 0;
        if(type==6){
            /* IntroCamera.lang1c is two u16s; its consumer reads [1].
             * Swap each halfword in place so the language ID stays there. */
            if(!words(c,o,7,4)||!words(c,o+28,2,2)||!words(c,o+32,2,4))return 0;
        }else if(!words(c,o,size/4,4))return 0;
        if(type==9)return 1;
    }
}
static int setup32ObjectPrefix(Convert *c,U32 o)
{
    /* Header = u16 scale,u8 state,u8 type; object/pad are separate s16s.
     * The final two words are RGBA byte vectors, not integer scalars. */
    return mark(c,o,2) && words(c,o+4,2,2) && words(c,o+8,28,4);
}
static int setup32Monitor(Convert *c,U32 o)
{
    /* cmdlist, two halfwords, tconfig/floats, twelve colour bytes, two floats. */
    return mark(c,o,4) && words(c,o+4,2,2) &&
        words(c,o+8,22,4) && words(c,o+108,2,4);
}
static int setup32PropRecord(Convert *c,U32 o,U32 type,U32 count)
{
    U32 i,n;
    int object;
    object=(type>=1 && type<=8 && type!=2) || type==10 || type==11 ||
        type==12 || type==13 || type==17 || type==20 || type==21 ||
        type==36 || type==39 || type==40 || type==42 || type==43 ||
        type==45 || type==47;
    if(!setup32Claim(c,o,count*4,SETUP32_PROP))return 0;
    if(object){
        if(count<32 || !setup32ObjectPrefix(c,o))return 0;
        if(type==10 || type==11){
            n=type==10?1:4;
            for(i=0;i<n;i++)if(!setup32Monitor(c,o+128+i*116))return 0;
            /* Multi-monitor ImageNums is u8[4]; single monitor tail is s32[3]. */
            return type==11 ? 1 : words(c,o+244,3,4);
        }
        for(i=32;i<count;i++){
            U32 p=o+i*4;
            if((type==1 && i==38) || ((type==39 || type==40) && i==33) || type==20){
                if(!words(c,p,2,2))return 0;
            }else if((type==1 && i==47) || (type==8 && i==32)){
                if(!mark(c,p+2,2))return 0;
            }else if(type==1 && i==49){
                if(!mark(c,p,2))return 0;
            }else if(!mark(c,p,4))return 0;
        }
        return 1;
    }
    if(!mark(c,o,2))return 0;
    if(type==9)return words(c,o+4,10,2) && mark(c,o+24,4);
    if(type==18)return mark(c,o+4,4) && mark(c,o+8,2);
    if(type==22)return words(c,o+4,2,2) && words(c,o+8,2,4);
    return words(c,o+4,count-1,4);
}
static int setup32Props(Convert *c,U32 off)
{
    /* Serialized counts, not sizeof: type14 has no serialized next pointer,
     * type6 stops at CCTV.unkE8, and objective COPY_ITEM occupies three words. */
    static const U8 counts[49]={
        0,64,2,32,33,32,59,33,34,7,64,149,32,54,3,0,0,
        32,3,4,45,34,4,4,1,2,2,2,2,2,4,1,4,5,3,4,
        32,10,4,44,45,0,32,32,5,56,7,37,1
    };
    U32 o,type,count;
    if(!off)return 1;
    for(o=off;;o+=count*4){
        if(!span(c,o,4))return fail("unterminated setup prop list");
        type=c->image[o+3];
        if(type>=49 || !counts[type])return fail("unsupported setup prop type");
        count=counts[type];
        if(!span(c,o,count*4))return fail("truncated setup prop record");
        if(!setup32PropRecord(c,o,type,count))return 0;
        if(type==48)return 1;
    }
}
int GEAsset32_ConvertSetup(void *image,unsigned bytes)
{
    Convert c;
    U32 offsets[10],i,o,code;
    if(bytes<40)return fail("setup header truncated");
    if(!begin(&c,image,bytes))return 0;
    if(!setup32Claim(&c,0,40,SETUP32_HEADER)||!words(&c,0,10,4))return finish(&c,0);
    for(i=0;i<10;i++){
        offsets[i]=rd32(&c,i*4);
        if(offsets[i] && !setup32Offset(&c,offsets[i],4,1))return finish(&c,0);
    }
    for(i=0;i<10;i++){
        if(i==2 || i==3)continue;
        if(!setup32Table(&c,offsets[i],i))return finish(&c,0);
    }
    if(!setup32Intro(&c,offsets[2]) || !setup32Props(&c,offsets[3]))return finish(&c,0);
    if(offsets[5])for(o=offsets[5];rd32(&c,o);o+=8){
        /* Table traversal above has already bounded this loop and each offset. */
        code=rd32(&c,o);
        if(c.seen[code] && c.seen[code]!=SETUP32_AICODE){
            fail("setup AI bytecode overlaps a typed record");return finish(&c,0);
        }
        c.seen[code]=SETUP32_AICODE;
    }
    return finish(&c,1);
}
