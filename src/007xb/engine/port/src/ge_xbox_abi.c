/* Compile-time checks for structures shared with original ROM records and
 * the Xbox-facing libultra bridge. A mismatch must fail the build. */
#include <stddef.h>
#include <ultra64.h>
#include "bondtypes.h"
#include "sched.h"
#include "game/zlib.h"
#include "game/bg.h"
#include "game/ramromreplay.h"
#include "game/image.h"
#include "joy.h"
#include "snd.h"
#define GE_ABI_ASSERT(name, condition) typedef char ge_abi_##name[(condition) ? 1 : -1]
GE_ABI_ASSERT(pointer32, sizeof(void *) == 4);
GE_ABI_ASSERT(size_t4, sizeof(size_t) == 4);
GE_ABI_ASSERT(address_word4, sizeof(uintptr_t) == 4);
GE_ABI_ASSERT(u32_4, sizeof(u32) == 4);
GE_ABI_ASSERT(u64_8, sizeof(u64) == 8);
GE_ABI_ASSERT(float4, sizeof(f32) == 4);
GE_ABI_ASSERT(gfx8, sizeof(Gfx) == 8);
GE_ABI_ASSERT(gfx_w1_4, offsetof(Gfx, words.w1) == 4);
GE_ABI_ASSERT(gfx_dma_addr_4, offsetof(Gfx, dma.addr) == 4);
GE_ABI_ASSERT(vertex16, sizeof(Vtx) == 16);
GE_ABI_ASSERT(modelnode24, sizeof(ModelNode) == 24);
/* Runtime records shared by asset expansion, player updates and rendering. */
GE_ABI_ASSERT(matrix64, sizeof(Mtx) == 64);
GE_ABI_ASSERT(float_matrix64, sizeof(Mtxf) == 64);
GE_ABI_ASSERT(render_pos64, sizeof(RenderPosView) == 64);
GE_ABI_ASSERT(model_render64, sizeof(ModelRenderData) == 64);
GE_ABI_ASSERT(model_render_gdl12, offsetof(ModelRenderData, gdl) == 12);
GE_ABI_ASSERT(model_render_matrices16, offsetof(ModelRenderData, mtxlist) == 16);
GE_ABI_ASSERT(model_header32, sizeof(ModelFileHeader) == 32);
GE_ABI_ASSERT(model_header_switches8, offsetof(ModelFileHeader, Switches) == 8);
GE_ABI_ASSERT(model_header_records20, offsetof(ModelFileHeader, numRecords) == 20);
GE_ABI_ASSERT(model_header_textures24, offsetof(ModelFileHeader, Textures) == 24);
GE_ABI_ASSERT(model_texture12, sizeof(ModelFileTextures) == 12);
GE_ABI_ASSERT(model188, sizeof(Model) == 188);
GE_ABI_ASSERT(model_object8, offsetof(Model, obj) == 8);
GE_ABI_ASSERT(model_render_pos12, offsetof(Model, render_pos) == 12);
GE_ABI_ASSERT(model_rwdata16, offsetof(Model, datas) == 16);
GE_ABI_ASSERT(model_animation32, offsetof(Model, anim) == 32);
GE_ABI_ASSERT(model_anim_stream52, offsetof(Model, unk34) == 52);
GE_ABI_ASSERT(model_play_speed164, offsetof(Model, playspeed) == 164);
GE_ABI_ASSERT(held_weapon136, sizeof(WeaponObjRecord) == 136);
GE_ABI_ASSERT(texture16, sizeof(struct tex) == 16);
GE_ABI_ASSERT(texture_data4, offsetof(struct tex, data) == 4);
GE_ABI_ASSERT(texture_width8, offsetof(struct tex, width) == 8);
GE_ABI_ASSERT(texture_pool16, sizeof(struct texpool) == 16);
GE_ABI_ASSERT(texture_pool_right12, offsetof(struct texpool, rightpos) == 12);
GE_ABI_ASSERT(model_dl20, sizeof(ModelRoData_DisplayListRecord) == 20);
GE_ABI_ASSERT(model_dl_vertices12, offsetof(ModelRoData_DisplayListRecord, Vertices) == 12);
GE_ABI_ASSERT(model_collision32, sizeof(ModelRoData_DisplayList_CollisionRecord) == 32);
GE_ABI_ASSERT(model_collision_rwindex26, offsetof(ModelRoData_DisplayList_CollisionRecord, RwDataIndex) == 26);
GE_ABI_ASSERT(model_collision_base28, offsetof(ModelRoData_DisplayList_CollisionRecord, BaseAddr) == 28);
GE_ABI_ASSERT(model_collision_rw8, sizeof(ModelRwData_DisplayList_CollisionRecord) == 8);
GE_ABI_ASSERT(model_root_rw96, sizeof(ModelRwData_HeaderRecord) == 96);
GE_ABI_ASSERT(model_head_rw8, sizeof(ModelRwData_HeadPlaceholderRecord) == 8);
GE_ABI_ASSERT(contstatus4, sizeof(OSContStatus) == 4);
GE_ABI_ASSERT(contpad6, sizeof(OSContPad) == 6);
GE_ABI_ASSERT(contsample24, sizeof(struct contsample) == 24);
GE_ABI_ASSERT(contpad_button0, offsetof(OSContPad, button) == 0);
GE_ABI_ASSERT(contpad_stick2, offsetof(OSContPad, stick_x) == 2);
GE_ABI_ASSERT(save96, sizeof(save_data) == 96);
GE_ABI_ASSERT(save_options12, offsetof(save_data, options) == 12);
GE_ABI_ASSERT(replay232, sizeof(ramromfilestructure) == 232);
GE_ABI_ASSERT(replay_seed8, offsetof(ramromfilestructure, randomizer) == 8);
GE_ABI_ASSERT(replay_controllers24, offsetof(ramromfilestructure, size_cmds) == 24);
GE_ABI_ASSERT(replay_save28, offsetof(ramromfilestructure, savefile) == 28);
GE_ABI_ASSERT(replay_ticks124, offsetof(ramromfilestructure, totaltime_ms) == 124);
GE_ABI_ASSERT(replay_flags212, offsetof(ramromfilestructure, mp_flags) == 212);
GE_ABI_ASSERT(audio_instrument_sounds16, offsetof(ALInstrument, soundArray) == 16);
GE_ABI_ASSERT(audio_alt_sounds12, offsetof(struct ALInstrumentAlt_s, soundArray) == 12);
GE_ABI_ASSERT(audio_bank_instruments12, offsetof(ALBank, instArray) == 12);
GE_ABI_ASSERT(audio_alt_instruments12, offsetof(struct ALBankAlt_s, instArray) == 12);
GE_ABI_ASSERT(queue24, sizeof(OSMesgQueue) == 24);
GE_ABI_ASSERT(task64, sizeof(OSTask) == 64);
GE_ABI_ASSERT(schedclient8, sizeof(OSScClient) == 8);
GE_ABI_ASSERT(huffman8, sizeof(struct huft) == 8);
GE_ABI_ASSERT(bg_room24, sizeof(bg_room_data) == 24);
GE_ABI_ASSERT(bg_portal8, sizeof(bg_portal_data_entry) == 8);
GE_ABI_ASSERT(bg_portal_rooms4, offsetof(bg_portal_data_entry, connectedRoom1) == 4);
GE_ABI_ASSERT(bg_portal_points16, sizeof(bg_portal_entry) == 16);
GE_ABI_ASSERT(stan_header8, sizeof(StandTile) == 8);
GE_ABI_ASSERT(stan_room3, offsetof(StandTile, room) == 3);
GE_ABI_ASSERT(stan_mid4, offsetof(StandTile, mid) == 4);
GE_ABI_ASSERT(stan_tail6, offsetof(StandTile, tail) == 6);
GE_ABI_ASSERT(stan_points8, offsetof(StandTile, points) == 8);
GE_ABI_ASSERT(stan_point8, sizeof(StandTilePoint) == 8);
GE_ABI_ASSERT(stan_file_first4, offsetof(StandFileHeader, firstTile) == 4);
GE_ABI_ASSERT(setup40, sizeof(stagesetup) == 40);
GE_ABI_ASSERT(setup_waypoint16, sizeof(waypoint) == 16);
GE_ABI_ASSERT(setup_waygroup12, sizeof(waygroup) == 12);
GE_ABI_ASSERT(setup_path8, sizeof(PathRecord) == 8);
GE_ABI_ASSERT(setup_ailist8, sizeof(AIListRecord) == 8);
GE_ABI_ASSERT(setup_pad44, sizeof(PadRecord) == 44);
GE_ABI_ASSERT(setup_boundpad68, sizeof(BoundPadRecord) == 68);
GE_ABI_ASSERT(setup_pname4, sizeof(pname) == 4);
GE_ABI_ASSERT(setup_pad_link36, offsetof(PadRecord, plink) == 36);
GE_ABI_ASSERT(setup_pad_stan40, offsetof(PadRecord, stan) == 40);
GE_ABI_ASSERT(setup_object128, sizeof(ObjectRecord) == 128);
GE_ABI_ASSERT(setup_door256, sizeof(DoorRecord) == 256);
GE_ABI_ASSERT(setup_guard28, sizeof(GuardRecord) == 28);
GE_ABI_ASSERT(setup_object_mtx24, offsetof(ObjectRecord, mtx) == 24);
GE_ABI_ASSERT(setup_object_shade120, offsetof(ObjectRecord, shadecol) == 120);
GE_ABI_ASSERT(setup_object_next124, offsetof(ObjectRecord, nextcol) == 124);
GE_ABI_ASSERT(setup_guard_data24, offsetof(GuardRecord, Data) == 24);
