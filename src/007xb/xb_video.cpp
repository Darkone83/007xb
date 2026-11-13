#include "xb_video.h"
#include "xb_ge_fast3d.h"

XBVideo::XBVideo() : d3d_(0), device_(0), depth_(false), width_(640), height_(480),
    refresh_(60), widescreen_(false), mode_("480i") {}

struct XBVideoMode {
    unsigned width,height,refresh;
    DWORD flags;
    bool widescreen;
    const char* name;
};

/* A failed CreateDevice may return a partially created interface or modify
 * its parameter block. Give each retry fresh parameters and release any
 * partial device before changing the mode or depth format. */
static bool CreateVideoMode(IDirect3D8* d3d,const XBVideoMode& mode,
                            bool depth,D3DFORMAT depthFormat,
                            IDirect3DDevice8** device)
{
    D3DPRESENT_PARAMETERS p;
    ZeroMemory(&p, sizeof(p));
    p.BackBufferWidth = mode.width;
    p.BackBufferHeight = mode.height;
    p.Flags = mode.flags;
    p.FullScreen_RefreshRateInHz = mode.refresh;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.BackBufferCount = 1;
    /* 720p adds colour/depth storage and fill cost; keep MSAA disabled. */
    p.MultiSampleType = D3DMULTISAMPLE_2_SAMPLES;
	//p.MultiSampleType = D3DMULTISAMPLE_NONE;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.EnableAutoDepthStencil = depth ? TRUE : FALSE;
    p.AutoDepthStencilFormat = depthFormat;
    p.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    *device=0;
    const HRESULT hr=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,
        0,D3DCREATE_HARDWARE_VERTEXPROCESSING,&p,device);
    if(FAILED(hr) || !*device) {
        if(*device){(*device)->Release();*device=0;}
        return false;
    }
    return true;
}

bool XBVideo::Init()
{
    Shutdown();
    d3d_ = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d_) return false;
    /* The old XAPI filters HDTV flags against both the attached AV pack and
     * video standard. Follow its supported dashboard modes, as DarkDash and
     * XbDiag do; never force progressive output on an SDTV-only setup. */
    const DWORD video=XGetVideoFlags(), standard=XGetVideoStandard();
    const bool pal50=standard==XC_VIDEO_STANDARD_PAL_I &&
                     (video & XC_VIDEO_FLAGS_PAL_60Hz)==0;
    const bool wide=(video & XC_VIDEO_FLAGS_WIDESCREEN)!=0;
    const DWORD wideFlag=wide?D3DPRESENTFLAG_WIDESCREEN:0;
    XBVideoMode modes[3];unsigned count=0;
    if(video & XC_VIDEO_FLAGS_HDTV_720p) {
        XBVideoMode hd={1280,720,60,D3DPRESENTFLAG_PROGRESSIVE|D3DPRESENTFLAG_WIDESCREEN,true,"720p"};
        modes[count++]=hd;
    }
    if(video & XC_VIDEO_FLAGS_HDTV_480p) {
        XBVideoMode sd={640,480,60,D3DPRESENTFLAG_PROGRESSIVE|wideFlag,wide,"480p"};
        modes[count++]=sd;
    }
    XBVideoMode safe={640,pal50?576u:480u,pal50?50u:60u,
                     D3DPRESENTFLAG_INTERLACED|wideFlag,wide,pal50?"576i":"480i"};
    modes[count++]=safe;

    unsigned selected=count;
    for(unsigned i=0;i<count;++i) {
        if(CreateVideoMode(d3d_,modes[i],true,D3DFMT_D16,&device_) ||
           CreateVideoMode(d3d_,modes[i],true,D3DFMT_D24S8,&device_)) {
            selected=i;depth_=true;break;
        }
    }
    /* Gameplay requires depth. Try all depth-capable resolutions first;
     * retain the regional SD no-depth path only to display its startup error. */
    if(selected==count) {
        if(!CreateVideoMode(d3d_,safe,false,D3DFMT_D16,&device_)) {
            Shutdown();return false;
        }
        selected=count-1u;depth_=false;
    }
    const XBVideoMode& actual=modes[selected];
    width_=(int)actual.width;height_=(int)actual.height;
    refresh_=(int)actual.refresh;widescreen_=actual.widescreen;mode_=actual.name;
    /* Attach original GoldenEye fast3d API to the already validated 007xb device.
       DO NOT create a second Direct3D device, video subsystem or audio sink. */
    ge_xbox_d3d8_attach(device_, (unsigned)width_, (unsigned)height_);
    gfx_xbox_d3d8_api.init();
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_ZENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    return true;
}
void XBVideo::Shutdown()
{
    /* Release cached fast3d GPU objects before destroying the XDK device. */
    ge_xbox_d3d8_detach();
    if (device_) { device_->Release(); device_ = 0; }
    if (d3d_) { d3d_->Release(); d3d_ = 0; }
    depth_ = false;
    width_=640;height_=480;refresh_=60;widescreen_=false;mode_="480i";
}
void XBVideo::Begin()
{
    device_->Clear(0, 0, D3DCLEAR_TARGET | (depth_ ? D3DCLEAR_ZBUFFER : 0),
                   D3DCOLOR_XRGB(3, 5, 13), 1.0f, 0);
    device_->BeginScene();
    gfx_xbox_d3d8_api.start_frame();
}
void XBVideo::End()
{
    gfx_xbox_d3d8_api.end_frame();
    device_->EndScene();
    device_->Present(0, 0, 0, 0);
}
