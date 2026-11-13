#ifndef GE_NATIVE_FRAME_STATS_H
#define GE_NATIVE_FRAME_STATS_H

#include <stdint.h>
#include <string.h>
#include "xb_ge_fast3d.h"
#if !defined(GE_XBOX_BACKEND_STATS_ABI) || GE_XBOX_BACKEND_STATS_ABI != 2
#error Update xb_ge_fast3d.h to the RXDK-compatible stats ABI v2 before compiling ge_native_frame_stats.h.
#endif

/* Render-thread-only timing. These are CPU wall-clock intervals, including
 * driver waits, not GPU timer-query results. All addresses remain native32;
 * 64-bit integers are used only to accumulate clocks/counters without wrap. */
struct GEFramePerfWindow {
    unsigned frames,gaps,over35ms,over50ms,over100ms;
    uint64_t gapUs,workUs,presentUs,taskUs;
    uint64_t drawCalls,batches,inputTriangles,outputTriangles,uploadBytes;
    unsigned uploads,shaderCreates;
    uint64_t shaderProbes,softwareAddressTriangles,screenRejects;
    uint64_t vbLocks,smallUPDraws,vbDraws,upDraws;
    uint64_t drawApiUs,submitUs,uploadApiUs,fastSwizzles;
    unsigned maxGapUs,maxWorkUs,maxPresentUs,maxTaskUs;
    unsigned worstWorkUs,worstPresentUs,worstTaskUs;
    GEBackendFrameStats worstBackend;
};

struct GEFramePerf {
    GEFramePerfWindow window;
    uint64_t lastPresentUs;
    bool hasLastPresent;

    GEFramePerf() { Reset(); }
    void Reset() {
        ClearWindow();lastPresentUs=0;hasLastPresent=false;
    }
    void ClearWindow() { memset(&window,0,sizeof(window)); }
    static unsigned Elapsed(uint64_t begin,uint64_t end) {
        if(end<begin)return 0;
        const uint64_t difference=end-begin;
        return difference>0xFFFFFFFFull?0xFFFFFFFFu:(unsigned)difference;
    }
    static unsigned Average(uint64_t sum,unsigned count) {
        return count?(unsigned)(sum/count):0u;
    }
    void Record(uint64_t end,unsigned work,unsigned present,unsigned task,
                const GEBackendFrameStats& backend) {
        ++window.frames;
        window.workUs+=work;window.presentUs+=present;window.taskUs+=task;
        if(work>window.maxWorkUs)window.maxWorkUs=work;
        if(present>window.maxPresentUs)window.maxPresentUs=present;
        if(task>window.maxTaskUs)window.maxTaskUs=task;
        window.drawCalls+=backend.draw_calls;window.batches+=backend.gpu_batches;
        window.inputTriangles+=backend.input_triangles;
        window.outputTriangles+=backend.output_triangles;
        window.uploads+=backend.texture_uploads;
        window.uploadBytes+=backend.texture_upload_bytes;
        window.shaderCreates+=backend.shader_creates;
        window.shaderProbes+=backend.shader_probes;
        window.softwareAddressTriangles+=backend.software_address_triangles;
        window.screenRejects+=backend.screen_rejects;
        window.vbLocks+=backend.vb_locks;
        window.smallUPDraws+=backend.small_up_submissions;
        window.vbDraws+=backend.vb_submissions;
        window.upDraws+=backend.up_submissions;
        window.drawApiUs+=backend.draw_api_us;
        window.submitUs+=backend.submit_us;
        window.uploadApiUs+=backend.upload_api_us;
        window.fastSwizzles+=backend.fast_swizzles;
        if(hasLastPresent && end>=lastPresentUs) {
            const unsigned gap=Elapsed(lastPresentUs,end);
            ++window.gaps;window.gapUs+=gap;
            if(gap>35000u)++window.over35ms;
            if(gap>50000u)++window.over50ms;
            if(gap>100000u)++window.over100ms;
            if(gap>window.maxGapUs) {
                window.maxGapUs=gap;
                window.worstWorkUs=work;window.worstPresentUs=present;
                window.worstTaskUs=task;window.worstBackend=backend;
            }
        }
        lastPresentUs=end;hasLastPresent=true;
    }
};

#endif
