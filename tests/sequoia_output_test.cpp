#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using UInt32 = uint32_t;
#define __IO80211_TARGET 150000
#define __MAC_15_0 150000
enum { kIOReturnOutputSuccess, kIOReturnOutputDropped, MBUF_TYPE_FREE = 0,
       MBUF_PKTHDR = 1, IEEE80211_S_RUN = 4 };
struct Packet { unsigned type = 1, flags = MBUF_PKTHDR; };
using mbuf_t = Packet *;
static unsigned live = 0, freed = 0;
static unsigned mbuf_type(mbuf_t m) { assert(m); return m->type; }
static unsigned mbuf_flags(mbuf_t m) { assert(m && m->type); return m->flags; }
static void dispose(mbuf_t m) { assert(m && m->type && live); --live; ++freed; delete m; }
struct Queue {
    bool reject = false; mbuf_t queued = nullptr; unsigned calls = 0;
    bool lockEnqueue(mbuf_t m) { ++calls; assert(m && m->type && !queued);
        if (reject) return false; queued = m; return true; }
};
struct Stats { unsigned outputErrors = 0; };
struct _ifnet {
    Queue queue;
    struct { Queue *queue; } if_snd{&queue};
    Stats stats, *netStat = &stats;
    unsigned starts = 0;
    void (*if_start)(_ifnet *) = [](_ifnet *p) { ++p->starts; assert(p->queue.queued); };
};
struct Controller { struct { _ifnet ac_if; } ic_ac; int ic_state = IEEE80211_S_RUN; };
struct HAL { Controller ic; Controller *get80211Controller() { return &ic; } };
struct AirportItlwm {
    HAL hal, *fHalService = &hal;
    unsigned nativeTxOutputCalls = 0, nativeTxOutputGated = 0;
    void freePacket(mbuf_t m) { dispose(m); }
    UInt32 outputPacket(mbuf_t, void *);
};
#include "sequoia_output_method.inc"
int main() {
    unsigned cases=0;
    for(unsigned state=0;state<4;++state) for(unsigned kind=0;kind<4;++kind)
    for(bool stats : {false,true}) for(bool reject : {false,true}) {
        AirportItlwm d;auto &ifp=d.hal.ic.ic_ac.ac_if;
        if(state==1) d.hal.ic.ic_state=0;
        if(state==2) ifp.if_snd.queue=nullptr;
        if(state==3) ifp.if_start=nullptr;
        if(!stats) ifp.netStat=nullptr;
        ifp.queue.reject=reject;
        Packet sentinel;sentinel.type=MBUF_TYPE_FREE;
        mbuf_t m=kind==0?nullptr:kind==1?&sentinel:new Packet;
        if(kind>=2) {++live;if(kind==2)m->flags=0;}
        const bool accepted=state==0 && kind==3 && !reject;
        const auto initialFreed=freed;
        assert(d.outputPacket(m,nullptr)==(accepted?kIOReturnOutputSuccess:kIOReturnOutputDropped));
        assert(ifp.starts==unsigned(accepted));
        assert(ifp.queue.calls==unsigned(state==0 && kind==3));
        assert(freed-initialFreed==unsigned(kind>=2 && !accepted));
        assert(ifp.stats.outputErrors==unsigned(stats && state==0 && kind!=3));
        if(ifp.queue.queued) dispose(ifp.queue.queued);
        assert(!live);++cases;
    }
    printf("Legacy TX: %u null/header/free-marker/queue/start/admission ownership cases passed; mbuf and queue mocked\n",cases);
}
