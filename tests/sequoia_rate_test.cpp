#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/types.h>
#include <Airport/SequoiaTrafficCounters.h>
#define BIT(n) (1u << (n))
#define __packed __attribute__((packed))
#include "sequoia_rate_types.inc"
using IOReturn = int;
using UInt = unsigned;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady, kIOReturnUnsupported };
enum { IWX_DATA_PATH_GROUP, IWX_TLC_MNG_UPDATE_NOTIF };
enum { IWX_RATE_1M_INDEX = 0 };
#define XYLog(...) do {} while (0)
#define WARN_ON(...) do {} while (0)
#define le32toh(v) (v)
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
// Logging, legacy PLCP lookup and workloop/port readiness are host boundaries.
// The VHT conversion, TLC dispatch, generation check and Apple output are real.
static unsigned iwx_hwrate_to_plcp_idx(uint32_t) { return 0; }
struct { uint8_t plcp, rate; } iwx_rates[] = {{10,2}};
struct ieee80211_rateset { unsigned rs_nrates = 0; uint8_t rs_rates[16] = {}; };
struct ieee80211_node { ieee80211_rateset ni_rates; unsigned ni_txmcs = 0, ni_txrate = 0; };
struct ieee80211com { ieee80211_node *ic_bss = nullptr; };
struct iwx_softc { ieee80211com sc_ic; int sc_generation = 3; };
struct ItlNativeHal {
    using VhtRate = ::VhtRate;
    virtual bool nativeVhtRate(VhtRate &) const { return false; }
    virtual bool nativeTxNss(uint32_t &) const { return false; }
    virtual bool nativeMcsIndex(uint32_t &) const { return false; }
    virtual bool nativeTxRateMbps(uint32_t &) const { return false; }
    virtual ~ItlNativeHal() = default;
};
struct ItlIwx : ItlNativeHal {
    iwx_softc com;
    ieee80211_node peer;
    bool nativeJoinMode = true, portReady = true;
    uint32_t nativeTxRateFlags = 0;
    int nativeTxRateGeneration = -1;
    uint8_t notifVersion = 3;
    ItlIwx() { com.sc_ic.ic_bss = &peer; }
    bool nativePortReady() const { return portReady; }
    bool nativeVhtRate(VhtRate &) const override;
    bool nativeTxNss(uint32_t &) const override;
    bool nativeMcsIndex(uint32_t &) const override;
    bool nativeTxRateMbps(uint32_t &) const override;
    void iwx_rs_update(iwx_softc *, iwx_tlc_update_notif *);
    uint8_t iwx_lookup_notif_ver(iwx_softc *, int, int) { return notifVersion; }
    void iwx_rs_pretty_print_rate_v1(char *, size_t, uint32_t) {}
    void iwx_rs_pretty_print_rate(char *, size_t, uint32_t) {}
};
#include "iwx_rate_methods.inc"
struct HAL {
    ItlNativeHal *native;
    ItlNativeHal *getNativeHal() { return native; }
    void *get80211Controller() { return this; }
    void *getDriverInfo() { return this; }
};
static uint64_t continuousTicks = 0;
static uint64_t mach_continuous_time() { return continuousTicks; }
static void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns) { *ns = ticks * 3; }
struct AirportItlwm {
    struct Loop { bool gated = true; bool inGate() { return gated; } } loop;
    Loop *workQueue = &loop;
    Loop *getWorkQueue() { return workQueue; }
    SequoiaTrafficCounters nativeTrafficCounters;
    bool available = true, radio = true, port = true;
    unsigned unsupported = 0;
    bool nativeAvailable() { return available; }
    bool nativeRadioAvailable() { return radio; }
    bool nativePortAvailable() { return port; }
    bool nativeWclQueriesEnabled() { return true; }
    void recordUnsupportedRequest(UInt, bool) { ++unsupported; }
    void recordWclQuery(UInt) {}
    void recordConfigurationSetter(UInt) {}
#define STUB(name) template<class... Args> IOReturn name(Args...) { return kIOReturnUnsupported; }
    STUB(requestNativeScan) STUB(cancelNativeScan) STUB(requestNativeJoin)
    STUB(cancelNativeJoin) STUB(requestNativeLink) STUB(finishNativeLinkConfiguration)
    STUB(requestNativeLeave)
};
struct apple80211_rate_data;
struct AirportItlwmSkywalkInterface {
    AirportItlwm *instance;
    HAL *fHalService;
    IOReturn dispatchSequoiaRequest(UInt, void *);
    IOReturn getSequoiaVhtRate(void *);
    IOReturn getSequoiaNss(void *);
    IOReturn getSequoiaRate(void *);
    IOReturn getSequoiaMcs(void *);
    STUB(getSequoiaHardwareAddress) STUB(getSequoiaLowLatencyInfo) STUB(getSequoiaBSSInfo)
    STUB(getSequoiaExtendedBSSInfo) STUB(setSequoiaFastLane) STUB(getSequoiaBGScanCache)
    STUB(getSequoiaMaxNssForAp) STUB(getSequoiaWclChannelsInfo)
    IOReturn getSequoiaWclTrafficCounters(void *);
    STUB(getCHANNEL) STUB(getPOWERSAVE) STUB(getRATE) STUB(getOP_MODE) STUB(getRSSI)
    STUB(getSequoiaSupportedChannels) STUB(getCHANNELS_INFO) STUB(getMCS) STUB(getNSS)
};
#undef STUB
#include "sequoia_rate_methods.inc"

using u32 = uint32_t;
#define WARN_ON_ONCE(x) (x)
#define WARN_ONCE(...) ((void)0)
enum { RATE_INFO_BW_20, RATE_INFO_BW_40, RATE_INFO_BW_80, RATE_INFO_BW_160,
       RATE_INFO_BW_5, RATE_INFO_BW_10, RATE_INFO_FLAGS_SHORT_GI = 1 };
enum { NL80211_RATE_INFO_HE_GI_0_8, NL80211_RATE_INFO_HE_GI_1_6, NL80211_RATE_INFO_HE_GI_3_2 };
enum { NL80211_RATE_INFO_HE_RU_ALLOC_26, NL80211_RATE_INFO_HE_RU_ALLOC_52,
    NL80211_RATE_INFO_HE_RU_ALLOC_106, NL80211_RATE_INFO_HE_RU_ALLOC_242,
    NL80211_RATE_INFO_HE_RU_ALLOC_484, NL80211_RATE_INFO_HE_RU_ALLOC_996,
    NL80211_RATE_INFO_HE_RU_ALLOC_2x996, RATE_INFO_BW_HE_RU = 10 };
using u64 = uint64_t;
#define WARN(...) ((void)0)
#define do_div(v, d) ((v) /= (d))
struct rate_info { unsigned mcs, nss, bw, flags, he_gi = 0, he_ru_alloc = 0, he_dcm = 0; };
#include "sequoia_rate_reference.inc"

static unsigned currentRateTests() {
    unsigned cases = 0;
    for (unsigned version : {2u,3u,static_cast<unsigned>(IWX_FW_CMD_VER_UNKNOWN)})
    for (bool ht : {false,true}) for (unsigned mcs=0;mcs<(ht?8u:10u);++mcs)
    for (unsigned nss=1;nss<=2;++nss) for (unsigned width=0;width<(ht?2u:4u);++width)
    for (unsigned gi=0;gi<2;++gi) for (unsigned offset=0;offset<8;++offset) {
        ItlIwx d; HAL hal{&d}; AirportItlwm controller;
        AirportItlwmSkywalkInterface iface{&controller,&hal};
        d.notifVersion=version;
        iwx_tlc_update_notif notif = {};
        notif.sta_id=IWX_STATION_ID; notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=mcs | ((nss-1)<<(ht && version!=3?3:4)) | (width<<11) |
            (ht ? (version==3?IWX_RATE_MCS_HT_MSK:IWX_RATE_MCS_HT_MSK_V1) :
                  (version==3?IWX_RATE_MCS_VHT_MSK:IWX_RATE_MCS_VHT_MSK_V1)) |
            (gi ? (version==3?IWX_RATE_MCS_SGI_MSK:IWX_RATE_MCS_SGI_MSK_V1) : 0);
        d.iwx_rs_update(&d.com,&notif);
        // Stale BSD state must not control these native replies.
        d.peer.ni_txmcs=99;
        uint8_t data[40]; memset(data,0xa5,sizeof(data));
        const bool valid=ht || width!=0 || mcs!=9;
        assert(iface.dispatchSequoiaRequest(468,data+offset)==(valid?kIOReturnSuccess:kIOReturnNotReady));
        if (valid) {
            rate_info ref={ht?(nss-1)*8+mcs:mcs,nss,width,gi?RATE_INFO_FLAGS_SHORT_GI:0u};
            unsigned expected=(ht?cfg80211_calculate_bitrate_ht(&ref):cfg80211_calculate_bitrate_vht(&ref))/10;
            uint32_t words[6];memcpy(words,data+offset,sizeof(words));
            assert(words[0]==APPLE80211_VERSION && words[1]==1 && words[2]==expected);
            assert(words[3]==0 && words[4]==0 && words[5]==0);
        }
        for (unsigned i=0;i<sizeof(data);++i)
            if (!valid || i<offset || i>=offset+24) assert(data[i]==0xa5);
        memset(data,0xa5,sizeof(data));
        assert(iface.dispatchSequoiaRequest(473,data+offset)==kIOReturnSuccess);
        uint32_t words[2];memcpy(words,data+offset,sizeof(words));
        assert(words[0]==APPLE80211_VERSION && words[1]==(ht?(nss-1)*8+mcs:mcs));
        for (unsigned i=0;i<sizeof(data);++i)
            if (i<offset || i>=offset+8) assert(data[i]==0xa5);
        ++cases;
    }
    const uint8_t plcp[]={10,20,55,110,13,15,5,7,9,11,1,3};
    const unsigned mbps[]={1,2,5,11,6,9,12,18,24,36,48,54};
    for (unsigned version : {2u,3u,static_cast<unsigned>(IWX_FW_CMD_VER_UNKNOWN)})
    for (unsigned i=0;i<12;++i) {
        ItlIwx d;HAL hal{&d};AirportItlwm controller;AirportItlwmSkywalkInterface iface{&controller,&hal};
        d.notifVersion=version;
        iwx_tlc_update_notif notif={};notif.sta_id=IWX_STATION_ID;notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=version==3 ? (i<4?i:IWX_RATE_MCS_LEGACY_OFDM_MSK|(i-4)) :
                                  plcp[i]|(i<4?IWX_RATE_MCS_CCK_MSK_V1:0);
        d.iwx_rs_update(&d.com,&notif);
        uint32_t words[6]={};
        assert(iface.dispatchSequoiaRequest(468,words)==kIOReturnSuccess && words[2]==mbps[i]);
        assert(iface.dispatchSequoiaRequest(473,words)==kIOReturnNotReady);
        ++cases;
    }
    // Invalid/unsupported formats, stale generation, unready ports, and a
    // later legacy or HE notification cannot return a previous HT/VHT rate.
    for (unsigned fault=0;fault<16;++fault) {
        ItlIwx d;HAL hal{&d};AirportItlwm controller;AirportItlwmSkywalkInterface iface{&controller,&hal};
        iwx_tlc_update_notif notif={};notif.sta_id=IWX_STATION_ID;notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=IWX_RATE_MCS_VHT_MSK|3;d.iwx_rs_update(&d.com,&notif);
        if(fault==0) d.nativeTxRateGeneration=-1;
        if(fault==1) ++d.com.sc_generation;
        if(fault==2) d.portReady=false;
        if(fault==3) controller.radio=false;
        if(fault==4) controller.port=false;
        if(fault==5) controller.available=false;
        if(fault==6) iface.instance=nullptr;
        if(fault==7) iface.fHalService=nullptr;
        if(fault==8) hal.native=nullptr;
        ItlNativeHal unsupported;
        if(fault==9) hal.native=&unsupported;
        if(fault>=10) {
            const uint32_t bad[]={IWX_RATE_MCS_HT_MSK|8,IWX_RATE_MCS_HT_MSK|(2<<11),
                IWX_RATE_MCS_VHT_MSK|10,IWX_RATE_MCS_EHT_MSK,0xffffffffu,IWX_RATE_MCS_HE_MSK|IWX_RATE_MCS_HE_TYPE_MU|11};
            notif.rate=bad[fault-10];d.iwx_rs_update(&d.com,&notif);
        }
        uint8_t data[24];memset(data,0xa5,sizeof(data));
        assert(iface.dispatchSequoiaRequest(468,data)==kIOReturnNotReady);
        for(auto byte:data) assert(byte==0xa5);
        if(fault!=15) {
            assert(iface.dispatchSequoiaRequest(473,data)==kIOReturnNotReady);
            for(auto byte:data) assert(byte==0xa5);
        } else { // MCS query can report HE index without claiming HE bitrate support.
            assert(iface.dispatchSequoiaRequest(473,data)==kIOReturnSuccess);
            uint32_t words[2];memcpy(words,data,8);assert(words[1]==11);
        }
        ++cases;
    }
    // Every old legacy PLCP byte, including valid bytes with the wrong format.
    for(unsigned cck=0;cck<2;++cck) for(unsigned code=0;code<256;++code) {
        ItlIwx d; d.notifVersion=2;
        iwx_tlc_update_notif notif={};notif.sta_id=IWX_STATION_ID;notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=code|(cck?IWX_RATE_MCS_CCK_MSK_V1:0);d.iwx_rs_update(&d.com,&notif);
        int match=-1;for(unsigned i=cck?0:4;i<(cck?4:12);++i) if(plcp[i]==code) match=i;
        uint32_t actual=0xa5a5a5a5;
        assert(d.nativeTxRateMbps(actual)==(match>=0));
        assert(actual==(match>=0?mbps[match]:0xa5a5a5a5));
        ++cases;
    }
    return cases;
}

static unsigned heRateTests() {
    unsigned cases = 0;
    for (unsigned version : {2u, 3u}) for (unsigned type=0; type<4; ++type)
    for (unsigned gi=0; gi<(version==3?8u:5u); ++gi)
    for (unsigned mcs=0; mcs<16; ++mcs) for (unsigned nss=1; nss<=2; ++nss)
    for (unsigned bw=0; bw<4; ++bw) for (unsigned dcm=0; dcm<2; ++dcm)
    for (unsigned ru=0; ru<2; ++ru) {
        // Old GI value4 is encoded using GI3+SGI, only in (extended) SU.
        if (version==2 && gi==4 && type>=2) continue;
        ItlIwx d; d.notifVersion=version;
        iwx_tlc_update_notif notif={}; notif.sta_id=IWX_STATION_ID;
        notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=mcs|((nss-1)<<4)|(bw<<11)|(dcm?IWX_RATE_HE_DUAL_CARRIER_MODE_MSK:0);
        if(version==3) notif.rate |= IWX_RATE_MCS_HE_MSK|(type<<23)|(gi<<20)|(ru<<26);
        else notif.rate |= IWX_RATE_MCS_HE_MSK_V1|(type<<22)|((gi==4?3:gi)<<20)|
            (gi==4?IWX_RATE_MCS_SGI_MSK_V1:0)|(ru<<IWX_RATE_MCS_HE_106T_POS_V1);
        d.iwx_rs_update(&d.com,&notif);
        uint32_t result=0xa5a5a5a5;
        const bool valid=type<2 && gi<5 && mcs<12 &&
            (!dcm || mcs==0 || mcs==1 || mcs==3 || mcs==4) && !ru && (type==0 || bw==0);
        assert(d.nativeTxRateMbps(result)==valid);
        if(valid) {
            rate_info ref={mcs,nss,ru?RATE_INFO_BW_HE_RU:bw,0};
            ref.he_gi=gi==2?1:gi==3?2:0;
            ref.he_ru_alloc=ru?NL80211_RATE_INFO_HE_RU_ALLOC_106:0;ref.he_dcm=dcm;
            unsigned reference=cfg80211_calculate_bitrate_he(&ref)/10;
            // Linux's pre-rounded table differs by at most 1 integer Mbps.
            if (!(result==reference || result+1==reference || result==reference+1))
                fprintf(stderr,"HE v%u type%u gi%u mcs%u nss%u bw%u dcm%u ru%u actual%u ref%u flags%x\n",version,type,gi,mcs,nss,bw,dcm,ru,result,reference,d.nativeTxRateFlags);
            assert(result==reference || result+1==reference || result==reference+1);
            if(mcs==11 && nss==2 && bw==3 && gi==0 && !ru && !dcm) assert(result==2401);
            if(mcs==0 && nss==1 && bw==0 && gi==0 && !ru && !dcm) assert(result==8);
            assert(((d.nativeTxRateFlags & IWX_RATE_MCS_HE_GI_LTF_MSK)>>20)==gi);
        } else assert(result==0xa5a5a5a5);
        ++cases;
    }
    return cases;
}

static unsigned trafficTests() {
    unsigned cases=0;
    for(unsigned offset=0;offset<8;++offset) for(unsigned fault=0;fault<5;++fault) {
        ItlIwx driver;HAL hal{&driver};AirportItlwm controller;
        AirportItlwmSkywalkInterface iface{&controller,&hal};
        controller.nativeTrafficCounters.tx=uint64_t(UINT32_MAX)+19;
        controller.nativeTrafficCounters.rx=uint64_t(UINT32_MAX)+23;
        continuousTicks=1234567890;
        if(fault==1) iface.instance=nullptr;
        if(fault==2) controller.available=false;
        if(fault==3) controller.loop.gated=false;
        if(fault==4) controller.workQueue=nullptr;
        uint8_t bytes[72];memset(bytes,0xa5,sizeof(bytes));
        assert(iface.getSequoiaWclTrafficCounters(bytes+offset)==
            (fault?kIOReturnNotReady:kIOReturnSuccess));
        if(!fault) {
            uint64_t out[7];memcpy(out,bytes+offset,sizeof(out));
            assert(out[0]==0 && out[1]==0 && out[4]==0 && out[5]==0);
            assert(out[2]==uint64_t(UINT32_MAX)+19 && out[3]==uint64_t(UINT32_MAX)+23);
            assert(out[6]==continuousTicks*3);
            controller.radio=false;controller.port=false;
            ++continuousTicks; controller.nativeTrafficCounters.received();
            uint8_t again[56];assert(iface.dispatchSequoiaRequest(521,again)==kIOReturnSuccess);
            uint64_t next[7];memcpy(next,again,sizeof(next));
            assert(next[2]==out[2] && next[3]-out[3]==1 && next[6]-out[6]==3);
        }
        for(unsigned i=0;i<sizeof(bytes);++i)
            if(fault || i<offset || i>=offset+56) assert(bytes[i]==0xa5);
        assert(iface.getSequoiaWclTrafficCounters(nullptr)==kIOReturnBadArgument);
        ++cases;
    }
    return cases;
}

int main() {
    unsigned cases = currentRateTests() + heRateTests() + trafficTests();
    for (unsigned version : {2u,3u,static_cast<unsigned>(IWX_FW_CMD_VER_UNKNOWN)}) for (unsigned mcs=0;mcs<10;++mcs)
    for (unsigned nss=1;nss<=2;++nss) for (unsigned bw=0;bw<4;++bw)
    for (unsigned shortGI=0;shortGI<2;++shortGI) for (unsigned offset=0;offset<8;++offset) {
        ItlIwx d; HAL hal{&d}; AirportItlwm controller;
        AirportItlwmSkywalkInterface iface{&controller,&hal};
        d.notifVersion=version;
        iwx_tlc_update_notif notif = {};
        notif.sta_id=IWX_STATION_ID; notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=mcs | ((nss-1)<<4) | (bw<<11) |
            (version==3 ? IWX_RATE_MCS_VHT_MSK : IWX_RATE_MCS_VHT_MSK_V1) |
            (shortGI ? (version==3 ? IWX_RATE_MCS_SGI_MSK : IWX_RATE_MCS_SGI_MSK_V1) : 0);
        d.iwx_rs_update(&d.com,&notif);
        uint8_t buffer[36]; memset(buffer,0xa5,sizeof(buffer));
        assert(iface.dispatchSequoiaRequest(476,buffer+offset)==kIOReturnSuccess);
        uint32_t words[5]; memcpy(words,buffer+offset,sizeof(words));
        assert(words[0]==APPLE80211_VERSION && words[1]==mcs && words[2]==nss &&
               words[3]==(20u<<bw) && words[4]==(shortGI?400u:800u));
        for (unsigned i=0;i<sizeof(buffer);++i)
            if (i<offset || i>=offset+20) assert(buffer[i]==0xa5);
        assert(controller.unsupported==0 && d.peer.ni_txmcs==mcs);
        memset(buffer,0xa5,sizeof(buffer));
        assert(iface.dispatchSequoiaRequest(504,buffer+offset)==kIOReturnSuccess);
        uint32_t nssWords[2]; memcpy(nssWords,buffer+offset,sizeof(nssWords));
        assert(nssWords[0]==APPLE80211_VERSION && nssWords[1]==nss);
        for (unsigned i=0;i<sizeof(buffer);++i)
            if (i<offset || i>=offset+8) assert(buffer[i]==0xa5);
        ++cases;
    }
    for (unsigned fault=0;fault<16;++fault) {
        ItlIwx d; HAL hal{&d}; ItlNativeHal unsupportedHal; AirportItlwm controller;
        AirportItlwmSkywalkInterface iface{&controller,&hal};
        iwx_tlc_update_notif notif = {};
        notif.sta_id=IWX_STATION_ID; notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=IWX_RATE_MCS_VHT_MSK|7;
        if (fault==1) ++notif.sta_id;
        if (fault==2) notif.flags=0;
        if (fault==3) d.nativeJoinMode=false;
        if (fault==4) notif.rate=IWX_RATE_MCS_VHT_MSK|10;
        if (fault==5) notif.rate|=IWX_RATE_MCS_CHAN_WIDTH_320;
        if (fault!=0) d.iwx_rs_update(&d.com,&notif);
        if (fault==6) ++d.com.sc_generation;
        if (fault==7) d.portReady=false;
        if (fault==8) controller.radio=false;
        if (fault==9) controller.port=false;
        if (fault==10) controller.available=false;
        if (fault==11) iface.instance=nullptr;
        if (fault==12) iface.fHalService=nullptr;
        if (fault==13) hal.native=nullptr;
        if (fault==14) hal.native=&unsupportedHal;
        if (fault==15) { // A subsequent non-VHT rate must invalidate the VHT answer.
            notif.rate=IWX_RATE_MCS_HE_MSK|7; d.iwx_rs_update(&d.com,&notif);
        }
        uint8_t buffer[20]; memset(buffer,0xa5,sizeof(buffer));
        assert(iface.dispatchSequoiaRequest(476,buffer)==kIOReturnNotReady);
        for (auto byte:buffer) assert(byte==0xa5);
        if (fault!=15) {
            assert(iface.dispatchSequoiaRequest(504,buffer)==kIOReturnNotReady);
            for (auto byte:buffer) assert(byte==0xa5);
        }
        ++cases;
    }
    // HT's old format encodes NSS in the aggregate MCS index, whereas the
    // normalized format moves it to bit4. Run the actual TLC conversion.
    for (unsigned version : {2u,3u,static_cast<unsigned>(IWX_FW_CMD_VER_UNKNOWN)})
    for (unsigned nss=1;nss<=2;++nss) for (unsigned mcs=0;mcs<8;++mcs)
    for (unsigned bw=0;bw<2;++bw) {
        ItlIwx d;
        d.notifVersion=version;
        iwx_tlc_update_notif notif = {};
        notif.sta_id=IWX_STATION_ID; notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=(version==3 ? IWX_RATE_MCS_HT_MSK | ((nss-1)<<4)
                             : IWX_RATE_MCS_HT_MSK_V1 | ((nss-1)<<3)) | mcs | (bw<<11);
        d.iwx_rs_update(&d.com,&notif);
        uint32_t actual=0;
        assert(d.nativeTxNss(actual) && actual==nss);
        assert(d.peer.ni_txmcs==(nss-1)*8+mcs);
        ++cases;
    }
    // Every normalized format/MCS/width/NSS combination, including unknown
    // formats and malformed rates. Failed queries must not mutate the reply.
    for (unsigned format=0;format<8;++format) for (unsigned mcs=0;mcs<16;++mcs)
    for (unsigned bw=0;bw<8;++bw) for (unsigned streams=1;streams<=2;++streams) {
        ItlIwx d; HAL hal{&d}; AirportItlwm controller;
        AirportItlwmSkywalkInterface iface{&controller,&hal};
        iwx_tlc_update_notif notif = {};
        notif.sta_id=IWX_STATION_ID; notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        notif.rate=(format<<8)|mcs|(bw<<11)|((streams-1)<<4);
        d.iwx_rs_update(&d.com,&notif);
        const bool valid=format<2 || (format==2 && mcs<8 && bw<2) ||
            (format==3 && mcs<10 && bw<4) || (format==4 && mcs<12 && bw<4);
        const bool validRate=(format==0 && mcs<4 && streams==1) ||
            (format==1 && mcs<8 && streams==1) || (format==2 && mcs<8 && bw<2) ||
            (format==3 && mcs<10 && bw<4 && (bw!=0 || mcs!=9)) ||
            (format==4 && mcs<12 && bw<4);
        uint8_t rateBuffer[26]; memset(rateBuffer,0xa5,sizeof(rateBuffer));
        assert(iface.dispatchSequoiaRequest(468,rateBuffer+1)==
               (validRate?kIOReturnSuccess:kIOReturnNotReady));
        assert(rateBuffer[0]==0xa5 && rateBuffer[25]==0xa5);
        if(!validRate) for(auto byte:rateBuffer) assert(byte==0xa5);
        memset(rateBuffer,0xa5,sizeof(rateBuffer));
        const bool validMcs=valid && format>=2;
        assert(iface.dispatchSequoiaRequest(473,rateBuffer+1)==
               (validMcs?kIOReturnSuccess:kIOReturnNotReady));
        for(unsigned i=0;i<sizeof(rateBuffer);++i)
            if(!validMcs || i<1 || i>=9) assert(rateBuffer[i]==0xa5);
        uint8_t buffer[10]; memset(buffer,0xa5,sizeof(buffer));
        assert(iface.dispatchSequoiaRequest(504,buffer+1)==
               (valid?kIOReturnSuccess:kIOReturnNotReady));
        if (valid) {
            uint32_t words[2]; memcpy(words,buffer+1,sizeof(words));
            assert(words[0]==APPLE80211_VERSION && words[1]==(format<2?1:streams));
            assert(buffer[0]==0xa5 && buffer[9]==0xa5);
        } else {
            for (auto byte:buffer) assert(byte==0xa5);
            uint32_t untouched=0xa5;
            assert(!d.nativeTxNss(untouched) && untouched==0xa5);
        }
        ++cases;
    }
    // A later legacy/HT/VHT/HE notification replaces the previous stream
    // count; this must not keep reporting the peer's maximum of two streams.
    {
        ItlIwx d; HAL hal{&d}; AirportItlwm controller;
        AirportItlwmSkywalkInterface iface{&controller,&hal};
        iwx_tlc_update_notif notif = {};
        notif.sta_id=IWX_STATION_ID; notif.flags=IWX_TLC_NOTIF_FLAG_RATE;
        for (uint32_t rate : {uint32_t(IWX_RATE_MCS_VHT_MSK|16|3),
              uint32_t(IWX_RATE_MCS_VHT_MSK|3), uint32_t(IWX_RATE_MCS_HT_MSK|16|5),
              uint32_t(IWX_RATE_MCS_HE_MSK|5), uint32_t(IWX_RATE_MCS_LEGACY_OFDM_MSK|5)}) {
            notif.rate=rate; d.iwx_rs_update(&d.com,&notif);
            uint32_t words[2]={};
            assert(iface.dispatchSequoiaRequest(504,words)==kIOReturnSuccess);
            assert(words[1]==((rate&16)?2u:1u));
            ++cases;
        }
    }
    ItlIwx d; HAL hal{&d}; AirportItlwm controller;
    AirportItlwmSkywalkInterface iface{&controller,&hal};
    assert(iface.dispatchSequoiaRequest(476,nullptr)==kIOReturnBadArgument);
    assert(iface.dispatchSequoiaRequest(504,nullptr)==kIOReturnBadArgument);
    assert(iface.dispatchSequoiaRequest(468,nullptr)==kIOReturnBadArgument);
    assert(iface.dispatchSequoiaRequest(473,nullptr)==kIOReturnBadArgument);
    assert(iface.dispatchSequoiaRequest(999,nullptr)==kIOReturnUnsupported);
    assert(controller.unsupported==1);
    printf("Native RATE/MCS/VHT/NSS: %u TLC/legacy-format conversion/unaligned ABI/lifecycle cases passed; firmware, port and workloop mocked\n",cases+5);
}
