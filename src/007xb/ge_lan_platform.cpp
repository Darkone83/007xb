#include "ge_lan.h"
#include <string.h>
#if defined(XBOX) || defined(_XBOX)
#include <xtl.h>
#include <winsockx.h>
static SOCKET s_lanSocket=INVALID_SOCKET;
static CRITICAL_SECTION s_lanLock;
static int s_lanLockReady=0,s_xnetReady=0,s_wsaReady=0;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <ifaddrs.h>
static int s_lanSocket=-1;
static pthread_mutex_t s_lanLock=PTHREAD_MUTEX_INITIALIZER;
#endif

/* Addresses crossing this interface are numeric a.b.c.d = 0xAABBCCDD,
 * ports are host-order. Explicit sockaddr bytes avoid dependence on the
 * original engine's separately defined ntohl/ntohs symbols. */
static void Address(struct sockaddr_in *a,unsigned ip,unsigned port) {
    memset(a,0,sizeof(*a));a->sin_family=AF_INET;
    unsigned char *p=(unsigned char*)&a->sin_addr;
    p[0]=(unsigned char)(ip>>24);p[1]=(unsigned char)(ip>>16);
    p[2]=(unsigned char)(ip>>8);p[3]=(unsigned char)ip;
    p=(unsigned char*)&a->sin_port;p[0]=(unsigned char)(port>>8);p[1]=(unsigned char)port;
}
static unsigned ReadIP(const struct sockaddr_in *a) {
    const unsigned char *p=(const unsigned char*)&a->sin_addr;
    return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];
}
extern "C" void geLanPlatformLock(void) {
#if defined(XBOX) || defined(_XBOX)
    if(s_lanLockReady)EnterCriticalSection(&s_lanLock);
#else
    pthread_mutex_lock(&s_lanLock);
#endif
}
extern "C" void geLanPlatformUnlock(void) {
#if defined(XBOX) || defined(_XBOX)
    if(s_lanLockReady)LeaveCriticalSection(&s_lanLock);
#else
    pthread_mutex_unlock(&s_lanLock);
#endif
}
extern "C" unsigned geLanPlatformNow(void) {
#if defined(XBOX) || defined(_XBOX)
    return (unsigned)GetTickCount();
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned)((unsigned long long)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u);
#endif
}
/* Session identity is deliberately not based on millisecond uptime alone:
 * two consoles booted together can share that value. Mix device identity
 * and a high-resolution counter; this is an epoch nonce, not a secret. */
extern "C" unsigned geLanPlatformEntropy(void) {
    unsigned value=2166136261u;
#if defined(XBOX) || defined(_XBOX)
    XNADDR address;memset(&address,0,sizeof(address));
    if(s_xnetReady)XNetGetTitleXnAddr(&address);
    for(unsigned i=0;i<sizeof(address.abEnet);++i)value=(value^address.abEnet[i])*16777619u;
    LARGE_INTEGER counter;QueryPerformanceCounter(&counter);
    value^=(unsigned)counter.LowPart;value^=(unsigned)counter.HighPart;
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    value^=(unsigned)t.tv_nsec;value^=(unsigned)getpid()*2654435761u;
#endif
    return value^geLanPlatformNow();
}
extern "C" void geLanPlatformClose(void) {
#if defined(XBOX) || defined(_XBOX)
    if(s_lanSocket!=INVALID_SOCKET){closesocket(s_lanSocket);s_lanSocket=INVALID_SOCKET;}
    if(s_wsaReady){WSACleanup();s_wsaReady=0;}
    if(s_xnetReady){XNetCleanup();s_xnetReady=0;}
#else
    if(s_lanSocket>=0){close(s_lanSocket);s_lanSocket=-1;}
#endif
}
extern "C" int geLanPlatformOpen(unsigned port) {
#if defined(XBOX) || defined(_XBOX)
    if(!s_lanLockReady){InitializeCriticalSection(&s_lanLock);s_lanLockReady=1;}
    XNetStartupParams params;memset(&params,0,sizeof(params));
    params.cfgSizeOfStruct=(BYTE)sizeof(params);
    params.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    params.cfgSockDefaultRecvBufsizeInK=16;params.cfgSockDefaultSendBufsizeInK=16;
    if(XNetStartup(&params)!=0)return 0;
    s_xnetReady=1;
    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa)!=0){geLanPlatformClose();return 0;}s_wsaReady=1;
    s_lanSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s_lanSocket==INVALID_SOCKET){geLanPlatformClose();return 0;}
    unsigned long nonblocking=1;
    if(ioctlsocket(s_lanSocket,FIONBIO,&nonblocking)!=0){geLanPlatformClose();return 0;}
#else
    s_lanSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s_lanSocket<0)return 0;
    int flags=fcntl(s_lanSocket,F_GETFL,0);
    if(flags<0||fcntl(s_lanSocket,F_SETFL,flags|O_NONBLOCK)<0){geLanPlatformClose();return 0;}
#endif
    int yes=1;struct sockaddr_in addr;Address(&addr,0,port);
    if(setsockopt(s_lanSocket,SOL_SOCKET,SO_BROADCAST,(const char*)&yes,sizeof(yes))!=0 ||
       bind(s_lanSocket,(struct sockaddr*)&addr,sizeof(addr))!=0){geLanPlatformClose();return 0;}
    return 1;
}
extern "C" int geLanPlatformSend(unsigned ip,unsigned port,const void *data,unsigned bytes) {
    struct sockaddr_in addr;Address(&addr,ip,port);
    int n=sendto(s_lanSocket,(const char*)data,(int)bytes,0,(struct sockaddr*)&addr,sizeof(addr));
    return n==(int)bytes;
}
/* 0 means queue drained; -1 is a real socket error. Oversized datagrams are
 * discarded instead of treating truncation as a parseable smaller packet. */
extern "C" int geLanPlatformReceive(unsigned *ip,unsigned *port,void *data,unsigned capacity) {
    struct sockaddr_in addr;
#if defined(XBOX) || defined(_XBOX)
    int len=sizeof(addr);
    int n=recvfrom(s_lanSocket,(char*)data,(int)capacity,0,(struct sockaddr*)&addr,&len);
    if(n==SOCKET_ERROR){int e=WSAGetLastError();if(e==WSAEWOULDBLOCK||e==WSAEMSGSIZE)return 0;return -1;}
#else
    socklen_t len=sizeof(addr);
    int n=(int)recvfrom(s_lanSocket,(char*)data,capacity,MSG_TRUNC,(struct sockaddr*)&addr,&len);
    if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return 0;return -1;}
    if(n>(int)capacity)return 0;
#endif
    if(len!=(int)sizeof(addr)||addr.sin_family!=AF_INET)return 0;
    *ip=ReadIP(&addr);const unsigned char *p=(const unsigned char*)&addr.sin_port;
    *port=((unsigned)p[0]<<8)|p[1];return n;
}
extern "C" unsigned geLanPlatformParseIP(const char *text) {
    unsigned v[4]={0,0,0,0};int part=0,digits=0;if(!text)return 0;
    for(;*text;++text){
        if(*text>='0'&&*text<='9'){if(++digits>3)return 0;v[part]=v[part]*10u+(unsigned)(*text-'0');if(v[part]>255)return 0;}
        else if(*text=='.'&&part<3&&digits){++part;digits=0;}
        else return 0;
    }
    if(part!=3||!digits)return 0;
    return (v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];
}
extern "C" void geLanPlatformFormatIP(unsigned ip,char *out) {
    /* Four bounded octets need at most 15 characters plus NUL. All callers
     * provide 24 bytes. Keep this path independent of desktop stdio/UCRT. */
    for(unsigned part=0;part<4u;++part){
        const unsigned octet=(ip>>(24u-part*8u))&255u;
        if(octet>=100u)*out++=(char)('0'+octet/100u);
        if(octet>=10u)*out++=(char)('0'+(octet/10u)%10u);
        *out++=(char)('0'+octet%10u);
        if(part!=3u)*out++='.';
    }
    *out=0;
}
extern "C" void geLanPlatformLocalAddress(char *out) {
    unsigned ip=0;
#if defined(XBOX) || defined(_XBOX)
    if(s_xnetReady){XNADDR addr;memset(&addr,0,sizeof(addr));XNetGetTitleXnAddr(&addr);
        const unsigned char *p=(const unsigned char*)&addr.ina;
        ip=((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];}
#else
    struct ifaddrs *first=0;
    if(getifaddrs(&first)==0){for(struct ifaddrs *a=first;a;a=a->ifa_next){
        if(a->ifa_addr&&a->ifa_addr->sa_family==AF_INET){unsigned n=ReadIP((const struct sockaddr_in*)a->ifa_addr);
            if(n&&(n>>24)!=127){ip=n;break;}}}freeifaddrs(first);}
#endif
    geLanPlatformFormatIP(ip,out);
}
