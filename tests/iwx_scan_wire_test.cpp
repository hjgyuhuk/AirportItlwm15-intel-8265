#include <Airport/SequoiaScanPlan.h>
#include <cassert>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <vector>
using u8 = uint8_t;
#define ETHER_ADDR_LEN 6
#define BIT(n) (1U << (n))
#include "iwx_scan_wire_types.inc"
#define IO80211FAMILY_V2 1
#define __IO80211_TARGET 150000
#define __MAC_15_0 150000
#define XYLog(...) ((void)0)
#define container_of(ptr, type, member) current
#define htole16(x) (x)
#define htole32(x) (x)
#define cpu_to_le16(x) (x)
#define IEEE80211_CHAN_MAX 196
#define IEEE80211_ELEMID_SSID 0
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->ic_freq < 3000)
#define isset(a,b) ((a) != 0)
struct ieee80211_channel { unsigned ic_flags = 1, ic_freq = 0; };
struct ieee80211com {
    int ic_des_esslen = 1;
    uint8_t ic_des_essid[32] = {'x'};
    ieee80211_channel ic_channels[197];
};
struct iwx_softc { ieee80211com sc_ic; unsigned sc_ucode_api = 1, sc_capa_n_scan_channels = 52; };
static unsigned ieee80211_mhz2ieee(unsigned f, int) { return f < 3000 ? (f - 2407) / 5 : (f - 5000) / 5; }
static uint32_t iwx_cmd_id(unsigned op, unsigned group, unsigned ver) { return op | group << 8 | ver << 16; }
enum { M_NOWAIT = 1, M_WAITOK = 2, M_ZERO = 4 };
static int liveAllocations = 0;
static bool allocationFailure = false;
static void *allocate(size_t size, int, int) {
    if (allocationFailure) return nullptr;
    auto *p = std::calloc(1, size);
    if (p) ++liveAllocations;
    return p;
}
static void release(void *p) { assert(p && liveAllocations > 0); --liveAllocations; std::free(p); }
#define malloc(size, type, flags) allocate(size, type, flags)
#define free(p) release(p)
class ItlIwx;
static ItlIwx *current;
class ItlIwx {
public:
    bool nativeScanWildcard = false;
    const SequoiaWCL::ScanPlan *nativeScanPlan = nullptr;
    unsigned version = 12;
    std::vector<uint8_t> command;
    uint32_t flags = 0;
    int probeError = 0, sendError = 0;
    size_t iwx_umac_scan_size(iwx_softc *) { return version == 12 ? sizeof(iwx_scan_req_umac_v12) : sizeof(iwx_scan_req_umac_v14); }
    int iwx_fill_probe_req(iwx_softc *, iwx_scan_probe_req *) { return probeError; }
    int iwx_send_cmd(iwx_softc *, iwx_host_cmd *c) {
        const auto *p = static_cast<const uint8_t *>(c->data[0]);
        command.assign(p, p + c->len[0]); flags = c->flags; return sendError;
    }
    int iwx_umac_scan_fill_ssids(iwx_softc *, iwx_ssid_ie *, int);
    uint8_t iwx_umac_scan_fill_channels(iwx_softc *, iwx_scan_channel_cfg_umac *, int, int);
    int iwx_umac_scan_v12(iwx_softc *, int);
    int iwx_umac_scan_v14(iwx_softc *, int);
};
#include "iwx_scan_wire_methods.inc"
#undef malloc
#undef free
int main() {
    using namespace SequoiaWCL;
    unsigned traces = 0;
#ifndef REPRO_MULTI_SSID
    for (auto version : {12u,14u}) for (int bg : {0,1}) for (bool native : {false,true}) {
        ItlIwx d; current = &d; d.version = version;
        iwx_softc sc;
        sc.sc_ic.ic_channels[1].ic_freq = 2412;
        sc.sc_ic.ic_channels[144].ic_freq = 5720;
        for (unsigned i = 1; i <= 196; ++i)
            if (i != 1 && i != 144) sc.sc_ic.ic_channels[i].ic_flags = 0;
        Channel inventory[2];
        assert(channel20MHz(1,Band::GHz2,false,false,inventory[0]));
        assert(channel20MHz(144,Band::GHz5,true,true,inventory[1]));
        ScanRequest request = {}; request.version = 1; request.scanType = 1;
        request.activeDwell = 20; request.passiveDwell = 110;
        request.ssidLength = 1; request.ssid[0] = 'x';
        uint8_t mac[6] = {2,1,2,3,4,5}, entropy[6] = {2,6,7,8,9,10};
        ScanPlan plan;
        assert(plan.prepare(&request,sizeof(request),inventory,2,mac,entropy,6,false) == ScanPlanStatus::Success);
        if (native) d.nativeScanPlan = &plan;
        assert((version == 12 ? d.iwx_umac_scan_v12(&sc,bg) : d.iwx_umac_scan_v14(&sc,bg)) == 0);
        // Both APIs deliberately share general/periodic layout, but use
        // different channel/probe structs. Read each real wire type separately.
        const auto verify = [&](const auto &p) {
            assert(bool(p.general_params.flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_NTFY_ITER_COMPLETE) == native);
            assert(bool(p.general_params.flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE) == bool(bg));
            assert(p.general_params.max_out_of_time[0] == (bg ? 120u : 0u));
            assert(p.general_params.suspend_time[1] == (bg ? 120u : 0u));
            assert(p.periodic_params.schedule[0].iter_count == 1);
            assert(p.channel_params.count == 2);
            assert(p.channel_params.channel_config[0].v2.channel_num == 1);
            assert(p.channel_params.channel_config[1].v2.channel_num == 144);
            assert(p.channel_params.channel_config[0].flags == (bg || version==12 ? 0u : 1u));
            if (native) assert(p.channel_params.channel_config[1].flags == 0); // DFS stays passive.
            assert(bool(d.flags & IWX_CMD_ASYNC) == bool(bg));
        };
        if (version == 12) {
            iwx_scan_req_umac_v12 wire; memcpy(&wire,d.command.data(),sizeof(wire)); verify(wire.scan_params);
            assert(wire.scan_params.probe_params.ssid_num == (bg ? 0 : 1));
        } else {
            iwx_scan_req_umac_v14 wire; memcpy(&wire,d.command.data(),sizeof(wire)); verify(wire.scan_params);
        }
        ++traces;
    }
#endif
    for (auto version : {14u,12u}) for (unsigned count=0;count<=kMaxScanSSIDs;++count)
    for (unsigned form=0;form<5;++form) for (int bg : {0,1})
    for (bool passive : {false,true}) for (bool dfsOnly : {false,true}) {
#ifdef REPRO_MULTI_SSID
        if (count!=2 || form!=0 || bg || passive || dfsOnly) continue;
#endif
        ItlIwx d; current=&d; d.version=version;
        iwx_softc sc;
        for (auto &c:sc.sc_ic.ic_channels) c.ic_flags=0;
        sc.sc_ic.ic_channels[1]={1,2412}; sc.sc_ic.ic_channels[144]={1,5720};
        Channel inventory[2];
        assert(channel20MHz(1,Band::GHz2,false,false,inventory[0]));
        assert(channel20MHz(144,Band::GHz5,true,true,inventory[1]));
        ScanRequest request={}; request.version=1; request.scanType=passive?2:1;
        request.activeDwell=20; request.passiveDwell=110; request.ssidCount=count;
        for (unsigned i=0;i<count;++i) {
            auto &ssid=request.ssids[i]; ssid.version=1;
            ssid.length=(i%2)?32:3;
            // Synthetic binary SSIDs, including NUL bytes and maximum length.
            for (unsigned j=0;j<ssid.length;++j) ssid.bytes[j]=j?uint8_t(i+j):0;
            if ((form==1 && i+1==count) || (form==2 && !i) || form==4) ssid={};
            if (!ssid.length && i+1<count) ssid.version=1;
            if (form==3) ssid=request.ssids[0]; // duplicate entries stay bounded
        }
        uint8_t mac[6]={2,1,2,3,4,5}, entropy[6]={2,6,7,8,9,10};
        ScanPlan plan;
        assert(plan.prepare(&request,sizeof(request),inventory+(dfsOnly?1:0),dfsOnly?1:2,
                            mac,entropy,6,false)==ScanPlanStatus::Success);
        if (bg) plan.markAssociatedScan(1);
        d.nativeScanPlan=&plan;
        // Match nativeScan's legacy bridge, while the modern builders must
        // still take every SSID from the plan rather than this single entry.
        sc.sc_ic.ic_des_esslen=plan.active()?plan.probeSSIDLength(0):0;
        if (sc.sc_ic.ic_des_esslen)
            memcpy(sc.sc_ic.ic_des_essid,plan.probeSSID(0),sc.sc_ic.ic_des_esslen);
        d.nativeScanWildcard=plan.active() && !sc.sc_ic.ic_des_esslen;
        assert((version==12?d.iwx_umac_scan_v12(&sc,bg):d.iwx_umac_scan_v14(&sc,bg))==0);
        const unsigned expected=plan.active() && !bg ? (count?count:1):0;
        const auto verify=[&](const auto &p) {
            assert(bool(p.general_params.flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE)==!expected);
            assert(p.channel_params.count==(dfsOnly?1:2));
            for (unsigned i=0;i<IWX_PROBE_OPTION_MAX;++i) {
                const auto &entry=p.probe_params.direct_scan[i];
                const unsigned length=i<expected && count?request.ssids[count-1-i].length:0;
                assert(entry.id==0 && entry.len==length);
                for (unsigned j=0;j<32;++j)
                    assert(entry.ssid[j]==(j<length?request.ssids[count-1-i].bytes[j]:0));
            }
            const uint32_t bitmap=expected && version==14 ? (1u<<expected)-1:0;
            assert(p.channel_params.channel_config[0].flags==(dfsOnly?0:bitmap));
            if (!dfsOnly) assert(p.channel_params.channel_config[1].flags==0);
            assert(p.probe_params.short_ssid_num==0 && p.probe_params.bssid_num==0);
            assert(p.periodic_params.schedule[0].iter_count==1);
        };
        if (version==12) {
            iwx_scan_req_umac_v12 wire; memcpy(&wire,d.command.data(),sizeof(wire)); verify(wire.scan_params);
            assert(wire.scan_params.probe_params.ssid_num==expected);
        } else {
            iwx_scan_req_umac_v14 wire; memcpy(&wire,d.command.data(),sizeof(wire)); verify(wire.scan_params);
        }
        assert(liveAllocations==0); ++traces;
    }
    for (auto version:{12u,14u}) for (unsigned failure=0;failure<5;++failure) {
        ItlIwx d; current=&d; d.version=version; iwx_softc sc; ScanPlan invalid;
        if (failure==0) allocationFailure=true;
        if (failure==1) d.probeError=EIO;
        if (failure==2) d.nativeScanPlan=&invalid;
        if (failure==3) sc.sc_ic.ic_des_esslen=33;
        if (failure==4) d.sendError=EIO;
        const int error=version==12?d.iwx_umac_scan_v12(&sc,0):d.iwx_umac_scan_v14(&sc,0);
        assert(error==(failure==0?ENOMEM:(failure==1 || failure==4)?EIO:EINVAL));
        assert(liveAllocations==0);
        if (failure<4) assert(d.command.empty());
        allocationFailure=false; ++traces;
    }
    printf("IWX actual UMAC builders: %u v12/v14 multi-SSID/wildcard/binary/duplicate/passive/DFS/background/failure-release cases passed; probe construction and send boundary mocked\n",traces);
}
