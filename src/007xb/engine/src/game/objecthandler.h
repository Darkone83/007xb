#ifndef _OBJECTHANDLER_
#define _OBJECTHANDLER_
#include <ultra64.h>
#include <bondtypes.h>
#include <image.h>
#ifdef PORT
#include <stddef.h>
#endif

struct bondstruct_unk_animation_related {
    char* uselessPointer; // Is incremented like a count when an animation is copied from ROM to RAM but it's never read
    char* animBufferPtr1; 
    char* animBufferPtr2; 
};

struct bondstruct_unk_op07_related {
    s32 unk00;
    s32 unk04;
    s32 unk0C;
};

#ifdef PORT
/* The PORT model manager casts each slot to struct Model. Its in-use flag,
 * RW-data capacity and RW-data pointer must share the Model field offsets,
 * and modelInit/animInit need storage for the entire Model.
 *
 * Derive the layout from Model: native Xbox uses 188 bytes with obj/datas at
 * 0x08/0x10; PC64 uses 248 bytes with obj/datas at 0x10/0x20. Hardcoded PC64
 * padding put the Xbox slot fields at 0x10/0x20 and allocated only 184 bytes,
 * breaking allocation/reuse and overrunning an animated slot by four bytes.
 * Retain the public slot field names and the original N64 layout below. */
struct AnimModelSlot {
    s16  unk00;
    s16  unk02;                  /* Model.rwdatalen */
    char pad04[offsetof(struct Model, obj) - 4];
    s32  unk08;                  /* low word of Model.obj = in-use flag */
    char pad14[offsetof(struct Model, datas) - offsetof(struct Model, obj) - sizeof(s32)];
    void *unk10;                 /* Model.datas (RW-data pool pointer) */
    char pad28[sizeof(struct Model) - offsetof(struct Model, datas) - sizeof(void *)];
};

struct ModelSlot {
    s16  unk00;
    s16  unk02;                  /* Model.rwdatalen */
    char pad04[offsetof(struct Model, obj) - 4];
    s32  unk08;                  /* low word of Model.obj = in-use flag */
    char pad14[offsetof(struct Model, datas) - offsetof(struct Model, obj) - sizeof(s32)];
    void *unk10;                 /* Model.datas (RW-data pool pointer) */
    char pad28[sizeof(struct Model) - offsetof(struct Model, datas) - sizeof(void *)];
};

/* These structs are cast to Model throughout model.c. Reject an ABI drift
 * at compile time before a slot can corrupt its neighbours or RW-data. */
typedef char ModelSlotLayoutCheck[
    sizeof(struct ModelSlot) == sizeof(struct Model) &&
    offsetof(struct ModelSlot, unk02) == offsetof(struct Model, rwdatalen) &&
    offsetof(struct ModelSlot, unk08) == offsetof(struct Model, obj) &&
    offsetof(struct ModelSlot, unk10) == offsetof(struct Model, datas) ? 1 : -1];
typedef char AnimModelSlotLayoutCheck[
    sizeof(struct AnimModelSlot) == sizeof(struct Model) &&
    offsetof(struct AnimModelSlot, unk02) == offsetof(struct Model, rwdatalen) &&
    offsetof(struct AnimModelSlot, unk08) == offsetof(struct Model, obj) &&
    offsetof(struct AnimModelSlot, unk10) == offsetof(struct Model, datas) ? 1 : -1];
#else
struct AnimModelSlot {
    s16 unk00;
    s16 unk02;
    s32 unk04;
    s32 unk08;
    s32 unk0c;
    void *unk10;
    s32 unk14;
    s32 unk18;
    s32 unk1c;
    s32 unk20;
    s32 unk24;
    s32 unk28;
    s32 unk2c;
    s32 unk30;
    s32 unk34;
    s32 unk38;
    s32 unk3c;
    s32 unk40;
    s32 unk44;
    s32 unk48;
    s32 unk4c;
    s32 unk50;
    s32 unk54;
    s32 unk58;
    s32 unk5c;
    s32 unk60;
    s32 unk64;
    s32 unk68;
    s32 unk6c;
    s32 unk70;
    s32 unk74;
    s32 unk78;
    s32 unk7c;
    s32 unk80;
    s32 unk84;
    s32 unk88;
    s32 unk8c;
    s32 unk90;
    s32 unk94;
    s32 unk98;
    s32 unk9c;
    s32 unka0;
    s32 unka4;
    s32 unka8;
    s32 unkac;
    s32 unkb0;
    s32 unkb4;
    s32 unkb8;

    // is this struct size 0xbc or 0xc0 ?
    //s32 unkbc;
};

struct ModelSlot {
    s16 unk00;
    s16 unk02;
    s32 unk04;
    s32 unk08;
    s32 unk0c;
    void *unk10;
    s32 unk14;
    s32 unk18;
    s32 unk1c;
};
#endif

extern struct AnimModelSlot *g_AnimModelSlots;
extern struct ModelSlot *g_ModelSlots;

extern struct ModelHitEntry *g_ModelHitFreeList;
extern s32 g_ModelDistanceDisabled;
extern f32 g_ModelDistanceScale;
extern u32 g_ModelAnimMergingEnabled;
extern s32 D_80036410;
extern struct bondstruct_unk_animation_related* D_80036414;
extern s32 D_80036418;
extern s32 D_8003641C;
extern u32 D_800363F0;

extern coord3d D_80036094;
extern coord3d D_800360A0;
extern coord3d D_800360AC;
extern coord3d D_800360B8;
extern coord3d D_80036244;
extern coord3d D_80036254;

extern struct Vertex* (*vtxallocator)(s32 numvertices);
extern void (*g_ModelJointPositionedFunc)(s32 mtxindex, Mtxf *mtx);
extern struct bondstruct_unk_op07_related D_800360C4[];
extern Vertex D_800363E0;
extern Vtx D_800363F8;
extern coord3d D_80036408;

void fileLoad(ModelFileHeader *header,char *name);
void load_object_into_memory_unused_maybe(ModelFileHeader *header,int *recallstring,int *targetloc,int sizeleft);

// tentative signature
PropRecord *chrGiveWeapon(ChrRecord *self, s32 PropID, ITEM_IDS ItemID, s32 flags);

// called with struct ChrRecord->field_20
ModelHitEntry* sub_GAME_7F06B120(ModelHitEntry* head, Model* context);
void sub_GAME_7F06B248(ModelHitEntry *entry);
void drawjointlist(ModelRenderData *arg0, ModelHitEntry *entry);
void sub_GAME_7F06B29C(ModelHitEntry *arg0);
ModelHitEntry *sub_GAME_7F06BB28(ModelHitEntry *modelhit);
s32 probably_damage_detail_blood_effect_related(ModelHitEntry **entryptr, coord3d *raypos, coord3d *raydir, Model **outModel, ModelNode **inoutNode);
s32 sub_GAME_7F06C010(ModelHitEntry **entryptr, coord3d *modelRayStart, coord3d *modelRayDir, Model **outModel, ModelNode **outNode);

void load_object_fill_header(struct ModelFileHeader *objheader, u8 *name, u8* dst, s32 size, struct texpool * buffer);


#endif
