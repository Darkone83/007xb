#include "ge_lan.h"
#include <string.h>

extern "C" {
int geLanPlatformOpen(unsigned);void geLanPlatformClose(void);
void geLanPlatformLock(void);void geLanPlatformUnlock(void);
unsigned geLanPlatformNow(void);
unsigned geLanPlatformEntropy(void);
int geLanPlatformSend(unsigned,unsigned,const void*,unsigned);
int geLanPlatformReceive(unsigned*,unsigned*,void*,unsigned);
unsigned geLanPlatformParseIP(const char*);
void geLanPlatformFormatIP(unsigned,char*);void geLanPlatformLocalAddress(char*);
}
namespace {
/* Fixed storage: no allocation, callbacks, waits, or engine mutation under
 * this lock. Datagrams are bounded to 768 bytes and receive work to 64 packets
 * per pump. Source slots and session epochs are checked before any state edit. */
enum { MAGIC=0x47454c31u, HEADER=32, PACKET=768, RING=64, INPUT_LEAD=2,
       WINDOW=8, HEARTBEAT_MS=250, RETRY_MS=50, PEER_TIMEOUT_MS=5000,
       STALL_TIMEOUT_MS=6000, LOAD_TIMEOUT_MS=45000, END_RETRY_MS=100,
       END_TIMEOUT_MS=3000, HOST_EXPIRE_MS=3000 };
enum Type { DISCOVER=1, ADVERTISE, JOIN, STATE, CLIENT_STATUS, START,
            LOADED, GO, INPUT, BUNDLE, PROGRESS, QUIT, END, END_ACK, REJECT };
struct Guard {Guard(){geLanPlatformLock();}~Guard(){geLanPlatformUnlock();}};
struct Endpoint {unsigned ip,port;};
struct Peer {
    Endpoint address;unsigned epoch,last_seen,last_progress,completed;
    unsigned options_seq,ready_seq;char name[GE_LAN_NAME_BYTES];
};
struct Found {Endpoint address;unsigned session,epoch,seen;GE_LanHostInfo info;};
struct InputRow {unsigned tick,mask;GE_LanPlayerInput input[4];};
struct BundleRow {unsigned tick,valid;GE_LanTick bundle;};
struct HashRow {unsigned tick,mask;unsigned hash[4];};
struct Context {
    int initialized,state,role,slot,round_active,start_pending,load_notified;
    int end_reason,clear_pending,tick_inflight;
    unsigned port,content,epoch,session,sequence,now,state_since,last_send;
    unsigned last_retry,last_discovery,last_end,ready_mask,loaded_mask,end_ack;
    unsigned end_since,next_local,next_bundle,next_consume,verified,next_due,pace_rem;
    unsigned local_options_seq,local_ready_seq,local_ready_revision,local_ready;
    unsigned local_bad_since;
    GE_LanMatchConfig config;
    GE_LanPlayerOptions local_options;
    GE_LanPlayerInput latest;
    Peer peer[4];Found found[GE_LAN_MAX_HOSTS];unsigned found_count;
    InputRow input[RING];BundleRow bundle[RING];HashRow hashes[RING];
    Endpoint discovery;
    char host_name[GE_LAN_NAME_BYTES],message[96];
};
static Context s;
static unsigned s_nonce=0x91b70001u;
typedef char AssertUnsigned32[sizeof(unsigned)==4?1:-1];
typedef char AssertBoundedMemory[sizeof(Context)<96u*1024u?1:-1];
static void Text(char *to,unsigned cap,const char *from) {
    unsigned n=0;if(from)while(n+1<cap&&from[n]){to[n]=from[n];++n;}to[n]=0;
}
static unsigned All(void){return (1u<<s.config.player_count)-1u;}
static int Due(unsigned t){return (int)(s.now-t)>=0;}
static int Same(const Endpoint&a,const Endpoint&b){return a.ip==b.ip&&a.port==b.port;}
static int Terminal(void){return s.state==GE_LAN_ENDING||s.state==GE_LAN_ENDED;}
static unsigned NewNonce(void){s_nonce+=0x9e3779b9u;unsigned x=geLanPlatformEntropy()^s_nonce^(s.port<<16);x^=x>>16;x*=0x7feb352du;x^=x>>15;return x?x:1u;}
static void Message(const char *m){Text(s.message,sizeof(s.message),m);}
static int OptionsOK(const GE_LanPlayerOptions&o) {
    return o.character>=0&&o.character<64&&o.handicap>=0&&o.handicap<=10&&
        ((o.control_style>=0&&o.control_style<=3)||o.control_style==9)&&o.team>=0&&o.team<=3;
}
static int SameOptions(const GE_LanPlayerOptions&a,const GE_LanPlayerOptions&b) {
    return a.character==b.character&&a.handicap==b.handicap&&a.control_style==b.control_style&&a.team==b.team;
}
static int ConfigOK(const GE_LanMatchConfig&c,int starting) {
    static const int stages[12]={-1,38,31,50,48,45,46,34,27,24,39,32};
    static const unsigned capacity[12]={4,4,4,4,4,4,4,4,3,3,3,2};
    if(c.build_id!=GE_LAN_BUILD_ID||c.player_count<1||c.player_count>4||
       c.mp_stage_index<0||c.mp_stage_index>=12||c.scenario<0||c.scenario>7||
       c.weapon_set<0||c.weapon_set>13||c.game_length<0||c.game_length>7||
       c.aim_option<0||c.aim_option>3||c.difficulty< -1||c.difficulty>3)return 0;
    if(starting){
        if(c.player_count<2||c.player_count>capacity[c.mp_stage_index])return 0;
        if((c.scenario==5||c.scenario==6)&&c.player_count!=4)return 0;
        if(c.scenario==7&&c.player_count!=3)return 0;
        int matched=0;
        for(int i=1;i<12;++i)if(c.stage_id==stages[i]&&c.player_count<=capacity[i])matched=1;
        if(!matched||(c.mp_stage_index&&c.stage_id!=stages[c.mp_stage_index]))return 0;
    } else if(c.stage_id!= -1) {
        int matched=0;for(int i=1;i<12;++i)if(c.stage_id==stages[i])matched=1;
        if(!matched)return 0;
    }
    for(unsigned i=0;i<4;++i)if(!OptionsOK(c.player[i]))return 0;
    return 1;
}
struct Writer {
    unsigned char bytes[PACKET];unsigned pos;int ok;
    Writer():pos(HEADER),ok(1){memset(bytes,0,HEADER);}
    void u8(unsigned v){if(pos>=PACKET){ok=0;return;}bytes[pos++]=(unsigned char)v;}
    void u16(unsigned v){u8(v>>8);u8(v);}
    void u32(unsigned v){u16(v>>16);u16(v);}
    void name(const char *p){unsigned i=0;for(;i<GE_LAN_NAME_BYTES;++i){unsigned v=p&&p[i]?(unsigned char)p[i]:0;u8(v);if(!v){while(++i<GE_LAN_NAME_BYTES)u8(0);break;}}}
};
struct Reader {
    const unsigned char *bytes;unsigned pos,end;int ok;
    Reader(const unsigned char*p,unsigned n):bytes(p),pos(HEADER),end(n),ok(1){}
    unsigned u8(){if(pos>=end){ok=0;return 0;}return bytes[pos++];}
    unsigned u16(){unsigned a=u8();return (a<<8)|u8();}
    unsigned u32(){unsigned a=u16();return (a<<16)|u16();}
    void name(char *p){for(unsigned i=0;i<GE_LAN_NAME_BYTES;++i){unsigned c=u8();p[i]=(c>=32&&c<127)?(char)c:0;}p[GE_LAN_NAME_BYTES-1]=0;}
    int done()const{return ok&&pos==end;}
};
static void Put16(unsigned char*p,unsigned v){p[0]=(unsigned char)(v>>8);p[1]=(unsigned char)v;}
static void Put32(unsigned char*p,unsigned v){Put16(p,v>>16);Put16(p+2,v);}
static unsigned Get16(const unsigned char*p){return ((unsigned)p[0]<<8)|p[1];}
static unsigned Get32(const unsigned char*p){return (Get16(p)<<16)|Get16(p+2);}
static void Send(const Endpoint&to,unsigned type,Writer&w) {
    if(!w.ok||!s.initialized||!to.port)return;
    Put32(w.bytes,MAGIC);Put16(w.bytes+4,GE_LAN_PROTOCOL_VERSION);
    w.bytes[6]=(unsigned char)type;w.bytes[7]=(unsigned char)(s.slot<0?255:s.slot);
    Put32(w.bytes+8,s.session);Put32(w.bytes+12,s.epoch);Put32(w.bytes+16,++s.sequence);
    Put16(w.bytes+20,w.pos-HEADER);Put32(w.bytes+24,GE_LAN_BUILD_ID);Put32(w.bytes+28,s.content);
    geLanPlatformSend(to.ip,to.port,w.bytes,w.pos);
}
static void WriteOptions(Writer&w,const GE_LanPlayerOptions&o){w.u32(o.character);w.u32(o.handicap);w.u32(o.control_style);w.u32(o.team);}
static void ReadOptions(Reader&r,GE_LanPlayerOptions&o){o.character=(int)r.u32();o.handicap=(int)r.u32();o.control_style=(int)r.u32();o.team=(int)r.u32();}
static void WriteConfig(Writer&w,const GE_LanMatchConfig&c) {
    w.u32(c.revision);w.u32(c.build_id);w.u32(c.player_count);
    w.u32(c.stage_id);w.u32(c.mp_stage_index);w.u32(c.scenario);w.u32(c.weapon_set);
    w.u32(c.game_length);w.u32(c.aim_option);w.u32(c.difficulty);
    w.u32(c.rng_hi);w.u32(c.rng_lo);w.u32(c.chr_rng_hi);w.u32(c.chr_rng_lo);
    for(unsigned i=0;i<4;++i)WriteOptions(w,c.player[i]);
}
static void ReadConfig(Reader&r,GE_LanMatchConfig&c) {
    c.revision=r.u32();c.build_id=r.u32();c.player_count=r.u32();
    c.stage_id=(int)r.u32();c.mp_stage_index=(int)r.u32();c.scenario=(int)r.u32();c.weapon_set=(int)r.u32();
    c.game_length=(int)r.u32();c.aim_option=(int)r.u32();c.difficulty=(int)r.u32();
    c.rng_hi=r.u32();c.rng_lo=r.u32();c.chr_rng_hi=r.u32();c.chr_rng_lo=r.u32();
    for(unsigned i=0;i<4;++i)ReadOptions(r,c.player[i]);
}
static void WriteInput(Writer&w,const GE_LanPlayerInput&i) {
    w.u16(i.buttons);w.u16(i.pressed);w.u8((unsigned char)i.stick_x);w.u8((unsigned char)i.stick_y);
    w.u16(i.native_buttons);w.u16(i.native_pressed);w.u8((unsigned char)i.move_x);w.u8((unsigned char)i.move_y);
    w.u8((unsigned char)i.look_x);w.u8((unsigned char)i.look_y);w.u8(i.connected);w.u8(i.native_connected);
}
static int ReadInput(Reader&r,GE_LanPlayerInput&i) {
    i.buttons=(unsigned short)r.u16();i.pressed=(unsigned short)r.u16();i.stick_x=(signed char)r.u8();i.stick_y=(signed char)r.u8();
    i.native_buttons=(unsigned short)r.u16();i.native_pressed=(unsigned short)r.u16();i.move_x=(signed char)r.u8();i.move_y=(signed char)r.u8();
    i.look_x=(signed char)r.u8();i.look_y=(signed char)r.u8();i.connected=(unsigned char)r.u8();i.native_connected=(unsigned char)r.u8();
    return r.ok&&i.connected<=1&&i.native_connected<=1&&i.stick_x>=-80&&i.stick_x<=80&&i.stick_y>=-80&&i.stick_y<=80&&
        i.move_x>=-80&&i.move_x<=80&&i.move_y>=-80&&i.move_y<=80&&i.look_x>=-80&&i.look_x<=80&&i.look_y>=-80&&i.look_y<=80;
}
static int SameInput(const GE_LanPlayerInput&a,const GE_LanPlayerInput&b) {
    return a.buttons==b.buttons&&a.pressed==b.pressed&&a.stick_x==b.stick_x&&a.stick_y==b.stick_y&&
      a.native_buttons==b.native_buttons&&a.native_pressed==b.native_pressed&&a.move_x==b.move_x&&a.move_y==b.move_y&&
      a.look_x==b.look_x&&a.look_y==b.look_y&&a.connected==b.connected&&a.native_connected==b.native_connected;
}
static void ResetSession(void) {
    unsigned port=s.port,content=s.content;int initialized=s.initialized;Endpoint discovery=s.discovery;
    GE_LanPlayerInput latest=s.latest;
    memset(&s,0,sizeof(s));s.initialized=initialized;s.port=port;s.content=content;s.discovery=discovery;s.latest=latest;
    s.state=GE_LAN_IDLE;s.slot=-1;s.epoch=NewNonce();s.now=geLanPlatformNow();s.state_since=s.now;
    s.config.build_id=GE_LAN_BUILD_ID;s.config.stage_id=38;s.config.mp_stage_index=1;
    s.config.game_length=2;s.config.weapon_set=1;s.config.aim_option=3;s.config.difficulty=-1;
    s.config.revision=1;
    for(unsigned i=0;i<4;++i){s.config.player[i].character=(int)i;s.config.player[i].handicap=5;s.config.player[i].control_style=9;}
    Message("Choose Host or Join LAN");
}
static void BeginEnd(int reason) {
    if(reason<GE_LAN_END_QUIT||reason>GE_LAN_END_CONTROLLER)reason=GE_LAN_END_QUIT;
    if(Terminal())return;
    s.end_reason=reason;s.state=GE_LAN_ENDING;s.end_since=s.now;s.last_end=s.now-END_RETRY_MS;
    s.end_ack=s.slot>=0?(1u<<s.slot):0;s.start_pending=0;Message(GE_LanEndReasonText(reason));
}
static void SendState(unsigned slot,unsigned type) {
    Writer w;w.u8(s.state);w.u8(s.ready_mask);w.u8(s.loaded_mask);w.u8(0);
    WriteConfig(w,s.config);
    for(unsigned i=0;i<s.config.player_count;++i){w.u32(s.peer[i].epoch);w.u32(s.peer[i].options_seq);w.u32(s.peer[i].ready_seq);w.name(s.peer[i].name);}
    Send(s.peer[slot].address,type,w);
}
static void SendAllState(unsigned type){for(unsigned i=1;i<s.config.player_count;++i)SendState(i,type);}
static void StartRunning(void) {
    s.state=GE_LAN_RUNNING;s.state_since=s.now;s.next_due=s.now+100;s.pace_rem=0;
    s.next_local=s.next_bundle=s.next_consume=s.verified=0;s.tick_inflight=0;
    s.latest.pressed=0;s.latest.native_pressed=0;s.local_bad_since=0;
    memset(s.input,0,sizeof(s.input));memset(s.bundle,0,sizeof(s.bundle));memset(s.hashes,0,sizeof(s.hashes));
    for(unsigned i=0;i<s.config.player_count;++i){s.peer[i].last_progress=s.now;s.peer[i].completed=0;}
    Message("LAN match running");
}
static InputRow& Inputs(unsigned tick) {
    InputRow&r=s.input[tick%RING];if(!r.mask||r.tick!=tick){memset(&r,0,sizeof(r));r.tick=tick;}return r;
}
static HashRow& Hashes(unsigned tick) {
    HashRow&r=s.hashes[tick%RING];if(!r.mask||r.tick!=tick){memset(&r,0,sizeof(r));r.tick=tick;}return r;
}
static void Verify(void) {
    while(s.verified<s.next_consume){HashRow&r=Hashes(s.verified);if(r.mask!=All())break;
        for(unsigned i=1;i<s.config.player_count;++i)if(r.hash[i]!=r.hash[0]){BeginEnd(GE_LAN_END_DESYNC);return;}
        ++s.verified;
    }
}
static void CaptureInputs(void) {
    if(s.slot<0)return;
    while(s.next_local<=s.next_consume+INPUT_LEAD&&s.next_local<s.verified+WINDOW){
        InputRow&r=Inputs(s.next_local);r.input[s.slot]=s.latest;r.mask|=1u<<s.slot;
        s.latest.pressed=s.latest.native_pressed=0;++s.next_local;
    }
}
static void SendInputs(void) {
    if(!s.next_local)return;
    unsigned first=s.next_consume>2?s.next_consume-2:0;
    if(first>=s.next_local)return;
    unsigned count=s.next_local-first;if(count>8)count=8;
    Writer w;w.u32(first);w.u8(count);
    for(unsigned t=first;t<first+count;++t)WriteInput(w,Inputs(t).input[s.slot]);
    Send(s.peer[0].address,INPUT,w);
}
static void SendProgress(void) {
    Writer w;w.u32(s.next_consume);w.u32(s.verified);
    unsigned count=s.next_consume-s.verified;if(count>WINDOW)count=WINDOW;w.u8(count);
    for(unsigned t=s.verified;t<s.verified+count;++t){HashRow&r=Hashes(t);w.u32(r.hash[s.slot]);}
    Send(s.peer[0].address,PROGRESS,w);
}
static void SendBundles(unsigned slot) {
    Writer w;w.u32(s.verified);unsigned first=s.peer[slot].completed;
    if(first>s.next_bundle)return;
    unsigned count=s.next_bundle-first;if(count>8)count=8;
    w.u32(first);w.u8(count);
    for(unsigned t=first;t<first+count;++t){BundleRow&r=s.bundle[t%RING];if(!r.valid||r.tick!=t)return;
        for(unsigned p=0;p<s.config.player_count;++p)WriteInput(w,r.bundle.input[p]);}
    Send(s.peer[slot].address,BUNDLE,w);
}
static void SendClientStatus(void) {
    Writer w;w.u32(s.local_options_seq);w.u32(s.local_ready_seq);w.u32(s.local_ready_revision);w.u8(s.local_ready);
    WriteOptions(w,s.local_options);Send(s.peer[0].address,CLIENT_STATUS,w);
}
static void SendLoaded(void){Writer w;w.u32(s.config.revision);w.u8(s.load_notified?1:0);Send(s.peer[0].address,LOADED,w);}
static void SendEnd(void) {
    if(s.role==GE_LAN_ROLE_HOST){for(unsigned i=1;i<s.config.player_count;++i){
        Writer w;w.u8(s.end_reason);w.u32(s.next_consume);Send(s.peer[i].address,END,w);}}
    else if(s.role==GE_LAN_ROLE_CLIENT){Writer w;w.u8(s.end_reason);Send(s.peer[0].address,QUIT,w);}
}
static void Reject(const Endpoint&from,unsigned reason){Writer w;w.u8(reason);Send(from,REJECT,w);}
static void ReceiveState(Reader&r,unsigned packetType,const Endpoint&from,unsigned hostEpoch,unsigned session) {
    unsigned state=r.u8(),ready=r.u8(),loaded=r.u8();r.u8();GE_LanMatchConfig c;ReadConfig(r,c);
    if(!r.ok||c.player_count>4||!ConfigOK(c,packetType==START||packetType==GO))return;
    Peer incoming[4];memset(incoming,0,sizeof(incoming));int slot=-1;
    for(unsigned i=0;i<c.player_count;++i){incoming[i].epoch=r.u32();incoming[i].options_seq=r.u32();incoming[i].ready_seq=r.u32();r.name(incoming[i].name);if(incoming[i].epoch==s.epoch)slot=(int)i;}
    if(!r.done()||slot<1||incoming[0].epoch!=hostEpoch||ready>=(1u<<c.player_count)||loaded>=(1u<<c.player_count))return;
    if(Terminal())return;
    if(s.state!=GE_LAN_JOINING&&(s.slot!=slot||c.revision<s.config.revision))return;
    if(s.round_active){
        if(c.revision!=s.config.revision)return;
        if(packetType==GO&&s.state==GE_LAN_LOADING&&s.load_notified)StartRunning();
        s.loaded_mask=loaded;s.peer[0].last_seen=s.now;return;
    }
    if(packetType==GO)return; /* A START must be loaded before GO is accepted. */
    if(packetType==START&&state!=GE_LAN_LOADING)return;
    if(packetType==STATE&&state!=GE_LAN_LOBBY)return;
    int joined=s.state==GE_LAN_JOINING;unsigned oldRevision=s.config.revision;
    s.config=c;s.session=session;s.slot=slot;s.role=GE_LAN_ROLE_CLIENT;s.ready_mask=ready;s.loaded_mask=loaded;
    for(unsigned i=0;i<c.player_count;++i){s.peer[i].epoch=incoming[i].epoch;s.peer[i].options_seq=incoming[i].options_seq;
        s.peer[i].ready_seq=incoming[i].ready_seq;Text(s.peer[i].name,GE_LAN_NAME_BYTES,incoming[i].name);}
    s.peer[0].address=from;s.peer[0].last_seen=s.now;
    if(joined){s.local_options=c.player[slot];s.local_options_seq=s.peer[slot].options_seq;s.local_ready_seq=s.peer[slot].ready_seq;s.local_ready=0;s.local_ready_revision=c.revision;}
    else if(s.peer[slot].options_seq>=s.local_options_seq)s.local_options=c.player[slot];
    if(s.peer[slot].ready_seq>=s.local_ready_seq){s.local_ready=(ready>>slot)&1u;s.local_ready_revision=c.revision;}
    else if(oldRevision!=c.revision){s.local_ready=0;s.local_ready_revision=c.revision;++s.local_ready_seq;}
    if(packetType==START){s.state=GE_LAN_LOADING;s.round_active=1;s.start_pending=1;s.load_notified=0;s.state_since=s.now;s.latest.pressed=s.latest.native_pressed=0;Message("Loading agreed LAN match");}
    else{s.state=GE_LAN_LOBBY;Message("LAN lobby: select Ready");}
}
static void Handle(const unsigned char *packet,unsigned bytes,const Endpoint&from) {
    if(bytes<HEADER||Get32(packet)!=MAGIC||packet[22]||packet[23]||Get16(packet+20)!=bytes-HEADER)return;
    unsigned type=packet[6],slot=packet[7],session=Get32(packet+8),epoch=Get32(packet+12);
    if(!epoch||epoch==s.epoch||type<DISCOVER||type>REJECT)return;
    Reader r(packet,bytes);
    int compatible=Get16(packet+4)==GE_LAN_PROTOCOL_VERSION&&Get32(packet+24)==GE_LAN_BUILD_ID&&Get32(packet+28)==s.content;
    if(!compatible){
        if(type==DISCOVER&&r.done()&&s.role==GE_LAN_ROLE_HOST&&s.state==GE_LAN_LOBBY){
            Writer w;w.name(s.host_name);w.u8(s.config.player_count);Send(from,ADVERTISE,w);
        }
        if(type==JOIN&&s.role==GE_LAN_ROLE_HOST&&s.state==GE_LAN_LOBBY)Reject(from,1);
        if(type==ADVERTISE&&s.state==GE_LAN_BROWSING)Message("A LAN host has a different build or archive");
        if(type==REJECT&&s.state==GE_LAN_JOINING&&Same(s.peer[0].address,from)&&s.peer[0].epoch==epoch){
            unsigned why=r.u8();if(r.done()&&why==1){s.state=GE_LAN_ERROR;Message("Host build or archive differs");}
        }
        return;
    }
    if(type==DISCOVER){if(r.done()&&s.role==GE_LAN_ROLE_HOST&&s.state==GE_LAN_LOBBY){Writer w;w.name(s.host_name);w.u8(s.config.player_count);Send(from,ADVERTISE,w);}return;}
    if(type==ADVERTISE&&s.state==GE_LAN_BROWSING){
        GE_LanHostInfo info;memset(&info,0,sizeof(info));r.name(info.name);info.player_count=r.u8();
        if(!r.done()||!session||info.player_count<1||info.player_count>=4)return;
        unsigned idx=s.found_count;for(unsigned i=0;i<s.found_count;++i)if(Same(s.found[i].address,from)){idx=i;break;}
        if(idx>=GE_LAN_MAX_HOSTS)return;
        if(idx==s.found_count)++s.found_count;
        Found&f=s.found[idx];f.address=from;f.epoch=epoch;f.session=session;f.seen=s.now;f.info=info;f.info.port=from.port;geLanPlatformFormatIP(from.ip,f.info.address);Message("Select a LAN host");return;
    }
    if(type==REJECT&&s.state==GE_LAN_JOINING&&Same(s.peer[0].address,from)){unsigned why=r.u8();if(!r.done())return;s.state=GE_LAN_ERROR;Message(why==1?"Host build or archive differs":"Host cannot accept another player");return;}
    if(session!=s.session||!session)return;
    if(type==JOIN&&s.role==GE_LAN_ROLE_HOST){
        char name[GE_LAN_NAME_BYTES];r.name(name);if(!r.done()||slot!=255)return;
        for(unsigned i=1;i<s.config.player_count;++i)if(s.peer[i].epoch==epoch&&Same(s.peer[i].address,from)){s.peer[i].last_seen=s.now;SendState(i,s.state==GE_LAN_LOBBY?STATE:START);return;}
        if(s.state!=GE_LAN_LOBBY||s.config.player_count>=4){Reject(from,2);return;}
        unsigned i=s.config.player_count++;Peer&p=s.peer[i];p.address=from;p.epoch=epoch;p.last_seen=p.last_progress=s.now;
        Text(p.name,GE_LAN_NAME_BYTES,name);
        if(!p.name[0]){
            /* Slots are bounded to four. Avoid the desktop stdio formatter:
             * its UCRT helpers are not supplied by the classic Xbox CRT. */
            static const char * const names[GE_LAN_MAX_PLAYERS]={"Player 1","Player 2","Player 3","Player 4"};
            Text(p.name,GE_LAN_NAME_BYTES,names[i]);
        }
        s.config.player[i].character=(int)i;s.config.player[i].handicap=5;s.config.player[i].control_style=9;s.config.player[i].team=0;
        ++s.config.revision;s.ready_mask=0;SendAllState(STATE);return;
    }
    if(s.role==GE_LAN_ROLE_CLIENT){
        if(!Same(s.peer[0].address,from)||s.peer[0].epoch!=epoch||slot!=0)return;
        if(type==STATE||type==START||type==GO){ReceiveState(r,type,from,epoch,session);return;}
        if(type==END){unsigned reason=r.u8();r.u32();if(!r.done()||reason<1||reason>GE_LAN_END_CONTROLLER)return;
            if(!Terminal())BeginEnd((int)reason);
            s.peer[0].last_seen=s.now;Writer w;w.u8(reason);Send(from,END_ACK,w);return;}
        if(type!=BUNDLE||s.state!=GE_LAN_RUNNING)return;
        unsigned verified=r.u32(),first=r.u32(),count=r.u8();
        if(count>8||first>s.next_consume+WINDOW||verified>s.next_consume||first+count<first||first+count>s.next_consume+WINDOW)return;
        GE_LanTick incoming[8];memset(incoming,0,sizeof(incoming));
        for(unsigned n=0;n<count;++n){incoming[n].tick=first+n;incoming[n].delta=GE_LAN_TICK_DELTA;incoming[n].player_count=s.config.player_count;
            for(unsigned p=0;p<s.config.player_count;++p)if(!ReadInput(r,incoming[n].input[p]))return;}
        if(!r.done())return;
        s.peer[0].last_seen=s.now;if(verified>s.verified)s.verified=verified;
        for(unsigned n=0;n<count;++n){unsigned t=first+n;if(t<s.next_consume)continue;BundleRow&b=s.bundle[t%RING];
            if(b.valid&&b.tick==t){for(unsigned p=0;p<s.config.player_count;++p)if(!SameInput(b.bundle.input[p],incoming[n].input[p])){BeginEnd(GE_LAN_END_PROTOCOL);return;}}
            else{b.tick=t;b.valid=1;b.bundle=incoming[n];}}
        return;
    }
    if(s.role!=GE_LAN_ROLE_HOST||slot<1||slot>=s.config.player_count||!Same(s.peer[slot].address,from)||s.peer[slot].epoch!=epoch)return;
    Peer&p=s.peer[slot];
    if(type==QUIT){unsigned reason=r.u8();if(r.done()&&reason>=1&&reason<=GE_LAN_END_CONTROLLER){p.last_seen=s.now;BeginEnd((int)reason);}return;}
    if(type==END_ACK){unsigned reason=r.u8();if(r.done()&&Terminal()&&reason==(unsigned)s.end_reason){s.end_ack|=1u<<slot;p.last_seen=s.now;}return;}
    if(type==CLIENT_STATUS&&s.state==GE_LAN_LOBBY){
        unsigned optionsSeq=r.u32(),readySeq=r.u32(),revision=r.u32(),ready=r.u8();GE_LanPlayerOptions options;ReadOptions(r,options);
        if(!r.done()||ready>1||!OptionsOK(options))return;
        p.last_seen=s.now;
        if(optionsSeq>p.options_seq){
            p.options_seq=optionsSeq;options.team=s.config.player[slot].team;
            if(!SameOptions(options,s.config.player[slot])){s.config.player[slot]=options;++s.config.revision;s.ready_mask&=~(1u<<slot);}
        }
        if(readySeq>p.ready_seq&&revision==s.config.revision){p.ready_seq=readySeq;
            if(ready)s.ready_mask|=1u<<slot;else s.ready_mask&=~(1u<<slot);}
        return;
    }
    if(type==LOADED&&(s.state==GE_LAN_LOADING||s.state==GE_LAN_RUNNING)){
        unsigned revision=r.u32(),loaded=r.u8();
        if(r.done()&&revision==s.config.revision&&loaded<=1){
            if(loaded)s.loaded_mask|=1u<<slot;
            p.last_seen=s.now;
            if(s.state==GE_LAN_RUNNING)SendState(slot,GO);
        }return;
    }
    if(type==INPUT&&s.state==GE_LAN_RUNNING){
        unsigned first=r.u32(),count=r.u8();if(!count||count>8||first>s.next_bundle+WINDOW||first+count<first||first+count>s.next_bundle+WINDOW)return;
        GE_LanPlayerInput incoming[8];memset(incoming,0,sizeof(incoming));for(unsigned n=0;n<count;++n)if(!ReadInput(r,incoming[n]))return;
        if(!r.done())return;
        p.last_seen=s.now;
        for(unsigned n=0;n<count;++n){unsigned t=first+n;if(t<s.next_bundle)continue;InputRow&row=Inputs(t);
            if((row.mask&(1u<<slot))&&!SameInput(row.input[slot],incoming[n])){BeginEnd(GE_LAN_END_PROTOCOL);return;}
            row.input[slot]=incoming[n];row.mask|=1u<<slot;}
        return;
    }
    if(type==PROGRESS&&s.state==GE_LAN_RUNNING){
        unsigned completed=r.u32(),first=r.u32(),count=r.u8();
        if(count>WINDOW||completed>s.next_bundle||completed<first||first+count<first||first+count>completed||first>s.verified+WINDOW)return;
        unsigned hash[WINDOW];for(unsigned n=0;n<count;++n)hash[n]=r.u32();if(!r.done())return;p.last_seen=s.now;
        if(completed>p.completed){p.completed=completed;p.last_progress=s.now;}
        for(unsigned n=0;n<count;++n){unsigned t=first+n;if(t<s.verified||t>=s.verified+WINDOW)continue;HashRow&h=Hashes(t);
            if((h.mask&(1u<<slot))&&h.hash[slot]!=hash[n]){BeginEnd(GE_LAN_END_DESYNC);return;}h.hash[slot]=hash[n];h.mask|=1u<<slot;}
        Verify();return;
    }
}
static void Pump(void) {
    s.now=geLanPlatformNow();
    for(unsigned n=0;n<64;++n){unsigned char packet[PACKET];Endpoint from;int bytes=geLanPlatformReceive(&from.ip,&from.port,packet,sizeof(packet));
        if(bytes<0){if(s.role!=GE_LAN_ROLE_NONE)BeginEnd(GE_LAN_END_NETWORK);break;}if(!bytes)break;Handle(packet,(unsigned)bytes,from);}
    if(Terminal()){
        if(s.now-s.last_end>=END_RETRY_MS){SendEnd();s.last_end=s.now;}
        if(s.now-s.end_since>=END_TIMEOUT_MS){s.state=GE_LAN_ENDED;if(s.clear_pending)ResetSession();}return;
    }
    if(s.state==GE_LAN_BROWSING){
        for(unsigned i=0;i<s.found_count;){if(s.now-s.found[i].seen>HOST_EXPIRE_MS){for(unsigned j=i+1;j<s.found_count;++j)s.found[j-1]=s.found[j];--s.found_count;}else ++i;}
        if(s.now-s.last_discovery>=500){Writer w;Send(s.discovery,DISCOVER,w);s.last_discovery=s.now;}return;
    }
    if(s.state==GE_LAN_JOINING){
        if(s.now-s.state_since>8000){BeginEnd(GE_LAN_END_TIMEOUT);return;}
        if(s.now-s.last_retry>=200){Writer w;w.name(s.host_name);Send(s.peer[0].address,JOIN,w);s.last_retry=s.now;}return;
    }
    if(s.role==GE_LAN_ROLE_NONE)return;
    for(unsigned i=s.role==GE_LAN_ROLE_HOST?1:0;i<(s.role==GE_LAN_ROLE_HOST?s.config.player_count:1);++i){
        if(s.now-s.peer[i].last_seen>PEER_TIMEOUT_MS){BeginEnd(GE_LAN_END_TIMEOUT);return;}}
    if(s.state==GE_LAN_LOADING&&s.now-s.state_since>LOAD_TIMEOUT_MS){BeginEnd(GE_LAN_END_LOAD_TIMEOUT);return;}
    if(s.state==GE_LAN_LOBBY||s.state==GE_LAN_LOADING){
        if(s.now-s.last_send>=HEARTBEAT_MS){
            if(s.role==GE_LAN_ROLE_HOST)SendAllState(s.state==GE_LAN_LOBBY?STATE:START);
            else if(s.state==GE_LAN_LOBBY)SendClientStatus();
            else { /* LOADED doubles as heartbeat only after this console loaded;
                    * while loading, resend status as a dedicated revision ack. */
                SendLoaded();
            }
            s.last_send=s.now;
        }
        if(s.role==GE_LAN_ROLE_HOST&&s.state==GE_LAN_LOADING&&s.loaded_mask==All()){StartRunning();SendAllState(GO);}
        return;
    }
    if(s.state!=GE_LAN_RUNNING)return;
    if(!s.latest.connected){if(!s.local_bad_since)s.local_bad_since=s.now;if(s.now-s.local_bad_since>1000){BeginEnd(GE_LAN_END_CONTROLLER);return;}}
    else s.local_bad_since=0;
    if(s.now-s.peer[s.slot].last_progress>STALL_TIMEOUT_MS){BeginEnd(GE_LAN_END_TIMEOUT);return;}
    if(s.role==GE_LAN_ROLE_HOST)for(unsigned i=1;i<s.config.player_count;++i)if(s.now-s.peer[i].last_progress>STALL_TIMEOUT_MS){BeginEnd(GE_LAN_END_TIMEOUT);return;}
    CaptureInputs();
    if(s.role==GE_LAN_ROLE_HOST&&s.next_bundle<s.verified+WINDOW&&s.next_bundle<=s.next_consume+INPUT_LEAD&&Due(s.next_due)){
        InputRow&r=Inputs(s.next_bundle);
        if(r.mask==All()){
            BundleRow&b=s.bundle[s.next_bundle%RING];b.valid=1;b.tick=s.next_bundle;b.bundle.tick=s.next_bundle;b.bundle.delta=GE_LAN_TICK_DELTA;b.bundle.player_count=s.config.player_count;
            memcpy(b.bundle.input,r.input,sizeof(b.bundle.input));++s.next_bundle;
            unsigned step=33;s.pace_rem+=10;if(s.pace_rem>=30){s.pace_rem-=30;++step;}
            s.next_due+=step;if(s.now-s.next_due<0x80000000u&&s.now-s.next_due>100)s.next_due=s.now+step;
            for(unsigned i=1;i<s.config.player_count;++i)SendBundles(i);
        }
    }
    if(s.now-s.last_retry>=RETRY_MS){
        if(s.role==GE_LAN_ROLE_CLIENT){SendInputs();SendProgress();}
        else for(unsigned i=1;i<s.config.player_count;++i)SendBundles(i);
        s.last_retry=s.now;
    }
    if(s.role==GE_LAN_ROLE_HOST&&s.now-s.last_send>=HEARTBEAT_MS){SendAllState(GO);s.last_send=s.now;}
}
}

extern "C" int GE_LanInit(unsigned port) {
    if(s.initialized)return 1;
    if(!port)port=GE_LAN_PORT;
    if(port>65535)return 0;
    int ok=geLanPlatformOpen(port);Guard guard;s.initialized=ok;s.port=port;s.discovery.ip=0xffffffffu;s.discovery.port=GE_LAN_PORT;
    ResetSession();if(!ok){s.state=GE_LAN_ERROR;Message("LAN socket could not be initialized");}return ok;
}
extern "C" void GE_LanSetContentFingerprint(unsigned fingerprint){Guard guard;if(s.role==GE_LAN_ROLE_NONE)s.content=fingerprint;}
extern "C" void GE_LanService(void){Guard guard;if(s.initialized)Pump();}
extern "C" void GE_LanTearDown(void){Guard guard;if(s.initialized){geLanPlatformClose();s.initialized=0;}ResetSession();}
extern "C" void GE_LanSetDiscoveryAddress(const char*ip,unsigned port){Guard guard;unsigned v=geLanPlatformParseIP(ip);if(v&&port&&port<=65535){s.discovery.ip=v;s.discovery.port=port;}}
extern "C" int GE_LanHost(const char*name){
    Guard guard;if(!s.initialized)return 0;if(Terminal()&&s.now-s.end_since<END_TIMEOUT_MS)return 0;
    if(s.round_active)return 0;
    ResetSession();s.role=GE_LAN_ROLE_HOST;s.slot=0;s.state=GE_LAN_LOBBY;s.session=NewNonce();
    s.config.player_count=1;s.peer[0].epoch=s.epoch;s.peer[0].last_seen=s.peer[0].last_progress=s.now;
    Text(s.host_name,GE_LAN_NAME_BYTES,name&&*name?name:"007xb LAN");Text(s.peer[0].name,GE_LAN_NAME_BYTES,s.host_name);
    s.local_options=s.config.player[0];Message("Hosting LAN: waiting for players");return 1;
}
extern "C" int GE_LanBrowse(void){
    Guard guard;if(!s.initialized||s.round_active||(Terminal()&&s.now-s.end_since<END_TIMEOUT_MS))return 0;
    ResetSession();s.state=GE_LAN_BROWSING;s.last_discovery=s.now-500;Message("Searching LAN for hosts");return 1;
}
extern "C" int GE_LanJoin(unsigned index){
    Guard guard;if(s.state!=GE_LAN_BROWSING||index>=s.found_count)return 0;Found f=s.found[index];
    ResetSession();s.state=GE_LAN_JOINING;s.role=GE_LAN_ROLE_CLIENT;s.session=f.session;s.peer[0].address=f.address;s.peer[0].epoch=f.epoch;
    s.peer[0].last_seen=s.now;s.last_retry=s.now-200;Text(s.host_name,GE_LAN_NAME_BYTES,"LAN player");Message("Joining LAN host");return 1;
}
extern "C" void GE_LanGetStatus(GE_LanStatus*out){
    if(!out)return;
    Guard guard;memset(out,0,sizeof(*out));out->state=s.state;out->role=s.role;out->local_slot=s.slot;
    out->player_count=s.config.player_count;out->ready_mask=s.ready_mask;out->loaded_mask=s.loaded_mask;out->tick=s.next_consume;
    out->discovered_count=s.found_count;out->end_reason=s.end_reason;out->round_active=s.round_active;Text(out->message,sizeof(out->message),s.message);
    geLanPlatformLocalAddress(out->local_address);
    for(unsigned i=0;i<4;++i)Text(out->peer_name[i],GE_LAN_NAME_BYTES,s.peer[i].name);
    for(unsigned i=0;i<s.found_count;++i)out->hosts[i]=s.found[i].info;
}
extern "C" int GE_LanGetConfig(GE_LanMatchConfig*out){Guard guard;if(!out||s.role==GE_LAN_ROLE_NONE||s.state==GE_LAN_JOINING)return 0;*out=s.config;return 1;}
extern "C" int GE_LanPublishHostConfig(const GE_LanMatchConfig*config){
    Guard guard;if(!config||s.role!=GE_LAN_ROLE_HOST||s.state!=GE_LAN_LOBBY)return 0;
    GE_LanMatchConfig c=*config;c.build_id=GE_LAN_BUILD_ID;c.player_count=s.config.player_count;c.revision=s.config.revision;
    for(unsigned i=0;i<4;++i){int team=c.player[i].team;c.player[i]=s.config.player[i];c.player[i].team=team;}
    c.rng_hi=s.config.rng_hi;c.rng_lo=s.config.rng_lo;c.chr_rng_hi=s.config.chr_rng_hi;c.chr_rng_lo=s.config.chr_rng_lo;
    if(!ConfigOK(c,0)){Message("Invalid LAN match settings");return 0;}
    Writer a,b;WriteConfig(a,c);WriteConfig(b,s.config);if(a.pos==b.pos&&memcmp(a.bytes+HEADER,b.bytes+HEADER,a.pos-HEADER)==0)return 1;
    c.revision=s.config.revision+1;s.config=c;s.ready_mask=0;s.local_ready=0;Message("Settings changed: players must Ready again");SendAllState(STATE);return 1;
}
extern "C" int GE_LanSetPlayerOptions(const GE_LanPlayerOptions*options){
    Guard guard;if(!options||!OptionsOK(*options)||s.state!=GE_LAN_LOBBY||s.slot<0)return 0;
    GE_LanPlayerOptions o=*options;o.team=s.config.player[s.slot].team;
    if(SameOptions(o,s.local_options))return 1;
    s.local_options=o;++s.local_options_seq;s.local_ready=0;++s.local_ready_seq;
    if(s.role==GE_LAN_ROLE_HOST){s.config.player[0]=o;++s.config.revision;s.ready_mask&=~1u;s.peer[0].options_seq=s.local_options_seq;s.peer[0].ready_seq=s.local_ready_seq;SendAllState(STATE);}
    s.local_ready_revision=s.config.revision;return 1;
}
extern "C" void GE_LanSetReady(int ready){
    Guard guard;if(s.state!=GE_LAN_LOBBY||s.slot<0)return;s.local_ready=ready?1u:0u;++s.local_ready_seq;s.local_ready_revision=s.config.revision;
    if(s.role==GE_LAN_ROLE_HOST){s.peer[0].ready_seq=s.local_ready_seq;if(ready)s.ready_mask|=1u;else s.ready_mask&=~1u;SendAllState(STATE);}
    else SendClientStatus();
}
static int RequestStartLocked(const GE_LanMatchConfig*resolved){
    if(!resolved||s.state!=GE_LAN_LOBBY||s.role!=GE_LAN_ROLE_HOST||s.config.player_count<2||s.ready_mask!=All()){Message("Every player must select Ready");return 0;}
    GE_LanMatchConfig c=*resolved,compare=c;compare.stage_id=s.config.stage_id;compare.rng_hi=s.config.rng_hi;compare.rng_lo=s.config.rng_lo;compare.chr_rng_hi=s.config.chr_rng_hi;compare.chr_rng_lo=s.config.chr_rng_lo;
    Writer a,b;WriteConfig(a,compare);WriteConfig(b,s.config);
    if(a.pos!=b.pos||memcmp(a.bytes+HEADER,b.bytes+HEADER,a.pos-HEADER)!=0||!ConfigOK(c,1)){Message("Match settings or roster changed; Ready again");return 0;}
    c.rng_hi=NewNonce();c.rng_lo=NewNonce();c.chr_rng_hi=NewNonce();c.chr_rng_lo=NewNonce();++c.revision;
    s.config=c;s.state=GE_LAN_LOADING;s.state_since=s.now;s.round_active=1;s.start_pending=1;s.load_notified=0;s.loaded_mask=0;
    for(unsigned i=0;i<c.player_count;++i)s.peer[i].last_seen=s.now;
    s.latest.pressed=s.latest.native_pressed=0;
    Message("Loading agreed LAN match");SendAllState(START);return 1;
}
extern "C" int GE_LanRequestStart(void){Guard guard;return RequestStartLocked(&s.config);}
extern "C" int GE_LanRequestStartWithConfig(const GE_LanMatchConfig*c){Guard guard;return RequestStartLocked(c);}
extern "C" int GE_LanPollStartConfiguration(GE_LanMatchConfig*out){Guard guard;if(!out||!s.start_pending||s.state!=GE_LAN_LOADING)return 0;*out=s.config;s.start_pending=0;return 1;}
extern "C" void GE_LanMarkLoaded(void){Guard guard;if(s.state!=GE_LAN_LOADING||s.slot<0)return;s.load_notified=1;s.loaded_mask|=1u<<s.slot;if(s.role==GE_LAN_ROLE_CLIENT)SendLoaded();}
extern "C" void GE_LanPublishLocalInput(const GE_LanPlayerInput*input){Guard guard;if(!input)return;unsigned legacy=s.latest.pressed|input->pressed,native=s.latest.native_pressed|input->native_pressed;s.latest=*input;s.latest.pressed=(unsigned short)legacy;s.latest.native_pressed=(unsigned short)native;}
extern "C" int GE_LanTryBeginTick(GE_LanTick*out){
    Guard guard;if(Terminal()||s.state==GE_LAN_ERROR)return -1;if(s.state!=GE_LAN_RUNNING||!out||s.tick_inflight)return 0;
    BundleRow&b=s.bundle[s.next_consume%RING];if(!b.valid||b.tick!=s.next_consume)return 0;
    *out=b.bundle;s.tick_inflight=1;return 1;
}
extern "C" void GE_LanEndTick(unsigned checksum){
    Guard guard;if(s.state!=GE_LAN_RUNNING||!s.tick_inflight||s.slot<0)return;HashRow&h=Hashes(s.next_consume);h.hash[s.slot]=checksum;h.mask|=1u<<s.slot;
    ++s.next_consume;s.tick_inflight=0;s.peer[s.slot].completed=s.next_consume;s.peer[s.slot].last_progress=geLanPlatformNow();if(s.role==GE_LAN_ROLE_HOST)Verify();
}
extern "C" void GE_LanRequestEnd(int reason){Guard guard;s.now=geLanPlatformNow();if(s.role==GE_LAN_ROLE_NONE){ResetSession();return;}BeginEnd(reason);}
extern "C" void GE_LanClearAfterEnd(void){Guard guard;if(Terminal()||s.state==GE_LAN_ERROR){s.round_active=0;s.clear_pending=1;s.now=geLanPlatformNow();if(s.now-s.end_since>=END_TIMEOUT_MS||s.state==GE_LAN_ERROR)ResetSession();}}
extern "C" int GE_LanIsMatch(void){Guard guard;return s.round_active;}
extern "C" int GE_LanGetLocalSlot(void){Guard guard;return s.slot;}
extern "C" const char* GE_LanEndReasonText(int reason){
    switch(reason){case GE_LAN_END_QUIT:return "A player left: the LAN match has ended";
    case GE_LAN_END_TIMEOUT:return "A peer stopped responding: LAN match ended";
    case GE_LAN_END_DESYNC:return "Game state differed: LAN test match ended";
    case GE_LAN_END_FINISHED:return "LAN match finished";
    case GE_LAN_END_PROTOCOL:return "Invalid session data: LAN match ended";
    case GE_LAN_END_NETWORK:return "Network unavailable: LAN match ended";
    case GE_LAN_END_LOAD_TIMEOUT:return "A peer did not finish loading: LAN match ended";
    case GE_LAN_END_CONTROLLER:return "A controller disconnected: LAN match ended";
    default:return "LAN session closed";}
}
