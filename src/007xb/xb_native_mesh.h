#ifndef XB_NATIVE_MESH_H
#define XB_NATIVE_MESH_H

/* 007xb Phase 6A: Xbox-side owner for immutable, hardware-transformed geometry.
 *
 * This is deliberately a RESOURCE/STREAM interface, not a second N64 display-
 * list interpreter. Source vertices are built ONCE, with an FVF selected by
 * the caller. World/view/projection and render-state setup remain under the
 * caller's control, and therefore can be set per draw without rebuilding VBs.
 *
 * Only call from the D3D8 owner/graphics thread. Never allocate or release a
 * device buffer in the original game/ROM loading thread. In particular, room
 * buffers must be retired before their source data is unloaded or mutated.
 *
 * Native room support will use this owner once N64 material/lighting/fog/
 * texture/clip equivalence and room lifetime are explicitly established.
 * No implicit switch from Fast3D is performed here.
 *
 * C++98 and classic Xbox D3D8. No STL, exceptions, RTTI, dynamic heap or
 * D3DX. Explicit Reset() required while the owning D3D device is alive.
 */
#include <xtl.h>
#include <stdint.h>
#include <string.h>

class XBNativeMesh {
public:
    XBNativeMesh()
      : vb_(0), ib_(0), vertexCount_(0), indexCount_(0), stride_(0), bytes_(0),
        fvf_(0), locked_(false), ready_(false) {}

    /* Caller fills the locked GPU buffer. Size and format are immutable
     * after EndBuild(), so vertices never require per-frame CPU repacking. */
    bool BeginBuild(IDirect3DDevice8* device, unsigned vertexCount,
                    unsigned stride, DWORD fvf, void** data) {
        if(data)*data=0;
        Reset();
        if(!data || !device || !stride || vertexCount<3u ||
           (vertexCount%3u)!=0u || stride>256u ||
           vertexCount>0xffffffffu/stride)return false;
        const unsigned bytes=vertexCount*stride;
        /* 2 MiB limit per mesh; the eventual room resource manager must
         * additionally enforce an aggregate budget on retail 64 MiB Xboxes. */
        if(bytes>2u*1024u*1024u)return false;
        IDirect3DVertexBuffer8* created=0;
        HRESULT hr=device->CreateVertexBuffer(bytes,D3DUSAGE_WRITEONLY,
                        fvf,D3DPOOL_MANAGED,&created);
        if(FAILED(hr) || !created) {
            if(created){created->Release();created=0;}
            hr=device->CreateVertexBuffer(bytes,D3DUSAGE_WRITEONLY,
                           fvf,D3DPOOL_DEFAULT,&created);
        }
        if(FAILED(hr) || !created) {
            if(created)created->Release();
            return false;
        }
        vb_=created;
        BYTE* mapped=0;
        hr=vb_->Lock(0,bytes,&mapped,0);
        if(FAILED(hr) || !mapped){
            if(SUCCEEDED(hr))vb_->Unlock();
            Reset();return false;
        }
        vertexCount_=vertexCount;stride_=stride;fvf_=fvf;bytes_=bytes;
        locked_=true;
        *data=mapped;
        return true;
    }

    bool EndBuild() {
        if(!vb_ || !locked_)return false;
        locked_=false;
        const HRESULT hr=vb_->Unlock();
        if(FAILED(hr)){Reset();return false;}
        ready_=true;
        return true;
    }

    /* Original Halo Xbox draws use separately resident vertex and index
     * buffers. Only *cold* packets call this: the ordered 16-bit index stream
     * maps repeated identical input vertices to one immutable GPU vertex. */
    bool BuildIndexed(IDirect3DDevice8* device,unsigned uniqueCount,
                      unsigned stride,DWORD fvf,const void* vertices,
                      unsigned indexCount,const unsigned short* indices) {
        Reset();
        if(!device || !vertices || !indices || uniqueCount<3u ||
           uniqueCount>65535u || indexCount<3u || (indexCount%3u)!=0u ||
           stride==0u || stride>256u ||
           uniqueCount>0xffffffffu/stride ||
           indexCount>0xffffffffu/sizeof(unsigned short))return false;
        for(unsigned i=0;i<indexCount;++i)if(indices[i]>=uniqueCount)return false;
        const unsigned vertexBytes=uniqueCount*stride;
        const unsigned indexBytes=indexCount*sizeof(unsigned short);
        if(vertexBytes>2u*1024u*1024u ||
           indexBytes>2u*1024u*1024u-vertexBytes)return false;
        HRESULT hr=device->CreateVertexBuffer(vertexBytes,D3DUSAGE_WRITEONLY,
                                fvf,D3DPOOL_MANAGED,&vb_);
        if(FAILED(hr) || !vb_) {
            if(vb_){vb_->Release();vb_=0;}
            hr=device->CreateVertexBuffer(vertexBytes,D3DUSAGE_WRITEONLY,
                                fvf,D3DPOOL_DEFAULT,&vb_);
        }
        if(FAILED(hr) || !vb_) {Reset();return false;}
        BYTE* vertexMap=0;
        hr=vb_->Lock(0,vertexBytes,&vertexMap,0);
        if(FAILED(hr) || !vertexMap){if(SUCCEEDED(hr))vb_->Unlock();Reset();return false;}
        memcpy(vertexMap,vertices,vertexBytes);
        hr=vb_->Unlock();
        if(FAILED(hr)){Reset();return false;}
        hr=device->CreateIndexBuffer(indexBytes,D3DUSAGE_WRITEONLY,
                                   D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib_);
        if(FAILED(hr) || !ib_) {
            if(ib_){ib_->Release();ib_=0;}
            hr=device->CreateIndexBuffer(indexBytes,D3DUSAGE_WRITEONLY,
                                       D3DFMT_INDEX16,D3DPOOL_DEFAULT,&ib_);
        }
        if(FAILED(hr) || !ib_){Reset();return false;}
        BYTE* indexMap=0;
        hr=ib_->Lock(0,indexBytes,&indexMap,0);
        if(FAILED(hr) || !indexMap){if(SUCCEEDED(hr))ib_->Unlock();Reset();return false;}
        memcpy(indexMap,indices,indexBytes);
        hr=ib_->Unlock();
        if(FAILED(hr)){Reset();return false;}
        vertexCount_=uniqueCount;indexCount_=indexCount;
        stride_=stride;fvf_=fvf;bytes_=vertexBytes+indexBytes;
        locked_=false;ready_=true;
        return true;
    }

    /* Same D3D8 binding order as the existing Darkone83 hardware logo.
     * The caller must set transforms, texture/combiners, depth and blending.
     * This function deliberately performs no CPU-side vertex conversion. */
    bool DrawTriangles(IDirect3DDevice8* device) const {
        if(!device || !ready_ || !vb_)return false;
        device->SetStreamSource(0,vb_,stride_);
        HRESULT hr;
        if(ib_) {
            const HRESULT bind=device->SetIndices(ib_,0);
            hr=SUCCEEDED(bind)?device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                         0,vertexCount_,0,indexCount_/3u):bind;
            device->SetIndices(0,0);
        } else {
            hr=device->DrawPrimitive(D3DPT_TRIANGLELIST,0,vertexCount_/3u);
        }
        device->SetStreamSource(0,0,0);
        return SUCCEEDED(hr);
    }

    void Reset() {
        if(vb_) {
            if(locked_)vb_->Unlock();
            vb_->Release();
        }
        if(ib_)ib_->Release();
        vb_=0;ib_=0;vertexCount_=0;indexCount_=0;stride_=0;fvf_=0;bytes_=0;
        locked_=false;ready_=false;
    }

    bool Ready() const { return ready_ && vb_!=0; }
    unsigned Bytes() const { return bytes_; }
    unsigned Vertices() const { return vertexCount_; }
    DWORD Format() const { return fvf_; }
private:
    XBNativeMesh(const XBNativeMesh&);
    XBNativeMesh& operator=(const XBNativeMesh&);
    IDirect3DVertexBuffer8* vb_;
    IDirect3DIndexBuffer8* ib_;
    unsigned vertexCount_,indexCount_,stride_,bytes_;
    DWORD fvf_;
    bool locked_,ready_;
};

#endif /* XB_NATIVE_MESH_H */
