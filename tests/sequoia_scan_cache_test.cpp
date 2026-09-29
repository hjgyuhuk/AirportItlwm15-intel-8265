#include <Airport/SequoiaScanPlan.h>
#include <HAL/ItlNativeScan.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <vector>

using namespace SequoiaWCL;
#define nitems(array) (sizeof(array) / sizeof((array)[0]))
#define isset(map, index) ((map)[(index)])
enum {
    IWM_REPLY_RX_PHY_CMD = 0xc0, IWM_REPLY_RX_MPDU_CMD = 0xc1,
    IWM_SCAN_OFFLOAD_COMPLETE = 0x6d, IEEE80211_S_INIT = 0,
    IEEE80211_FC0_SUBTYPE_MASK = 0xf0, IEEE80211_FC0_SUBTYPE_PROBE_RESP = 0x50,
    MBUF_DONTWAIT = 1, IWM_MIN_DBM = -100
};
struct Mbuf { std::vector<uint8_t> bytes; explicit Mbuf(size_t size) : bytes(size) {} };
using mbuf_t = Mbuf *;
static unsigned allocated = 0, freed = 0, collected = 0;
static int failure = 0;
static int mbuf_allocpacket(int flags, size_t size, void *, mbuf_t *out) {
    assert(flags == MBUF_DONTWAIT);
    if (failure == 1) return ENOMEM;
    *out = new Mbuf(size); ++allocated; return 0;
}
static void mbuf_freem(mbuf_t packet) { assert(packet); delete packet; ++freed; }
static size_t mbuf_len(mbuf_t packet) { return packet->bytes.size(); }
static mbuf_t mbuf_next(mbuf_t packet) { (void)packet; return nullptr; }
static int mbuf_copyback(mbuf_t packet, size_t offset, size_t length, const void *data, int flags) {
    assert(flags == MBUF_DONTWAIT && offset == 0 && length == packet->bytes.size());
    if (failure == 2) return ENOMEM;
    memcpy(packet->bytes.data(), data, length); return 0;
}
struct ieee80211_rxinfo { unsigned rxi_chan = 0; int rxi_rssi = 0; };
struct ieee80211com {
    int ic_state = 0, ic_max_rssi = 100;
    int ic_max_nnodes = 0;
    bool ic_chan_avail[256] = {};
    struct { unsigned ic_freq = 0; } ic_channels[256];
};
static void *ieee80211_find_node(ieee80211com *, const uint8_t *) { return nullptr; }
static ieee80211_rxinfo lastInfo;
static int lastProbe = 0;
static void ieee80211_collect_bss(ieee80211com *ic, mbuf_t packet, ieee80211_rxinfo *info, int probe) {
    assert(ic->ic_state == 0 && packet->bytes.size() >= 36);
    lastInfo = *info; lastProbe = probe; ++collected;
}
class ItlIwm {
public:
    struct { ieee80211com sc_ic; } com;
    struct Loop { bool gated = true; bool inGate() const { return gated; } } loop;
    Loop *getMainWorkLoop() { return &loop; }
    const ScanPlan *nativeScanPlan = nullptr;
    struct { bool active = true; bool valid() const { return active; } } nativeScanSink;
    int nativeScanCancel = 0;
    ItlScan::LegacyPhy nativeScanCachePhy;
    ItlScan::CacheDiagnostic nativeCacheDiagnostic;
    void iwm_native_scan_cache_rx(uint16_t, const void *, size_t);
};
#include "../itlwm/hal_iwm/native_scan_cache_sequoia.inc"

int main()
{
    ScanPlan plan;
    ScanRequest request = {};
    request.version = 1; request.scanType = 2; request.passiveDwell = 110;
    Channel inventory[] = {{1, 1, Width20MHz | Band2GHz | ActiveScan}};
    const uint8_t mac[] = {2, 0, 0, 0, 0, 1};
    assert(plan.prepare(&request, sizeof(request), inventory, 1, mac, nullptr, 0, false) == ScanPlanStatus::Success);
    ItlIwm driver;
    driver.nativeScanPlan = &plan;
    driver.com.sc_ic.ic_chan_avail[1] = true;
    driver.com.sc_ic.ic_channels[1].ic_freq = 2412;
    uint8_t phy[72] = {}; phy[22] = 1; phy[28] = 55;
    uint8_t mpdu[49] = {};
    mpdu[0] = 41; mpdu[4] = 0x80;
    memcpy(mpdu + 14, mac, 6); memcpy(mpdu + 20, mac, 6);
    mpdu[38] = 1; // ESS capability.
    mpdu[40] = 0; mpdu[41] = 0; // Hidden SSID.
    mpdu[42] = 1; mpdu[43] = 1; mpdu[44] = 0x82; // Supported rate.
    mpdu[45] = 3; // CRC and overrun OK.
    auto receive = [&] {
        driver.iwm_native_scan_cache_rx(0xc0, phy, sizeof(phy));
        driver.iwm_native_scan_cache_rx(0xc1, mpdu, sizeof(mpdu));
        assert(allocated == freed);
    };
    driver.iwm_native_scan_cache_rx(0xc1, mpdu, sizeof(mpdu));
    assert(!collected && !allocated);
    receive();
    assert(collected == 1 && lastInfo.rxi_chan == 1 && lastInfo.rxi_rssi == 45 && !lastProbe);
    mpdu[4] = 0x50; receive(); assert(lastProbe && collected == 2);
    const auto good = collected;
    for (failure = 1; failure <= 2; ++failure) receive();
    failure = 0; assert(collected == good);
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        switch (scenario) {
            case 0: driver.nativeScanPlan = nullptr; break;
            case 1: driver.nativeScanSink.active = false; break;
            case 2: driver.nativeScanCancel = 1; break;
            case 3: driver.loop.gated = false; break;
            case 4: driver.com.sc_ic.ic_state = 4; break;
            case 5: driver.com.sc_ic.ic_chan_avail[1] = false; break;
            case 6: mpdu[45] = 0; break;
            case 7: mpdu[14] = 4; break;
            case 8: mpdu[38] = 2; break;
            case 9: mpdu[0] = 0xff; break;
        }
        receive(); assert(collected == good);
        driver.nativeScanPlan = &plan; driver.nativeScanSink.active = true;
        driver.nativeScanCancel = 0; driver.loop.gated = true; driver.com.sc_ic.ic_state = 0;
        driver.com.sc_ic.ic_chan_avail[1] = true; mpdu[45] = 3; mpdu[14] = 2; mpdu[38] = 1; mpdu[0] = 41;
    }
    receive();
    const auto after = collected;
    driver.iwm_native_scan_cache_rx(0x6d, nullptr, 0);
    driver.iwm_native_scan_cache_rx(0xc1, mpdu, sizeof(mpdu));
    assert(collected == after);
    for (unsigned size = 0; size < 49; ++size) {
        driver.iwm_native_scan_cache_rx(0xc0, phy, sizeof(phy));
        driver.iwm_native_scan_cache_rx(0xc1, mpdu, size);
        assert(collected == after && allocated == freed);
    }
    phy[28] = 0; receive(); assert(lastInfo.rxi_rssi == 0);
    phy[28] = 1; driver.com.sc_ic.ic_max_rssi = 80; receive(); assert(lastInfo.rxi_rssi == 80);
    printf("Native scan cache: actual RX adapter, 3 allocation faults, admission/CRC/identity/NVM bounds and ownership passed; BSD collection mocked\n");
}
