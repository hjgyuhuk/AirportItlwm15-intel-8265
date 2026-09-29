#include <Airport/SequoiaNativeScan.h>
#include <Airport/SequoiaSecurity.h>
#include <HAL/ItlNativeScan.h>
#include <HAL/IwmOperationOwner.h>
#include <HAL/ItlBootProgress.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define XYLog(...) ((void)0)
static ItlBootProgress gItlwmHalProgress;
#define ITLWM_IWX_DETAIL(key, value) gItlwmHalProgress.iwxRecord(ItlBootProgress::key, (value))
#define ITLWM_IWX_BUMP(key) gItlwmHalProgress.iwxBump(ItlBootProgress::key)
#define ITLWM_HAL_STAGE(label) gItlwmHalProgress.record(label)
#define ITLWM_HAL_RESULT(value) gItlwmHalProgress.recordResult(value)
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define ic_if ic_ac.ac_if
using IOReturn = int;
using AbsoluteTime = uint64_t;
enum { kIOReturnSuccess = 0, kIOReturnError = 1000, THREAD_AWAKENED = 0,
    THREAD_TIMED_OUT = 1, THREAD_INTERRUPTED = 2, THREAD_INTERRUPTIBLE = 3, THREAD_UNINT = 4,
    kSecondScale = 1000000000, IEEE80211_NWID_LEN = 32, IEEE80211_S_INIT = 0, IEEE80211_S_RUN = 4,
    IFF_RUNNING = 1, IWX_FLAG_SCANNING = 2, IWX_FLAG_BGSCAN = 4,
    IWX_FLAG_RFKILL = 8, IWX_FLAG_SHUTDOWN = 16, IWX_FLAG_USE_ICT = 32,
    IWX_CSR_RESET = 1, IWX_CSR_RESET_REG_FLAG_SW_RESET = 2,
    IWX_CSR_GP_CNTRL = 3, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ = 4,
    IWX_SCAN_COMPLETE_UMAC = 0xb9, IWX_SCAN_ITERATION_COMPLETE_UMAC = 0xb5, PCATCH = 0,
    IWX_UCODE_TYPE_REGULAR = 0, IWX_DEVICE_FAMILY_AX210 = 210 };
static uint64_t clockNow = 1000000000;
static uint64_t airport_up_time() { return clockNow / 1000000; }
static void clock_interval_to_deadline(unsigned amount, unsigned scale, AbsoluteTime *deadline) {
    *deadline = clockNow + static_cast<uint64_t>(amount) * scale;
}
static uint64_t SEC_TO_NSEC(unsigned seconds) { return static_cast<uint64_t>(seconds) * 1000000000; }
struct OSObject { virtual ~OSObject() = default; virtual void release() {} };
struct Loop {
    bool gated = false, on = false;
    unsigned sleeps = 0, wakeups = 0;
    std::vector<AbsoluteTime> deadlines;
    std::function<int(void *, AbsoluteTime)> onSleep;
    bool inGate() { return gated; }
    bool onThread() { return on; }
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *object, void *arg = nullptr) {
        bool old = gated; gated = true;
        auto result = action(object, arg, nullptr, nullptr, nullptr);
        gated = old; return result;
    }
    int commandSleep(void *event, AbsoluteTime deadline, int) {
        assert(gated && !on); ++sleeps; deadlines.push_back(deadline);
        return onSleep ? onSleep(event, deadline) : THREAD_TIMED_OUT;
    }
    int commandSleep(void *event, int flags) { return commandSleep(event, 0, flags); }
    void commandWakeup(void *, bool) { assert(gated); ++wakeups; }
    void removeEventSource(OSObject *) { assert(gated); }
};
struct Interrupt : OSObject { void disable() {} };
struct Dma { bool live = false; };
struct Taskq { bool quiesced = false; };
static Taskq systemQueue;
static Taskq *systq = &systemQueue;
static void taskq_quiesce(Taskq *queue) { if (queue) queue->quiesced = true; }
static void taskq_destroy(Taskq *queue) { if (queue) assert(queue->quiesced); }
struct Timer { bool pending = true; };
static void timeout_del(Timer **timer) { if (*timer) (*timer)->pending = false; }
struct IOPCIDevice : OSObject {
    bool busmaster = true;
    void setBusMasterEnable(bool value) { busmaster = value; }
};
enum { IWX_FLAG_HW_ERR = 64, IEEE80211_M_STA = 1, LINK_STATE_UP = 1, LINK_STATE_DOWN = 0,
    IEEE80211_F_WEPON = 1, IEEE80211_F_RSNON = 2, IEEE80211_F_PSK = 4,
    IEEE80211_CIPHER_NONE = 0, IEEE80211_CIPHER_CCMP = 3, IEEE80211_CIPHER_TKIP = 2, IEEE80211_CIPHER_BIP = 4,
    IEEE80211_AKM_SHA256_PSK = 4, IEEE80211_NODE_MFP = 128,
    IEEE80211_NODE_RXMGMTPROT = 32, IEEE80211_NODE_TXMGMTPROT = 64, IEEE80211_KEY_IGTK = 8,
    IEEE80211_NODE_TXRXPROT = 3, IEEE80211_PROTO_RSN = 2, IEEE80211_AKM_PSK = 2,
    IEEE80211_KEY_SWCRYPTO = 1, IEEE80211_KEY_GROUP = 2, IEEE80211_KEY_TX = 4 };
struct ieee80211_key { unsigned k_cipher = 0, k_len = 0, k_id = 0, k_flags = 0; void *k_priv = nullptr; };
struct ieee80211_node {
    uint8_t ni_bssid[6] = {2,1,2,3,4,5};
    unsigned ni_port_valid = 0, ni_flags = 0, ni_rsnprotos = 0, ni_rsnakms = 0,
        ni_rsncipher = 0, ni_rsngroupcipher = 0, ni_rsngroupmgmtcipher = 0;
    ieee80211_key ni_pairwise_key;
};
struct _ifnet { unsigned if_flags = IFF_RUNNING, if_link_state = LINK_STATE_UP; };
struct ieee80211com {
    ieee80211_node *ic_bss = nullptr;
    unsigned ic_opmode = IEEE80211_M_STA, ic_flags = 0;
    ieee80211_key ic_nw_keys[6];
    SequoiaWCL::TkipCountermeasures ic_native_tkip;
    unsigned ic_nnodes = 0, ic_mgt_timer = 0;
    struct { _ifnet ac_if; } ic_ac;
    int ic_state = IEEE80211_S_INIT, ic_des_esslen = 3;
    uint8_t ic_des_essid[32] = {'o', 'l', 'd'};
    Timer *ic_bgscan_timeout = nullptr;
};
struct iwx_tx_ring { Dma payload; };
struct iwx_rx_ring { Dma memory; };
struct iwx_rxba_data {
    Timer *session_timer = nullptr;
    struct { Timer *reorder_timer = nullptr; } reorder_buf;
};
struct iwx_fw_sects {};
struct iwx_fw_info { iwx_fw_sects fw_sects[1]; };
struct iwx_softc {
    ieee80211com sc_ic;
    unsigned sc_flags = 0;
    int sc_tx_timer = 0;
    int sc_capa_n_scan_channels = 40, sc_generation = 1, sc_nic_locks = 0;
    struct { unsigned cfg_phy_cnt = 0; } sc_last_phy_info;
    Interrupt *sc_ih = nullptr;
    Taskq *sc_nswq = nullptr;
    void *sc_sh = nullptr;
    OSObject *sc_st = nullptr;
    iwx_rxba_data sc_rxba_data[2];
    iwx_tx_ring txq[2];
    iwx_rx_ring rxq;
    Dma ict_dma, ctxt_info_dma, prph_info_dma, prph_scratch_dma, iml_dma, pnvm_dram;
    Dma firmware_image, paging;
    iwx_fw_info sc_fw;
    int sc_device_family = IWX_DEVICE_FAMILY_AX210;
    struct { bool uc_intr = false, uc_ok = false; } sc_uc;
};
struct pci_attach_args { IOPCIDevice *pa_tag = nullptr; Loop *workloop = nullptr; };
struct iwx_scan_results_notif { uint8_t channel, band, probe_status, num_probe_not_sent; uint32_t duration; };
struct iwx_umac_scan_complete { uint32_t uid; uint8_t schedule, iter, status, ebs; uint32_t time, reserved; };
struct iwx_umac_scan_iter_complete_notif {
    uint32_t uid; uint8_t count, status, bt, last; uint32_t low, high;
    iwx_scan_results_notif results[0];
};
class ItlIwx;
static ItlIwx *currentDriver;
static void regWrite(iwx_softc *, unsigned reg, unsigned value);
static void delay(unsigned micros);
static void ieee80211_ifdetach(_ifnet *);
#define IWX_WRITE(sc, reg, value) regWrite(sc, reg, value)
#define IWX_SETBITS(sc, reg, value) regWrite(sc, reg, value)
#define IWX_CLRBITS(sc, reg, value) ((void)0)
#define DELAY(us) delay(us)

class ItlIwx : public OSObject {
public:
    Loop loop;
    iwx_softc com;
    pci_attach_args pci;
    IOPCIDevice device;
    OSObject mapping;
    Interrupt interrupt;
    Taskq stateQueue;
    Timer timers[3];
    bool nativeDetached = false, nativeCommandTransport = true, nativeJoinMode = false;
    bool nativeNet80211Attached = true, nativeSystemQueue = false;
    int nativeError = 0, nativeScanDone = 0, nativeScanCancel = 0;
    bool nativeWatchdogActive = false;
    uint64_t nativeWatchdogLastMs = 0;
    uint64_t nativeStateCompleted = 0, nativeStateSerial = 0;
    ieee80211_node peer;
    struct Progress {
        struct Association { unsigned status = 0; } association;
        bool ownsPeer(const uint8_t *, unsigned generation) const { return generation == 1; }
        const Association *assoc() const { return &association; }
    } nativeJoinProgress; // Only peer-event identity is mocked; port policy is production.
    bool nativeScanWildcard = false;
    bool nativeScanDataAllowed = false;
    int nativeScanDataGeneration = -1;
    uint32_t nativeTxRateFlags = 0;
    int nativeTxRateGeneration = -1;
    ItlScan::LegacyPhy nativeScanCachePhy;
    struct { unsigned nodesAtScanEnd = 0; } nativeCacheDiagnostic;
    ItlScan::NativeSink nativeScanSink;
    const SequoiaWCL::ScanPlan *nativeScanPlan = nullptr;
    uint8_t nativeScanNotifScratch[336] = {};
    IwmOperationOwner nativeFirmwareOwner;
    uintptr_t thread = 1;
    bool resetPosted = false, resetComplete = false, attachResult = true;
    unsigned frees = 0, stops = 0, aborts = 0, submissions = 0, ifdetaches = 0, releases = 0;
    std::function<void()> onSubmit, onAbort;
    bool nativePortReady() const;
    bool iwx_native_port_ready();
    bool iwx_native_data_operation_ready() const;
    int iwx_native_watchdog(uint64_t);
    int iwx_native_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *);
    unsigned softwareKeys = 0;
    int iwx_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *) { return EIO; }
    int ieee80211_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *key) {
        // Software crypto boundary only; no firmware command or gate release.
        assert(loop.inGate()); ++softwareKeys;
        key->k_flags |= IEEE80211_KEY_SWCRYPTO; key->k_priv = this; return 0;
    }
    int background = -1;
    int sendError = 0;
    bool imageConsumed = false;
    ItlIwx() { currentDriver = this; }
    Loop *getMainWorkLoop() { return &loop; }
    Loop *getMainCommandGate() { return &loop; }
    class FirmwareLease {
        ItlIwx *driver; uintptr_t owner; bool acquired;
    public:
        explicit FirmwareLease(ItlIwx *d) : driver(d), owner(d->thread),
            acquired(d->loop.inGate() && !d->loop.onThread() && !d->nativeDetached &&
                !(d->com.sc_flags & IWX_FLAG_SHUTDOWN) && d->nativeFirmwareOwner.acquire(owner)) {}
        ~FirmwareLease() { if (acquired) assert(driver->nativeFirmwareOwner.release(owner)); }
        explicit operator bool() const { return acquired; }
    };
    bool attach(IOPCIDevice *);
    void detach(IOPCIDevice *);
    void iwx_stop_device(iwx_softc *);
    int nativeScan(const SequoiaWCL::ScanPlan &, const ItlScan::NativeSink &);
    void cancelNativeScan();
    void iwx_native_scan_notification(iwx_softc *, uint16_t, const void *, size_t);
    int iwx_load_firmware(iwx_softc *);
    void wakeupOn(void *event) { loop.commandWakeup(event, false); }
    int tsleep_nsec(void *event, int, const char *, int ns) {
        if (ns < 0) return EWOULDBLOCK;
        auto result = loop.commandSleep(event, clockNow + ns, THREAD_INTERRUPTIBLE);
        return result == THREAD_TIMED_OUT ? ETIMEDOUT : 0;
    }
    int iwx_umac_scan(iwx_softc *, int bg) { background = bg; ++submissions; if (onSubmit) onSubmit(); return sendError; }
    int iwx_umac_scan_abort(iwx_softc *) { ++aborts; if (onAbort) onAbort(); return 0; }
    void iwx_native_reset_runtime() {}
    void iwx_disable_interrupts(iwx_softc *) { assert(loop.inGate()); }
    void iwx_disable_rx_dma(iwx_softc *) {} // Model a failed idle poll: DMA remains live.
    void iwx_apm_stop(iwx_softc *) {} // Model a failed master-disable poll too.
    void iwx_conf_msix_hw(iwx_softc *, int) {}
    void iwx_enable_rfkill_int(iwx_softc *) {}
    void iwx_check_rfkill(iwx_softc *) {}
    void iwx_prepare_card_hw(iwx_softc *) {}
    void reclaim(Dma &dma) {
        if (!dma.live) return;
        assert(loop.inGate() && (resetComplete || (imageConsumed &&
            (&dma == &com.firmware_image || &dma == &com.iml_dma))));
        dma.live = false; ++frees;
    }
    void iwx_reset_rx_ring(iwx_softc *, iwx_rx_ring *) { assert(resetComplete); }
    void iwx_reset_tx_ring(iwx_softc *, iwx_tx_ring *ring) { reclaim(ring->payload); }
    void iwx_free_rx_ring(iwx_softc *, iwx_rx_ring *ring) { reclaim(ring->memory); }
    void iwx_free_tx_ring(iwx_softc *, iwx_tx_ring *ring) { reclaim(ring->payload); }
    void iwx_dma_contig_free(Dma *dma) { reclaim(*dma); }
    void iwx_ctxt_info_free_fw_img(iwx_softc *sc) { reclaim(sc->firmware_image); }
    void iwx_ctxt_info_free_paging(iwx_softc *sc) { reclaim(sc->paging); }
    void iwx_fw_info_free(iwx_fw_info *) { assert(!nativeFirmwareOwner.busy()); }
    void iwx_pnvm_free(iwx_fw_info *) { assert(!nativeFirmwareOwner.busy()); }
    int iwx_nic_lock(iwx_softc *) { return 0; }
    void iwx_nic_unlock(iwx_softc *) {}
    int iwx_ctxt_info_gen3_init(iwx_softc *sc, iwx_fw_sects *) {
        sc->firmware_image.live = sc->paging.live = sc->iml_dma.live = true;
        return 0;
    }
    int iwx_ctxt_info_init(iwx_softc *sc, iwx_fw_sects *fw) { return iwx_ctxt_info_gen3_init(sc, fw); }
    void iwx_clear_reorder_buffer(iwx_softc *, iwx_rxba_data *rxba) {
        if (rxba->session_timer) assert(!rxba->session_timer->pending);
        if (rxba->reorder_buf.reorder_timer) assert(!rxba->reorder_buf.reorder_timer->pending);
    }
    bool iwx_attach(iwx_softc *sc, pci_attach_args *) {
        assert(loop.inGate() && !loop.onThread() && nativeFirmwareOwner.owns(thread));
        sc->sc_sh = &mapping; sc->sc_st = &mapping; sc->txq[0].payload.live = true;
        nativeNet80211Attached = attachResult;
        return attachResult;
    }
    void releaseAll() { pci = {}; ++releases; }
    void complete(bool success = true) {
        if (success) {
            uint8_t iteration[24] = {}; iteration[4] = 1; iteration[5] = 1;
            iteration[16] = nativeScanPlan ? nativeScanPlan->channels[0] : 1;
            iteration[17] = iteration[16] <= 14 ? 1 : 0;
            iwx_native_scan_notification(&com, IWX_SCAN_ITERATION_COMPLETE_UMAC, iteration, sizeof(iteration));
        }
        iwx_umac_scan_complete terminal = {}; terminal.status = success ? 1 : 2;
        iwx_native_scan_notification(&com, IWX_SCAN_COMPLETE_UMAC, &terminal, sizeof(terminal));
    }
};
static void regWrite(iwx_softc *, unsigned reg, unsigned value) {
    if (reg == IWX_CSR_RESET && (value & IWX_CSR_RESET_REG_FLAG_SW_RESET)) {
        currentDriver->resetPosted = true; currentDriver->resetComplete = false;
    }
}
static void delay(unsigned micros) {
    if (currentDriver->resetPosted && micros == 5000) {
        currentDriver->resetComplete = true; ++currentDriver->stops;
    }
}
static void ieee80211_ifdetach(_ifnet *) {
    assert(currentDriver->nativeNet80211Attached && currentDriver->resetComplete);
    ++currentDriver->ifdetaches;
}
#include "iwx_native_methods.inc"

struct ScanFixture {
    SequoiaWCL::NativeScan scan;
    SequoiaWCL::ScanDelivery::Slot slots[2];
    ItlScan::NativeSink sink;
    ItlIwx &driver;
    unsigned stopped = 0, posted = 0;
    explicit ScanFixture(ItlIwx &d, unsigned channelNumber = 1) : driver(d) {
        assert(scan.delivery.setStorage(slots, 2));
        SequoiaWCL::ScanRequest request = {}; request.version = 1; request.scanType = 2; request.passiveDwell = 110;
        SequoiaWCL::Channel channel;
        assert(SequoiaWCL::channel20MHz(channelNumber, channelNumber <= 14 ? SequoiaWCL::Band::GHz2 : SequoiaWCL::Band::GHz5, false, channelNumber == 144, channel));
        uint8_t address[6] = {2,1,2,3,4,5}, entropy[6] = {2,6,7,8,9,10};
        assert(scan.plan.prepare(&request, sizeof(request), &channel, 1, address, entropy, 6, false) ==
            SequoiaWCL::ScanPlanStatus::Success);
        sink.context = this;
        sink.submitted = [](void *c, ItlScan::Submission value) { static_cast<ScanFixture *>(c)->scan.submitted(value); };
        sink.failed = [](void *c, ItlScan::Outcome value) { static_cast<ScanFixture *>(c)->scan.failed(value); };
        sink.notification = [](void *c, uint16_t code, const void *data, size_t size) {
            static_cast<ScanFixture *>(c)->scan.notification(code, data, size);
        };
        sink.stopped = [](void *c) {
            auto *self = static_cast<ScanFixture *>(c);
            assert(self->driver.resetComplete); ++self->stopped; self->scan.hardwareStopped();
        };
    }
    int run() {
        assert(scan.begin());
        int result = driver.nativeScan(scan.plan, sink);
        scan.workerFinished();
        auto delivered = scan.delivery.deliverOne([&](uint32_t kind, const void *, size_t) {
            assert(kind == SequoiaWCL::kScanCompleteMessage); ++posted; return true;
        }, SequoiaWCL::NativeScan::failureStatus);
        assert(delivered == SequoiaWCL::ScanDelivery::Delivery::Complete && !scan.delivery.busy());
        return result;
    }
};

static void stopTests() {
    ItlIwx driver;
    driver.loop.gated = true;
    driver.com.txq[0].payload.live = true;
    driver.com.firmware_image.live = true;
    driver.com.paging.live = true;
    driver.nativeTxRateFlags = 0x123;
    driver.nativeTxRateGeneration = driver.com.sc_generation;
    driver.iwx_stop_device(&driver.com);
    assert(driver.resetComplete && driver.frees == 3 && !driver.nativeCommandTransport);
    assert(driver.nativeTxRateFlags == 0 && driver.nativeTxRateGeneration == -1);
}

static void detachTests() {
    for (unsigned reject = 0; reject < 4; ++reject) {
        ItlIwx d;
        d.loop.gated = true;
        d.loop.on = reject == 0;
        d.nativeDetached = reject == 1;
        if (reject == 2) d.pci.pa_tag = &d.device;
        assert(!d.attach(reject == 3 ? nullptr : &d.device));
        const char *expected[] = {"iwx:reject-workloop-thread", "iwx:reject-detached",
                                  "iwx:reject-already-attached", "iwx:reject-null-device"};
        assert(std::string(gItlwmHalProgress.current()) == expected[reject]);
        assert(!d.nativeFirmwareOwner.busy() && !d.frees);
    }
    {
        ItlIwx d;
        d.loop.gated = true; d.com.sc_sh = &d.mapping; d.com.sc_st = &d.mapping;
        d.com.txq[0].payload.live = d.com.rxq.memory.live = true;
        d.detach(&d.device);
        assert(d.resetComplete && !d.device.busmaster && d.frees == 2);
    }
    for (unsigned partial = 0; partial < 4; ++partial) {
        ItlIwx d;
        d.nativeNet80211Attached = partial == 3;
        if (partial) { d.com.sc_sh = &d.mapping; d.com.sc_st = &d.mapping; }
        d.com.txq[0].payload.live = partial >= 2;
        d.com.rxq.memory.live = partial >= 2;
        d.com.sc_ih = &d.interrupt;
        d.nativeSystemQueue = true; d.com.sc_nswq = &d.stateQueue;
        d.com.sc_rxba_data[0].session_timer = &d.timers[0];
        d.com.sc_rxba_data[0].reorder_buf.reorder_timer = &d.timers[1];
        if (d.nativeNet80211Attached) d.com.sc_ic.ic_bgscan_timeout = &d.timers[2];
        assert(d.nativeFirmwareOwner.acquire(2));
        d.loop.onSleep = [&](void *event, AbsoluteTime) {
            assert(event == &d.nativeFirmwareOwner && !d.frees);
            assert(!partial || (d.resetComplete && !d.device.busmaster));
            assert(d.nativeFirmwareOwner.release(2)); return THREAD_AWAKENED;
        };
        d.detach(&d.device);
        assert(d.nativeDetached && !d.nativeFirmwareOwner.busy() && !d.com.sc_nswq && !d.nativeSystemQueue);
        assert(d.ifdetaches == (partial == 3 ? 1u : 0u));
        assert(d.frees == (partial >= 2 ? 2u : 0u));
        const auto freed = d.frees, released = d.releases;
        d.detach(&d.device);
        assert(d.frees == freed && d.releases == released);
    }
    for (bool success : {false, true}) {
        ItlIwx d; d.nativeNet80211Attached = false; d.attachResult = success;
        assert(d.attach(&d.device) == success);
        assert(!d.nativeFirmwareOwner.busy());
        if (success) d.detach(&d.device);
        assert(d.nativeDetached && d.frees == 1);
    }
}

static void firmwareTests() {
    for (auto family : {200, static_cast<int>(IWX_DEVICE_FAMILY_AX210)}) {
        for (unsigned outcome = 0; outcome < 3; ++outcome) {
            ItlIwx d; d.loop.gated = true; d.com.sc_device_family = family;
            d.loop.onSleep = [&](void *, AbsoluteTime) {
                d.com.sc_uc.uc_ok = outcome == 0;
                d.imageConsumed = d.com.sc_uc.uc_ok;
                return outcome == 1 ? THREAD_TIMED_OUT : THREAD_AWAKENED;
            };
            const int expected[] = {0, ETIMEDOUT, EINVAL};
            assert(d.iwx_load_firmware(&d.com) == expected[outcome]);
            assert(d.frees == (outcome == 0 ? 2u : 0u));
            assert(d.com.paging.live && d.com.firmware_image.live == (outcome != 0));
            d.iwx_stop_device(&d.com);
            assert(d.frees == 3);
        }
    }
}

static void associatedScanTests() {
    // The associated scan must preserve a usable data port while its worker
    // sleeps with an exclusive firmware lease. Submission itself remains gated.
    {
        ItlIwx d; ScanFixture f(d, 40); d.loop.gated = true;
        d.com.sc_ic.ic_state = IEEE80211_S_RUN; d.com.sc_ic.ic_bss = &d.peer;
        d.nativeJoinMode = true; f.scan.plan.markAssociatedScan(40);
        d.loop.onSleep = [&](void *, AbsoluteTime) {
            assert(d.nativeFirmwareOwner.busy());
            assert(d.nativePortReady());
            d.complete(); return THREAD_AWAKENED;
        };
        assert(f.run() == 0 && d.nativePortReady());
    }
    for (bool secured : {false, true}) for (unsigned scenario = 0; scenario < 9; ++scenario) {
        ItlIwx d; ScanFixture f(d, 40); d.loop.gated = true;
        d.com.sc_ic.ic_state = IEEE80211_S_RUN;
        d.com.sc_ic.ic_bss = &d.peer;
        d.nativeJoinMode = true;
        if (secured) {
            d.com.sc_ic.ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK;
            d.peer.ni_port_valid = 1; d.peer.ni_flags = IEEE80211_NODE_TXRXPROT;
            d.peer.ni_rsnprotos = IEEE80211_PROTO_RSN; d.peer.ni_rsnakms = IEEE80211_AKM_PSK;
            d.peer.ni_rsncipher = d.peer.ni_rsngroupcipher = IEEE80211_CIPHER_CCMP;
            d.peer.ni_pairwise_key = {IEEE80211_CIPHER_CCMP,16,0,IEEE80211_KEY_SWCRYPTO,&d};
            d.com.sc_ic.ic_nw_keys[1] = {IEEE80211_CIPHER_CCMP,16,1,
                IEEE80211_KEY_SWCRYPTO | IEEE80211_KEY_GROUP,&d};
        }
        assert(d.nativePortReady());
        f.scan.plan.markAssociatedScan(40);
        d.onSubmit = [&] {
            assert(d.background == 1 && d.com.sc_flags == IWX_FLAG_BGSCAN);
            assert(d.nativeFirmwareOwner.busy() && !d.nativePortReady());
            assert(d.com.sc_ic.ic_state == IEEE80211_S_RUN);
            if (scenario == 0 || scenario == 1) d.complete(scenario == 0);
            if (scenario == 2 || scenario == 3) d.nativeScanCancel = ECANCELED;
            if (scenario == 6) d.sendError = EIO;
        };
        d.onAbort = [&] { assert(!d.nativePortReady()); if (scenario == 2 || scenario == 4) d.complete(false); };
        d.loop.onSleep = [&](void *, AbsoluteTime) {
            if (scenario == 7) { ++d.com.sc_generation; d.nativeCommandTransport = false; return THREAD_AWAKENED; }
            if (scenario == 8) { d.iwx_stop_device(&d.com); d.nativeDetached = true; return THREAD_AWAKENED; }
            return THREAD_TIMED_OUT;
        };
        const int expected[] = {0,0,ECANCELED,ECANCELED,ETIMEDOUT,ETIMEDOUT,EIO,ENXIO,ENXIO};
        assert(f.run() == expected[scenario]);
        const bool preserved = scenario == 0 || scenario == 1 || scenario == 2 || scenario == 4;
        assert(d.nativeCommandTransport == preserved);
        assert(bool(d.com.sc_ic.ic_if.if_flags & IFF_RUNNING) == preserved);
        assert(!d.nativeScanSink.valid() && !d.nativeScanPlan && !d.nativeFirmwareOwner.busy());
        if (preserved) {
            assert(!d.resetComplete && !d.nativeError && d.com.sc_generation == 1 && !f.stopped);
            assert(d.nativeJoinMode && d.com.sc_ic.ic_state == IEEE80211_S_RUN);
            d.nativeScanCancel = 0; d.onSubmit = [&] { d.complete(); };
            assert(f.run() == 0 && f.posted == 2 && d.com.sc_generation == 1);
        } else {
            assert(d.nativeError && (scenario == 8 || d.resetComplete));
        }
    }
    // Data/key permission is narrower than ownership of the scan lease.
    for (unsigned fault = 0; fault < 16; ++fault) {
        ItlIwx d; ScanFixture f(d, 40); d.loop.gated = true;
        auto &ic = d.com.sc_ic;
        ic.ic_state = IEEE80211_S_RUN; ic.ic_bss = &d.peer;
        d.nativeJoinMode = true; f.scan.plan.markAssociatedScan(40);
        ic.ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK;
        d.peer.ni_port_valid = 1; d.peer.ni_flags = IEEE80211_NODE_TXRXPROT;
        d.peer.ni_rsnprotos = IEEE80211_PROTO_RSN; d.peer.ni_rsnakms = IEEE80211_AKM_PSK;
        d.peer.ni_rsncipher = d.peer.ni_rsngroupcipher = IEEE80211_CIPHER_CCMP;
        d.peer.ni_pairwise_key = {IEEE80211_CIPHER_CCMP,16,0,IEEE80211_KEY_SWCRYPTO,&d};
        ic.ic_nw_keys[1] = {IEEE80211_CIPHER_CCMP,16,1,IEEE80211_KEY_SWCRYPTO | IEEE80211_KEY_GROUP,&d};
        d.onSubmit = [&] { assert(!d.nativePortReady()); };
        d.loop.onSleep = [&](void *, AbsoluteTime) {
            assert(d.nativePortReady());
            // An unrelated initializer still cannot acquire the firmware lease.
            assert(!d.nativeFirmwareOwner.acquire(2));
            const auto *savedPlan = d.nativeScanPlan;
            const auto savedSink = d.nativeScanSink;
            const auto savedPair = d.peer.ni_pairwise_key;
            if (fault == 1) d.cancelNativeScan();
            if (fault == 2) ++d.nativeScanDataGeneration;
            if (fault == 3) d.nativeScanPlan = nullptr;
            if (fault == 4) d.nativeScanSink = {};
            if (fault == 5) d.nativeScanDataAllowed = false;
            if (fault == 6) d.com.sc_flags |= IWX_FLAG_SCANNING;
            if (fault == 7) d.com.sc_flags &= ~IWX_FLAG_BGSCAN;
            if (fault == 8) ++d.nativeStateSerial;
            if (fault == 9) d.nativeDetached = true;
            if (fault == 10) d.nativeCommandTransport = false;
            if (fault == 11) d.nativeError = EIO;
            if (fault == 12) d.com.sc_flags |= IWX_FLAG_HW_ERR;
            if (fault == 13) d.peer.ni_pairwise_key.k_priv = nullptr;
            if (fault == 14) d.loop.gated = false;
            if (fault == 15) d.peer.ni_port_valid = 0;
            assert(d.nativePortReady() == (fault == 0));
            if (fault <= 12 || fault == 14) {
                auto &nextGTK = ic.ic_nw_keys[2];
                nextGTK = {IEEE80211_CIPHER_CCMP,16,2,IEEE80211_KEY_GROUP,nullptr};
                int result = d.iwx_native_set_key(&ic, &d.peer, &nextGTK);
                assert(result == (fault == 0 ? 0 : fault == 14 ? EWOULDBLOCK : ENETDOWN));
                assert(d.softwareKeys == (fault == 0 ? 1u : 0u));
            }
            // Restore the fixture so the original terminal/lease cleanup runs.
            d.nativeScanCancel = 0; d.nativeScanDataGeneration = d.com.sc_generation;
            d.nativeScanPlan = savedPlan; d.nativeScanSink = savedSink;
            d.nativeScanDataAllowed = true; d.com.sc_flags = IWX_FLAG_BGSCAN;
            d.nativeStateSerial = d.nativeStateCompleted; d.nativeDetached = false;
            d.nativeCommandTransport = true; d.nativeError = 0; d.loop.gated = true;
            d.peer.ni_pairwise_key = savedPair; d.peer.ni_port_valid = 1;
            d.complete(); return THREAD_AWAKENED;
        };
        assert(f.run() == 0);
        assert(d.nativePortReady() && !d.nativeScanDataAllowed && d.nativeScanDataGeneration == -1);
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        ItlIwx d; ScanFixture f(d); d.loop.gated = true;
        d.com.sc_ic.ic_state = IEEE80211_S_RUN; d.com.sc_ic.ic_bss = &d.peer;
        d.nativeJoinMode = true; f.scan.plan.markAssociatedScan(1);
        if (fault == 0) d.com.sc_ic.ic_if.if_link_state = 0;
        if (fault == 1) d.com.sc_ic.ic_bss = nullptr;
        if (fault == 2) d.nativeJoinMode = false;
        if (fault == 3) d.com.sc_ic.ic_state = IEEE80211_S_INIT;
        if (fault == 4) assert(d.nativeFirmwareOwner.acquire(2));
        if (fault == 5) ++d.nativeStateSerial;
        if (fault == 6) d.com.sc_flags = IWX_FLAG_HW_ERR;
        if (fault == 7) d.com.sc_ic.ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK; // No keys.
        assert(d.nativeScan(f.scan.plan,f.sink) == EOPNOTSUPP && !d.submissions && !d.resetComplete);
        if (fault == 4) assert(d.nativeFirmwareOwner.release(2));
    }
    puts("IWX connected scan: terminal/abort boundaries preserve link, missing terminal resets, generation/detach, repeated scan and admission faults passed");
}

static void watchdogTests() {
    // Run the real watchdog from inside the real scan's accepted sleep window.
    // Timer/firmware notifications are deterministic; no radio is accessed.
    for (bool management : {false, true}) for (unsigned outcome = 0; outcome < 4; ++outcome) {
        ItlIwx d; ScanFixture f(d, 40); d.loop.gated = true;
        auto &ic = d.com.sc_ic;
        ic.ic_state = IEEE80211_S_RUN; ic.ic_bss = &d.peer;
        d.peer.ni_port_valid = 1; d.nativeJoinMode = true;
        f.scan.plan.markAssociatedScan(40);
        if (management) ic.ic_mgt_timer = 2; else d.com.sc_tx_timer = 2;
        assert(d.iwx_native_watchdog(1000) == 0);
        unsigned wakes = 0;
        d.onAbort = [&] {
            assert(!d.nativePortReady() && !d.nativeScanDataAllowed);
            assert(d.nativeFirmwareOwner.busy() && !d.resetComplete);
            if (outcome != 1) d.complete(false);
        };
        d.loop.onSleep = [&](void *, AbsoluteTime) {
            if (++wakes > 1) return THREAD_TIMED_OUT; // Missing abort terminal.
            assert(d.nativePortReady() && d.nativeScanDataAllowed);
            assert(d.iwx_native_watchdog(1999) == 0);
            assert(d.iwx_native_watchdog(2000) == 0);
            assert(management ? ic.ic_mgt_timer == 1 : d.com.sc_tx_timer == 1);
            if (outcome == 2) { // TX completion / response removes the timer.
                ic.ic_mgt_timer = 0; d.com.sc_tx_timer = 0;
                assert(d.iwx_native_watchdog(5000) == 0);
                d.complete(); return THREAD_AWAKENED;
            }
            if (outcome == 3) d.complete(); // Terminal racing the link timeout.
            const auto beforeWakeups = d.loop.wakeups;
            assert(d.iwx_native_watchdog(3000) == ETIMEDOUT);
            assert(d.loop.wakeups == beforeWakeups + 1);
            assert(d.nativeError == ETIMEDOUT && !d.peer.ni_port_valid);
            assert(ic.ic_if.if_link_state == LINK_STATE_DOWN);
            assert(!d.nativePortReady() && !d.nativeScanDataAllowed);
            assert(d.nativeScanCancel && d.nativeFirmwareOwner.busy());
            assert(d.iwx_native_watchdog(3001) == ETIMEDOUT);
            return THREAD_AWAKENED;
        };
        assert(f.run() == (outcome == 2 ? 0 : ETIMEDOUT));
        assert(!d.nativeFirmwareOwner.busy() && !d.nativeScanSink.valid() && !d.nativeScanPlan);
        assert(d.aborts == (outcome < 2 ? 1u : 0u));
        assert(d.resetComplete == (outcome != 2));
        assert(d.nativeCommandTransport == (outcome == 2));
        assert(d.com.sc_generation == (outcome == 2 ? 1 : 2));
        assert(f.posted == 1);
    }
    // Keep non-data firmware phases paused, and restart their time base.
    for (unsigned fault = 0; fault < 3; ++fault) {
        ItlIwx d; d.loop.gated = true; d.com.sc_ic.ic_bss = &d.peer;
        d.com.sc_tx_timer = 2;
        assert(d.iwx_native_watchdog(1000) == 0);
        if (fault == 0) ++d.nativeStateSerial;
        else assert(d.nativeFirmwareOwner.acquire(2));
        if (fault == 2) { d.nativeScanDataAllowed = true; d.nativeScanDataGeneration = 1; }
        assert(d.iwx_native_watchdog(30000) == 0 && !d.nativeWatchdogActive);
        assert(d.com.sc_tx_timer == 2);
        d.nativeStateSerial = d.nativeStateCompleted;
        if (fault) assert(d.nativeFirmwareOwner.release(2));
        assert(d.iwx_native_watchdog(40000) == 0);
        assert(d.iwx_native_watchdog(40999) == 0 && d.com.sc_tx_timer == 2);
        assert(d.iwx_native_watchdog(42000) == ETIMEDOUT);
    }
    // Context, lifetime, generation and hardware faults still win over timers.
    for (unsigned fault = 0; fault < 10; ++fault) {
        ItlIwx d; d.loop.gated = true; d.com.sc_ic.ic_bss = &d.peer;
        d.com.sc_tx_timer = 2;
        if (fault == 0) d.loop.gated = false;
        if (fault == 1) d.nativeDetached = true;
        if (fault == 2) d.nativeNet80211Attached = false;
        if (fault == 3) d.nativeCommandTransport = false;
        if (fault == 4) d.com.sc_ic.ic_bss = nullptr;
        if (fault == 5) d.com.sc_ic.ic_if.if_flags = 0;
        if (fault == 6) d.com.sc_flags = IWX_FLAG_SHUTDOWN;
        if (fault == 7) d.com.sc_flags = IWX_FLAG_HW_ERR;
        if (fault == 8) d.com.sc_flags = IWX_FLAG_RFKILL;
        if (fault == 9) ++d.com.sc_generation;
        assert(d.iwx_native_watchdog(1000) == (fault == 0 ? EWOULDBLOCK : ENETDOWN));
        assert(d.com.sc_tx_timer == 2 && !d.nativeWatchdogActive);
    }
    {
        ItlIwx d; d.loop.gated = true; d.com.sc_ic.ic_bss = &d.peer;
        assert(d.iwx_native_watchdog(1000) == 0);
        assert(d.iwx_native_watchdog(999) == ETIMEDOUT);
    }
    puts("IWX watchdog: TX/management expiry during real background scan, recovery terminal/missing/race, successful progress, paused phases and lifetime faults passed");
}

static void scanTests() {
    {
        ItlIwx d; ScanFixture f(d, 144); d.loop.gated = true;
        d.onSubmit = [&] { assert(d.nativeScanPlan->channels[0] == 144); d.complete(); };
        assert(f.run() == 0 && f.stopped == 1 && f.posted == 1);
    }
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        ItlIwx d; ScanFixture f(d); d.loop.gated = true;
        d.com.txq[0].payload.live = true;
        unsigned wakes = 0;
        d.onSubmit = [&] {
            assert(d.nativeFirmwareOwner.owns(1));
            if (scenario == 1) d.complete();
            if (scenario == 2) d.nativeScanCancel = ECANCELED;
            if (scenario == 3) d.sendError = EIO;
        };
        d.onAbort = [&] { if (scenario != 6) d.complete(false); };
        d.loop.onSleep = [&](void *event, AbsoluteTime deadline) {
            assert(event == &d.nativeScanDone && d.nativeFirmwareOwner.owns(1));
            ++wakes;
            if (scenario == 4 || scenario == 6) return THREAD_TIMED_OUT;
            if (scenario == 5) { d.nativeScanCancel = ECANCELED; return THREAD_AWAKENED; }
            if (scenario == 7 && wakes < 3) return THREAD_AWAKENED;
            if (scenario == 8) { d.nativeCommandTransport = false; ++d.com.sc_generation; return THREAD_AWAKENED; }
            if (scenario == 9) return THREAD_INTERRUPTED;
            if (scenario == 10) { d.complete(); return THREAD_TIMED_OUT; }
            assert(deadline == clockNow + SEC_TO_NSEC(30));
            d.complete(); return THREAD_AWAKENED;
        };
        const int expected[] = {0,0,ECANCELED,EIO,ETIMEDOUT,ECANCELED,ETIMEDOUT,0,ENXIO,EINTR,0};
        assert(f.run() == expected[scenario]);
        assert(d.resetComplete && !d.nativeFirmwareOwner.busy() && f.stopped == 1);
        assert(!d.nativeScanSink.valid() && !d.nativeScanPlan);
        assert(d.com.sc_ic.ic_des_esslen == 3 && memcmp(d.com.sc_ic.ic_des_essid, "old", 3) == 0);
        if (scenario == 1 || scenario == 2 || scenario == 3) assert(!d.loop.sleeps);
        if (scenario == 6) assert((d.loop.deadlines == std::vector<uint64_t>{clockNow + SEC_TO_NSEC(30), clockNow + SEC_TO_NSEC(3)}));
        if (scenario == 7) assert((d.loop.deadlines == std::vector<uint64_t>(3, clockNow + SEC_TO_NSEC(30))));
        // A failed/canceled/timed-out scan must release its slot for the next scan.
        d.nativeCommandTransport = true; d.com.sc_ic.ic_if.if_flags |= IFF_RUNNING;
        d.sendError = 0; d.onSubmit = [&] { d.complete(); };
        assert(f.run() == 0 && f.posted == 2 && f.stopped == 2);
    }
    ItlIwx d; ScanFixture f(d);
    assert(d.nativeScan(f.scan.plan, f.sink) == EWOULDBLOCK);
    d.loop.gated = true; d.loop.on = true;
    assert(d.nativeScan(f.scan.plan, f.sink) == EWOULDBLOCK);
    d.loop.on = false;
    assert(d.nativeFirmwareOwner.acquire(2));
    assert(d.nativeScan(f.scan.plan, f.sink) == EBUSY && !d.submissions);
    assert(d.nativeFirmwareOwner.release(2));
}

static void securityKeyTests() {
    unsigned cases = 0;
    for (unsigned pair : {unsigned(IEEE80211_CIPHER_CCMP), unsigned(IEEE80211_CIPHER_TKIP)})
    for (unsigned group : {unsigned(IEEE80211_CIPHER_CCMP), unsigned(IEEE80211_CIPHER_TKIP)})
    for (bool pmf : {false,true}) {
        ItlIwx d; d.loop.gated = true; d.nativeJoinMode = true;
        auto &ic = d.com.sc_ic; auto &ni = d.peer;
        ic.ic_bss = &ni; ic.ic_state = IEEE80211_S_RUN; ic.ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK;
        ni.ni_port_valid = 1; ni.ni_flags = IEEE80211_NODE_TXRXPROT;
        ni.ni_rsnprotos = IEEE80211_PROTO_RSN; ni.ni_rsnakms = IEEE80211_AKM_PSK;
        ni.ni_rsncipher = pair; ni.ni_rsngroupcipher = group; ni.ni_rsngroupmgmtcipher = IEEE80211_CIPHER_BIP;
        if (pmf) ni.ni_flags |= IEEE80211_NODE_MFP | IEEE80211_NODE_RXMGMTPROT | IEEE80211_NODE_TXMGMTPROT;
        auto &ptk = ni.ni_pairwise_key;
        ptk = {pair,pair == IEEE80211_CIPHER_TKIP ? 32u : 16u,0,0,nullptr};
        assert(!d.iwx_native_set_key(&ic,&ni,&ptk));
        auto &gtk = ic.ic_nw_keys[1];
        gtk = {group,group == IEEE80211_CIPHER_TKIP ? 32u : 16u,1,IEEE80211_KEY_GROUP,nullptr};
        assert(!d.iwx_native_set_key(&ic,&ni,&gtk));
        assert(d.iwx_native_port_ready() == !pmf);
        for (unsigned id : {4u,5u}) {
            auto &igtk = ic.ic_nw_keys[id];
            igtk = {IEEE80211_CIPHER_BIP,16,id,IEEE80211_KEY_IGTK,nullptr};
            const bool accepted = pmf && pair == IEEE80211_CIPHER_CCMP && group == IEEE80211_CIPHER_CCMP;
            assert((d.iwx_native_set_key(&ic,&ni,&igtk) == 0) == accepted);
            assert(d.iwx_native_port_ready() == (!pmf || accepted)); ++cases;
        }
        for (unsigned fault = 0; fault < 7; ++fault) {
            auto saved = gtk;
            gtk.k_priv = nullptr; gtk.k_flags = IEEE80211_KEY_GROUP;
            if (fault == 0) gtk.k_len ^= 16;
            if (fault == 1) gtk.k_cipher = IEEE80211_CIPHER_NONE;
            if (fault == 2) gtk.k_id = 4;
            if (fault == 3) gtk.k_flags |= 0x80;
            if (fault == 4) ++d.com.sc_generation;
            if (fault == 5) d.nativeError = EIO;
            if (fault == 6) ++d.nativeStateSerial;
            assert(d.iwx_native_set_key(&ic,&ni,&gtk) != 0);
            gtk = saved; d.com.sc_generation = 1; d.nativeError = 0; d.nativeStateSerial = 0; ++cases;
        }
        ic.ic_native_tkip.failure(airport_up_time()); ic.ic_native_tkip.failure(airport_up_time());
        if (pair == IEEE80211_CIPHER_TKIP || group == IEEE80211_CIPHER_TKIP) {
            assert(!d.iwx_native_port_ready());
            gtk.k_priv = nullptr; gtk.k_flags = IEEE80211_KEY_GROUP;
            assert(d.iwx_native_set_key(&ic,&ni,&gtk) == ENETDOWN); ++cases;
        }
    }
    printf("IWX TKIP/PMF real key/port admission: %u cases passed; software allocation mocked\n",cases);
}

int main(int argc, char **argv) {
    const std::string mode = argc == 2 ? argv[1] : "all";
    if (mode == "all" || mode == "security") securityKeyTests();
    if (mode == "all" || mode == "stop") stopTests();
    if (mode == "all" || mode == "detach") detachTests();
    if (mode == "all" || mode == "scan") { scanTests(); associatedScanTests(); }
    if (mode == "all" || mode == "firmware") firmwareTests();
    if (mode == "all" || mode == "watchdog") watchdogTests();
    puts("Actual IWX scan/stop/detach/firmware: reset-before-free, partial attach, lease drain, 11 scan outcomes, repeat-scan admission and 6 firmware DMA lifetimes passed; HW/kernel boundaries mocked");
}
