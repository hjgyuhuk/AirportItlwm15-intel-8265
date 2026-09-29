#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <HAL/ItlJoinEvent.h>
#include "pmf_types.inc"
#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define DPRINTF(x) ((void)0)
#define LINK_STATE_DOWN 0
#define IEEE80211_REASON_AUTH_EXPIRE 2
#define IEEE80211_CATEG_SA_QUERY 8
#define IEEE80211_ACTION_SA_QUERY_REQ 0
#define IEEE80211_ACTION_SA_QUERY_RESP 1
#define LE_READ_2(p) (uint16_t((p)[0]) | uint16_t((p)[1]) << 8)
struct Timer { bool pending = false; unsigned milliseconds = 0; };
struct ieee80211com;
struct ieee80211_node {
    ieee80211com *ni_ic = nullptr;
    unsigned ni_flags = IEEE80211_NODE_MFP | IEEE80211_NODE_TXMGMTPROT | IEEE80211_NODE_RXMGMTPROT;
    uint8_t ni_bssid[6] = {2,1,2,3,4,5};
    uint16_t ni_sa_query_trid = 0, ni_sa_query_peer_trid = 0;
    int ni_sa_query_count = 0, ni_port_valid = 1;
    Timer *ni_sa_query_to = nullptr;
};
struct ieee80211com {
    ieee80211_node *ic_bss = nullptr;
    int ic_opmode = IEEE80211_M_STA, ic_state = IEEE80211_S_RUN, link = 1;
    unsigned ic_flags = IEEE80211_F_RSNON, ic_xflags = 0;
    uint8_t ic_myaddr[6] = {2,6,7,8,9,10};
};
struct Mbuf { std::vector<uint8_t> bytes; };
using mbuf_t = Mbuf *;
static unsigned live, queries, responses, disconnects, decrypts;
static bool decryptOK = true;
static uint16_t echoed;
static size_t mbuf_len(mbuf_t m) { return m->bytes.size(); }
static void mbuf_freem(mbuf_t m) { if(m) { assert(live); --live; delete m; } }
#define mtod(m,t) reinterpret_cast<t>((m)->bytes.data())
static int splnet() { return 0; }
static void splx(int) {}
static uint32_t queryRandom() { return 0xfffd; }
#define arc4random queryRandom
static void timeout_del(Timer **t) { if (*t) (*t)->pending = false; }
static void timeout_add_msec(Timer **t, unsigned ms) { assert(*t); (*t)->pending = true; (*t)->milliseconds = ms; }
static void sendAction(ieee80211_node *ni,unsigned cat,unsigned action) {
    assert(cat == IEEE80211_CATEG_SA_QUERY);
    if(action == 0) ++queries;
    else { ++responses; echoed = ni->ni_sa_query_peer_trid; }
}
#undef IEEE80211_SEND_ACTION
#define IEEE80211_SEND_ACTION(ic,ni,cat,action,arg) sendAction(ni,cat,action)
static void ieee80211_set_link_state(ieee80211com *ic,int value) { ic->link = value; }
static void ieee80211_notify_native_join(ieee80211com *,ieee80211_node *,ItlJoin::Stage stage,unsigned reason) {
    assert(stage == ItlJoin::Stage::Deauth && reason == IEEE80211_REASON_AUTH_EXPIRE); ++disconnects;
}
static mbuf_t ieee80211_decrypt(ieee80211com *,mbuf_t m,ieee80211_node *) {
    ++decrypts; if(decryptOK) return m; mbuf_freem(m); return nullptr;
}
void ieee80211_sa_query_request(ieee80211com *,ieee80211_node *);
#include "pmf_functions.inc"
static mbuf_t frame(ieee80211com &ic,ieee80211_node &ni,unsigned subtype,unsigned reason,bool protectedFrame=false) {
    auto *m = new Mbuf; ++live; m->bytes.resize(28);
    auto *wh = mtod(m,ieee80211_frame *);
    wh->i_fc[0] = subtype; wh->i_fc[1] = protectedFrame ? IEEE80211_FC1_PROTECTED : 0;
    memcpy(wh->i_addr1,ic.ic_myaddr,6); memcpy(wh->i_addr2,ni.ni_bssid,6);memcpy(wh->i_addr3,ni.ni_bssid,6);
    m->bytes[24]=reason; return m;
}
int main() {
    unsigned cases=0;
    for (unsigned subtype : {IEEE80211_FC0_SUBTYPE_DEAUTH,IEEE80211_FC0_SUBTYPE_DISASSOC})
    for (unsigned reason=0;reason<32;++reason) for (unsigned mismatch=0;mismatch<4;++mismatch) {
        ieee80211com ic; ieee80211_node ni; Timer timer;
        ni.ni_ic=&ic;ic.ic_bss=&ni;ni.ni_sa_query_to=&timer;
        const unsigned old=queries;
        auto *m=frame(ic,ni,subtype,reason);
        if(mismatch) m->bytes[4+(mismatch-1)*6]^=2;
        assert(!ieee80211_mfp_input(&ic,m,&ni,subtype));
        const bool started=(reason==6||reason==7)&&!mismatch;
        assert(queries==old+unsigned(started));assert(ni.ni_port_valid&&ic.link==1);
        if(started) {
            assert(timer.pending&&timer.milliseconds==205);
            // Repeated forged disconnect cannot restart/extend this query.
            m=frame(ic,ni,subtype,reason);assert(!ieee80211_mfp_input(&ic,m,&ni,subtype));
            assert(queries==old+1);
            const uint16_t first=ni.ni_sa_query_trid;
            ieee80211_sa_query_timeout(&ni);ieee80211_sa_query_timeout(&ni);
            // Peer query must not replace our outstanding identifiers.
            m=frame(ic,ni,IEEE80211_FC0_SUBTYPE_ACTION,8,true);
            m->bytes[26]=0x34;m->bytes[27]=0x12;
            ieee80211_recv_sa_query_req(&ic,m,&ni);assert(echoed==0x1234);
            assert(uint16_t(ni.ni_sa_query_trid-first)==2);mbuf_freem(m);
            // Any still-outstanding request ID is accepted, including wrap.
            m=frame(ic,ni,IEEE80211_FC0_SUBTYPE_ACTION,8,true);
            m->bytes[26]=first&255;m->bytes[27]=first>>8;
            ieee80211_recv_sa_query_resp(&ic,m,&ni);mbuf_freem(m);
            assert(!timer.pending&&!(ni.ni_flags&IEEE80211_NODE_SA_QUERY));
            const unsigned disconnected=disconnects;ieee80211_sa_query_timeout(&ni);
            assert(disconnects==disconnected);
        }
        ++cases;
    }
    for(unsigned invalid=0;invalid<7;++invalid) {
        ieee80211com ic;ieee80211_node ni;Timer timer;ni.ni_ic=&ic;ic.ic_bss=&ni;ni.ni_sa_query_to=&timer;
        ieee80211_sa_query_request(&ic,&ni);
        auto *m=frame(ic,ni,IEEE80211_FC0_SUBTYPE_ACTION,8,true);
        m->bytes[26]=ni.ni_sa_query_trid&255;m->bytes[27]=ni.ni_sa_query_trid>>8;
        if(invalid<3)m->bytes[4+invalid*6]^=2;
        if(invalid==3)m->bytes[26]^=1;
        if(invalid==4)m->bytes.resize(27);
        if(invalid==5)ni.ni_port_valid=0;
        if(invalid==6)ic.ic_state=IEEE80211_S_INIT;
        ieee80211_recv_sa_query_resp(&ic,m,&ni);mbuf_freem(m);
        assert(timer.pending&&(ni.ni_flags&IEEE80211_NODE_SA_QUERY));++cases;
    }
    for(unsigned stale=0;stale<3;++stale) {
        ieee80211com ic;ieee80211_node ni;Timer timer;ni.ni_ic=&ic;ic.ic_bss=&ni;ni.ni_sa_query_to=&timer;
        unsigned old=disconnects;ieee80211_sa_query_request(&ic,&ni);
        if(stale==1)ic.ic_state=IEEE80211_S_INIT;
        if(stale==2)ic.ic_bss=nullptr;
        for(unsigned i=0;i<5;++i)ieee80211_sa_query_timeout(&ni);
        assert(!(ni.ni_flags&IEEE80211_NODE_SA_QUERY));assert(disconnects==old+(stale==0));++cases;
    }
    for(unsigned category=0;category<128;++category) {
        ieee80211com ic;ieee80211_node ni;ic.ic_bss=&ni;
        bool robust=category!=4&&category!=7&&category!=11&&category!=15&&category!=20&&category!=21&&category!=22&&category!=127;
        auto *m=frame(ic,ni,IEEE80211_FC0_SUBTYPE_ACTION,category);
        m=ieee80211_mfp_input(&ic,m,&ni,IEEE80211_FC0_SUBTYPE_ACTION);
        assert(bool(m)!=robust);mbuf_freem(m);++cases;
    }
    for(bool ok : {false,true}) for(bool multicast : {false,true}) {
        ieee80211com ic;ieee80211_node ni;ic.ic_bss=&ni;decryptOK=ok;
        auto *m=frame(ic,ni,IEEE80211_FC0_SUBTYPE_DEAUTH,3,!multicast);
        if(multicast)m->bytes[4]=0xff;
        auto old=decrypts;m=ieee80211_mfp_input(&ic,m,&ni,IEEE80211_FC0_SUBTYPE_DEAUTH);
        assert(decrypts==old+1&&bool(m)==ok);mbuf_freem(m);++cases;
    }
    assert(!live);printf("PMF real receive/SA Query: %u cases passed; crypto separately tested, timers mocked\n",cases);
}
