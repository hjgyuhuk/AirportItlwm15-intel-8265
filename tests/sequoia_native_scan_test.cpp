#include <Airport/SequoiaNativeScan.h>
#include <HAL/ItlNativeScan.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <functional>

using namespace SequoiaWCL;

static void prepare(NativeScan &scan, bool active = false)
{
    const Channel channels[] = {{1, 1, Width20MHz | Band2GHz | ActiveScan}, {1, 36, Width20MHz | Band5GHz | ActiveScan}};
    const uint8_t mac[] = {0x88,1,2,3,4,5}, entropy[] = {2,2,2,2,2,2};
    ScanRequest request = {};
    request.version = 1; request.scanType = active ? 1 : 2; request.privateMAC = 1; request.activeDwell = 10;
    request.passiveDwell = 110;
    assert(scan.plan.prepare(&request, sizeof(request), channels, 2, mac, entropy, 6, false) == ScanPlanStatus::Success);
}

static void frames(NativeScan &scan)
{
    uint8_t phy[72] = {}; phy[22] = 1; phy[28] = 55;
    uint8_t mpdu[46] = {}; mpdu[0] = 38; mpdu[4] = 0x80; mpdu[20] = 2; mpdu[42] = 3;
    scan.notification(0xc1, mpdu, sizeof(mpdu));
    assert(scan.delivery.pending() == 0);
    scan.notification(0xc0, phy, sizeof(phy));
    mpdu[42] = 0;
    scan.notification(0xc1, mpdu, sizeof(mpdu));
    assert(scan.delivery.pending() == 0);
    mpdu[42] = 3;
    scan.notification(0xc1, mpdu, sizeof(mpdu));
    assert(scan.delivery.pending() == 1);
    assert(scan.diagnostics.beaconsDecoded >= 1);
    assert(scan.diagnostics.beaconsWithSSID <= scan.diagnostics.beaconsDecoded);
    assert(scan.diagnostics.beaconsQueuedWithSSID <= scan.diagnostics.beaconsWithSSID);
    assert(scan.diagnostics.lastBeaconSSIDLength <= 32);
    assert(scan.diagnostics.lastBeaconFlags == 0x4000);
    phy[22] = 44;
    scan.notification(0xc0, phy, sizeof(phy));
    scan.notification(0xc1, mpdu, sizeof(mpdu));
    assert(scan.delivery.pending() == 1);
}

static void complete(NativeScan &scan, bool full = true, unsigned status = 1)
{
    uint8_t iteration[28] = {};
    iteration[0] = full ? 2 : 1; iteration[1] = 1;
    iteration[12] = 1; iteration[13] = 1; iteration[20] = 36;
    scan.notification(0xe7, iteration, sizeof(iteration));
    assert(!scan.delivery.drained());
    uint8_t terminal[12] = {}; terminal[2] = status;
    scan.notification(0x6d, terminal, sizeof(terminal));
    assert(scan.delivery.drained());
}

static void sessionTests()
{
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        ScanDelivery::Slot slots[2];
        NativeScan scan;
        assert(scan.delivery.setStorage(slots, 2));
        for (unsigned repeat = 0; repeat < 100; ++repeat) {
            prepare(scan);
            assert(scan.begin() && !scan.begin());
            assert(!scan.delivery.setStorage(slots, 2));
            assert(!scan.delivery.releaseStorage());
            if (scenario == 9) {
                scan.cancel();
                scan.workerFinished(); // Canceled before the queued worker ran.
            } else {
                if (scenario != 1) scan.submitted(ItlScan::Submission::Accepted);
                frames(scan);
                if (scenario == 2) scan.cancel();
                if (scenario == 3) scan.failed(ItlScan::Outcome::TimedOut);
                if (scenario == 4) scan.failed(ItlScan::Outcome::TransportError);
                if (scenario != 5) complete(scan, scenario != 6, scenario == 7 ? 9 : 1);
                if (scenario == 1) scan.submitted(ItlScan::Submission::Accepted);
                scan.hardwareStopped();
                assert(!scan.begin()); // Hardware drain does not release the worker.
                scan.workerFinished();
            }
            unsigned messages = 0;
            uint32_t finalStatus = UINT32_MAX;
            auto post = [&](uint32_t kind, const void *data, size_t size) {
                ++messages;
                if (kind == kScanCompleteMessage) { assert(size == 4); memcpy(&finalStatus, data, 4); }
                else assert(kind == kScanResultMessage && finalStatus == UINT32_MAX);
                return true;
            };
            const auto pending = scan.delivery.pending();
            assert(scan.delivery.deliverOne([](uint32_t, const void *, size_t) { return false; },
                NativeScan::failureStatus) == ScanDelivery::Delivery::Retry);
            assert(scan.delivery.pending() == pending && scan.delivery.busy());
            while (scan.delivery.busy()) scan.delivery.deliverOne(post, NativeScan::failureStatus);
            const bool success = scenario == 0 || scenario == 1 || scenario == 8;
            assert(finalStatus == (success ? 0u : 0xe3ff8100u));
            assert(messages == (scenario == 9 ? 1u : 2u));
            assert(!scan.workerActive() && scan.delivery.drained());
        }
        assert(scan.delivery.releaseStorage());
        assert(!scan.begin());
        assert(!scan.delivery.releaseStorage());
    }
}

#define __packed __attribute__((packed))
#define ETHER_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
#define IEEE80211_ADDR_LEN 6
#define htole16(value) (value)
#define htole32(value) (value)
#define le32toh(value) (value)
using bus_addr_t = uint64_t;
struct ieee80211_frame { uint8_t bytes[24]; };
enum { EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_VI, EDCA_AC_VO };
static uint32_t le32_to_cpup(const uint32_t *p) { uint32_t value; memcpy(&value, p, 4); return value; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wunused-function"
#include "../itlwm/hal_iwm/if_iwmreg.h"
#pragma clang diagnostic pop
#define IWM_AUX_STA_ID 1
#include "../itlwm/hal_iwm/scan_request_sequoia.inc"
#define nitems(array) (sizeof(array) / sizeof((array)[0]))
#define isset(array, bit) ((array)[(bit) / 8] & (1u << ((bit) % 8)))
enum { IWM_UCODE_TYPE_REGULAR = 0, IWM_DEVICE_FAMILY_7000 = 1, IWM_DEVICE_FAMILY_9000 = 9000,
       IFF_RUNNING = 1, IEEE80211_S_INIT = 0,
       IEEE80211_S_RUN = 4,
       IWM_FLAG_SCANNING = 1, IWM_FLAG_BGSCAN = 2, IWM_FLAG_RFKILL = 4,
       IEEE80211_CHAN_PASSIVE = 1, IEEE80211_CHAN_DFS = 2 };
struct iwm_host_cmd { unsigned id = 0; uint16_t len[2] = {}; const void *data[2] = {}; };
struct iwm_softc {
    int sc_device_family = IWM_DEVICE_FAMILY_7000, sc_mqrx_supported = 0;
    int sc_uc_current = IWM_UCODE_TYPE_REGULAR, cmdqid = IWM_DQA_CMD_QUEUE;
    unsigned sc_flags = 0, sc_capa_n_scan_channels = 40;
    uint8_t sc_enabled_capa[128] = {};
    uint8_t sc_ucode_api[128] = {};
    struct {
        int ic_state = IEEE80211_S_INIT;
        int ic_nnodes = 0;
        void *ic_bss = nullptr;
        uint8_t ic_chan_avail[32] = {};
        struct { unsigned ic_freq = 0, ic_flags = 0; } ic_channels[256];
        struct { struct { unsigned if_flags = IFF_RUNNING; } ac_if; } ic_ac;
    } sc_ic;
};
struct Loop {
    bool gated = true, on = false;
    bool inGate() { return gated; }
    bool onThread() { return on; }
    void commandWakeup(void *, bool) { assert(gated); }
};

class ItlIwm {

    // A channel without a calibration group cannot be scanned.
    uint16_t iwm_channel_id_to_papd(uint16_t ch_id)
    {
        if (ch_id <= 14) return 0;
        if (ch_id <= 64) return 1;
        if (ch_id <= 140) return 2;
        return 3;
    }
    bool iwm_channel_is_valid(struct iwm_softc *sc, uint16_t ch_id)
    {
        if (sc && ch_id < 256 && sc->sc_ic.ic_channels[ch_id].ic_freq &&
            sc->sc_ic.ic_channels[ch_id].ic_flags) return true;
        return ch_id <= 14 || (36 <= ch_id && ch_id <= 64 && ch_id % 4 == 0) ||
            (100 <= ch_id && ch_id <= 140 && ch_id % 4 == 0) ||
            (145 <= ch_id && ch_id <= 165 && ch_id % 4 == 1);
    }
public:
    iwm_softc com;
    Loop loop;
    bool nativeDetached = false, nativeNet80211Attached = true, commandTransportReady = true;
    bool busy = false, trial = false;
    ItlScan::NativeSink nativeScanSink;
    const ScanPlan *nativeScanPlan = nullptr;
    ItlScan::LegacyPhy nativeScanCachePhy;
    ItlScan::CacheDiagnostic nativeCacheDiagnostic;
    SequoiaWCL::ScanPlan nativeScanUsablePlan;
    uint32_t nativeScanFailStage = 0;
    uint32_t nativeScanFailChannel = 0;
    uint32_t nativeScanChannelCount = 0;
    uint16_t nativeScanChannelList[64] = {};
    bool nativeScanPassthrough = false;
    int nativeScanDone = 0, nativeScanCancel = 0;
    unsigned resets = 0, submits = 0, aborts = 0;
    int sendError = 0, waitError = 0;
    std::function<void()> onSubmit, onWait;
    Loop *getMainWorkLoop() { return &loop; }
    Loop *getMainCommandGate() { return &loop; }
    bool isHardwareTrial() { return trial; }
    struct FirmwareLease {
        ItlIwm *owner; bool held;
        explicit FirmwareLease(ItlIwm *d) : owner(d), held(!d->busy && !d->nativeDetached) { if (held) d->busy = true; }
        ~FirmwareLease() { if (held) owner->busy = false; }
        explicit operator bool() const { return held; }
    };
    int iwm_is_valid_channel(unsigned channel) { return channel && channel <= 196 && channel != 144; }
    uint8_t iwm_fw_valid_tx_ant(iwm_softc *) { return 3; }
    uint8_t iwm_fw_valid_rx_ant(iwm_softc *) { return 3; }
    int iwm_send_cmd(iwm_softc *, iwm_host_cmd *command) {
        assert(busy && nativeScanSink.valid());
        assert(command->id == IWM_SCAN_OFFLOAD_REQUEST_CMD && command->data[0] && command->len[0]);
        ++submits;
        if (onSubmit) onSubmit();
        return sendError;
    }
    int iwm_wait_status(const int *, int, void *, unsigned seconds, const int *failure = nullptr) {
        assert(seconds == 30 || seconds == 3);
        if (onWait) onWait();
        if (failure && *failure) return *failure;
        return nativeScanDone ? 0 : waitError;
    }
    int iwm_lmac_scan_abort(iwm_softc *) { ++aborts; return 0; }
    void iwm_stop_device(iwm_softc *) {
        ++resets;
        commandTransportReady = false;
        if (nativeScanSink.valid()) nativeScanSink.stopped(nativeScanSink.context);
    }
    int iwm_native_scan(const ScanPlan &, const ItlScan::NativeSink &);
    void iwm_native_scan_cancel();
    bool iwm_native_scan_rx(uint16_t, const void *, size_t);
    void iwm_native_scan_cache_rx(uint16_t, const void *, size_t) {}
};
static unsigned allocations = 0;
static bool failAllocation = false;
static void *testAllocate(size_t bytes, int, int) {
    if (failAllocation) return nullptr;
    ++allocations; return std::malloc(bytes);
}
static void testFree(void *p) { assert(allocations); --allocations; std::free(p); }
#define malloc(size, type, flags) testAllocate(size, 0, 0)
#define free(pointer) testFree(pointer)
#include "../itlwm/hal_iwm/native_scan_sequoia.inc"
#undef malloc
#undef free

static void transportTests()
{
    for (unsigned fault = 0; fault < 13; ++fault) {
        NativeScan scan;
        ScanDelivery::Slot slots[2];
        assert(scan.delivery.setStorage(slots, 2)); prepare(scan, fault >= 9); assert(scan.begin());
        ItlIwm driver;
        for (auto channel : {1, 36}) {
            driver.com.sc_ic.ic_channels[channel].ic_freq = 1;
            driver.com.sc_ic.ic_chan_avail[channel / 8] |= 1u << (channel % 8);
        }
        ItlScan::NativeSink sink;
        sink.context = &scan;
        sink.submitted = [](void *p, ItlScan::Submission s) { static_cast<NativeScan *>(p)->submitted(s); };
        sink.notification = [](void *p, uint16_t c, const void *b, size_t s) { static_cast<NativeScan *>(p)->notification(c,b,s); };
        sink.failed = [](void *p, ItlScan::Outcome o) { static_cast<NativeScan *>(p)->failed(o); };
        sink.stopped = [](void *p) { static_cast<NativeScan *>(p)->hardwareStopped(); };
        auto notify = [&] {
            uint8_t phy[72] = {}; phy[22] = 1;
            uint8_t mpdu[46] = {}; mpdu[0] = 38; mpdu[4] = 0x80; mpdu[20] = 2; mpdu[42] = 3;
            uint8_t iteration[28] = {}; iteration[0] = 2; iteration[1] = 1;
            iteration[12] = 1; iteration[13] = 1; iteration[20] = 36;
            uint8_t terminal[12] = {}; terminal[2] = 1;
            assert(driver.iwm_native_scan_rx(0xc0, phy, sizeof(phy)));
            assert(driver.iwm_native_scan_rx(0xc1, mpdu, sizeof(mpdu)));
            assert(driver.iwm_native_scan_rx(0xe7, iteration, sizeof(iteration)));
            assert(!driver.nativeScanDone);
            assert(driver.iwm_native_scan_rx(0x6d, terminal, sizeof(terminal)));
            assert(driver.nativeScanDone);
        };
        if (fault == 0) driver.onSubmit = notify;
        if (fault == 1) driver.onWait = notify;
        if (fault == 2) driver.sendError = ETIMEDOUT;
        if (fault == 3) driver.waitError = ETIMEDOUT;
        if (fault == 4) driver.onWait = [&] { driver.iwm_native_scan_cancel(); };
        if (fault == 5) failAllocation = true;
        if (fault == 6) driver.commandTransportReady = false;
        if (fault == 7) driver.busy = true;
        if (fault == 8) driver.onWait = [&] {
            driver.nativeDetached = true; driver.iwm_stop_device(&driver.com); driver.waitError = ENXIO;
        };
        if (fault == 9) driver.onWait = notify;
        if (fault == 10) driver.com.sc_ic.ic_channels[1].ic_flags |= IEEE80211_CHAN_PASSIVE;
        if (fault == 11) driver.com.sc_ic.ic_channels[36].ic_flags |= IEEE80211_CHAN_DFS;
        if (fault == 12) driver.waitError = ETIMEDOUT;
        const int error = driver.iwm_native_scan(scan.plan, sink);
        failAllocation = false;
        const bool success = fault < 2 || fault == 9;
        assert((error == 0) == success);
        scan.workerFinished();
        assert(!driver.nativeScanSink.valid() && !driver.nativeScanPlan && !allocations);
        assert(driver.resets == ((fault >= 5 && fault <= 7) || fault == 10 || fault == 11 ? 0u : 1u));
        assert(driver.aborts == (fault == 3 || fault == 4 || fault == 12 ? 1u : 0u));
        assert(scan.delivery.drained());
        uint32_t finalStatus = UINT32_MAX;
        while (scan.delivery.busy()) scan.delivery.deliverOne([&](uint32_t kind, const void *p, size_t) {
            if (kind == kScanCompleteMessage) memcpy(&finalStatus, p, 4);
            return true;
        }, NativeScan::failureStatus);
        assert(finalStatus == (success ? 0u : 0xe3ff8100u));
    }
}

static void associatedTests()
{
    // r225 associated bgscan admission on the HAL boundary: RUN + BSS admits,
    // sets BGSCAN (never SCANNING), leaves the device and IFF_RUNNING intact,
    // and passes PHY/MPDU RX back to the legacy data path (return false) while
    // still consuming the offload-complete terminal itself.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        ScanDelivery::Slot slots[2];
        NativeScan scan;
        assert(scan.delivery.setStorage(slots, 2));
        prepare(scan);
        scan.plan.markAssociatedScan(1);
        assert(scan.plan.associatedScan());
        assert(scan.begin());
        ItlIwm driver;
        for (auto channel : {1, 36}) {
            driver.com.sc_ic.ic_channels[channel].ic_freq = 1;
            driver.com.sc_ic.ic_chan_avail[channel / 8] |= 1u << (channel % 8);
        }
        int node = 0;
        driver.com.sc_ic.ic_state = IEEE80211_S_RUN;
        driver.com.sc_ic.ic_bss = &node;
        driver.com.sc_ucode_api[IWM_UCODE_TLV_API_FRAGMENTED_SCAN / 8] |=
            1u << (IWM_UCODE_TLV_API_FRAGMENTED_SCAN % 8);
        if (scenario == 1) driver.com.sc_ic.ic_state = IEEE80211_S_INIT;
        if (scenario == 2) driver.com.sc_ic.ic_bss = nullptr;
        if (scenario == 0) driver.onWait = [&] {
            assert(driver.nativeScanPassthrough);
            assert(driver.com.sc_flags & IWM_FLAG_BGSCAN);
            assert(!(driver.com.sc_flags & IWM_FLAG_SCANNING));
            uint8_t phy[72] = {}; phy[22] = 1;
            uint8_t mpdu[46] = {}; mpdu[0] = 38;
            assert(!driver.iwm_native_scan_rx(0xc0, phy, sizeof(phy)));
            assert(!driver.iwm_native_scan_rx(0xc1, mpdu, sizeof(mpdu)));
            assert(driver.nativeCacheDiagnostic.passthroughFrames == 2);
            uint8_t terminal[12] = {}; terminal[2] = 1;
            assert(driver.iwm_native_scan_rx(0x6d, terminal, sizeof(terminal)));
            assert(driver.nativeScanDone);
        };
        ItlScan::NativeSink sink;
        sink.context = &scan;
        sink.submitted = [](void *p, ItlScan::Submission s) { static_cast<NativeScan *>(p)->submitted(s); };
        sink.notification = [](void *p, uint16_t c, const void *b, size_t s) { static_cast<NativeScan *>(p)->notification(c, b, s); };
        sink.failed = [](void *p, ItlScan::Outcome o) { static_cast<NativeScan *>(p)->failed(o); };
        sink.stopped = [](void *p) { static_cast<NativeScan *>(p)->hardwareStopped(); };
        const int error = driver.iwm_native_scan(scan.plan, sink);
        const bool success = scenario == 0;
        assert((error == 0) == success);
        if (!success) assert(error == EOPNOTSUPP);
        scan.workerFinished();
        assert(!driver.nativeScanSink.valid() && !driver.nativeScanPlan && !allocations);
        assert(!driver.nativeScanPassthrough);
        assert(!(driver.com.sc_flags & (IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN)));
        if (success) {
            assert(driver.resets == 0 && driver.aborts == 0);
            assert(driver.com.sc_ic.ic_ac.ac_if.if_flags & IFF_RUNNING);
            assert(driver.nativeCacheDiagnostic.assocScanFragmented == 1);
            assert(driver.nativeCacheDiagnostic.passthroughFrames == 2);
        } else {
            assert(driver.resets == 0 && driver.aborts == 0);
        }
    }
}

static void ax210Channel144()
{
    NativeScan scan;
    ScanDelivery::Slot slots[2];
    assert(scan.delivery.setStorage(slots, 2));
    const Channel channel = {1, 144, Width20MHz | Band5GHz | DFS};
    ScanRequest request = {};
    request.version = 1; request.scanType = 2; request.passiveDwell = 110;
    uint8_t mac[6] = {2,1,2,3,4,5};
    assert(scan.plan.prepare(&request, sizeof(request), &channel, 1, mac, nullptr, 0, false) == ScanPlanStatus::Success);
    assert(scan.begin());
    scan.submitted(ItlScan::Submission::Accepted);
    uint8_t phy[72] = {}; phy[22] = 144; phy[28] = 55;
    uint8_t mpdu[46] = {}; mpdu[0] = 38; mpdu[4] = 0x80; mpdu[20] = 2; mpdu[42] = 3;
    scan.notification(0xc0, phy, sizeof(phy));
    scan.notification(0xc1, mpdu, sizeof(mpdu));
    assert(scan.delivery.pending() == 1 && scan.diagnostics.beaconsQueued == 1);
    phy[22] = 140; // Well-formed but outside the plan: never expose it to WCL.
    scan.notification(0xc0, phy, sizeof(phy));
    scan.notification(0xc1, mpdu, sizeof(mpdu));
    assert(scan.delivery.pending() == 1 && scan.diagnostics.beaconsFiltered == 1);
    uint8_t iteration[20] = {}; iteration[0] = 1; iteration[1] = 1;
    iteration[12] = 144; iteration[13] = 0; // Intel band: 5 GHz = 0.
    scan.notification(0xe7, iteration, sizeof(iteration));
    uint8_t terminal[12] = {}; terminal[2] = 1;
    scan.notification(0x6d, terminal, sizeof(terminal));
    scan.hardwareStopped(); scan.workerFinished();
    unsigned results = 0, completions = 0;
    auto post = [&](uint32_t kind, const void *data, size_t size) {
        if (kind == kScanCompleteMessage) {
            uint32_t status = UINT32_MAX; assert(size == sizeof(status)); memcpy(&status, data, size);
            assert(status == 0); ++completions;
        } else {
            assert(kind == kScanResultMessage && size >= sizeof(BeaconMetadata));
            BeaconMetadata metadata; memcpy(&metadata, data, sizeof(metadata));
            assert(metadata.channelSpec == 0xd090); ++results;
        }
        return true;
    };
    while (scan.delivery.busy()) scan.delivery.deliverOne(post, NativeScan::failureStatus);
    assert(results == 1 && completions == 1);
}

static void associatedCompletionCoverage()
{
    for (bool requireDiversity : {false, true}) for (bool full : {false, true}) {
        NativeScan scan;
        ScanDelivery::Slot slots[2];
        assert(scan.delivery.setStorage(slots, 2));
        prepare(scan); scan.plan.markAssociatedScan(1); assert(scan.begin());
        scan.submitted(ItlScan::Submission::Accepted);
        frames(scan); // One beacon, deliberately below the old IWM heuristic.
        complete(scan, full);
        scan.finishAssociatedScan(requireDiversity);
        scan.workerFinished();
        uint32_t status = UINT32_MAX;
        while (scan.delivery.busy()) scan.delivery.deliverOne([&](uint32_t kind, const void *data, size_t) {
            if (kind == kScanCompleteMessage) memcpy(&status, data, sizeof(status));
            return true;
        }, NativeScan::failureStatus);
        assert((status == 0) == (full && !requireDiversity));
        assert(scan.diagnostics.assocWithheld == (full && requireDiversity ? 1u : 0u));
    }
}

int main()
{
    associatedCompletionCoverage();
    ax210Channel144();
    sessionTests();
    transportTests();
    associatedTests();
    puts("Native WCL scan: 1,000 session traces, 13 production HAL worker/regulatory scenarios and associated bgscan admission passed");
}
