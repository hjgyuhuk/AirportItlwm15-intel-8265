#include <HAL/IwmOperationOwner.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#define __PRIVATE_SPI__ 1
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnCannotLock = 1000, kIOReturnNotReady,
       kIOReturnBusy, kIOReturnError };
enum { IFF_UP = 1, IFF_RUNNING = 2, IWX_FLAG_RFKILL = 4,
       IWX_FLAG_SCANNING = 8, IWX_FLAG_BGSCAN = 16, IEEE80211_S_INIT = 0 };
struct OSObject {};
struct IONetworkInterface { unsigned signals = 0; void signalOutputThread() { ++signals; } };
struct WorkLoop {
    bool gated = false, on = false;
    bool inGate() { return gated; }
    bool onThread() { return on; }
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *owner, void *arg = nullptr) {
        const bool saved = gated;
        gated = true;
        auto result = action(owner, arg, nullptr, nullptr, nullptr);
        gated = saved;
        return result;
    }
};
struct _ifnet {
    unsigned if_flags = 0;
    int if_snd = 0;
    IONetworkInterface *iface = nullptr;
    unsigned starts = 0;
    void (*if_start)(_ifnet *) = [](_ifnet *p) { ++p->starts; };
};
struct ieee80211com { _ifnet ic_if; int ic_state = 0; };
struct iwx_tx_ring { unsigned queued = 0, low_mark = 4, qid = 0; };
struct iwx_softc {
    ieee80211com sc_ic;
    unsigned qfullmsk = 0;
    unsigned sc_flags = 0, sc_generation = 0;
    struct { bool sku_cap_11n_enable = true, sku_cap_11ac_enable = true,
                  sku_cap_11ax_enable = true; } sc_nvm;
};
static void ifq_clr_oactive(int *q) { *q = 0; }
static bool ifq_is_oactive(int *q) { return *q != 0; }
static void ifq_flush(int *) {}

class ItlIwx : public OSObject {
public:
    iwx_softc com;
    WorkLoop loop;
    IwmOperationOwner firmwareOwner;
    bool nativeDetached = false, nativeNet80211Attached = true;
    bool nativeCommandTransport = false, nativeJoinMode = false, nativeWatchdogActive = false;
    int nativeError = 0, initError = 0, stopError = 0;
    uint64_t nativeStateSerial = 0, nativeStateCompleted = 0;
    struct { void clear() {} } nativeJoinProgress;
    struct { bool active = false; bool valid() const { return active; } } nativeScanSink;
    int nativeScanCancel = 0;
    bool nativeScanDone = false;
    int nativeScanCachePhy = 0;
    unsigned initializations = 0, stopped = 0, ht = 0, vht = 0, he = 0, wakeups = 0;
    WorkLoop *getMainWorkLoop() { return &loop; }
    class FirmwareLease {
        ItlIwx *driver;
        bool held;
    public:
        explicit FirmwareLease(ItlIwx *d) : driver(d), held(d->loop.inGate() && !d->loop.onThread() &&
            !d->nativeDetached && d->firmwareOwner.acquire(1)) {}
        ~FirmwareLease() { if (held) assert(driver->firmwareOwner.release(1)); }
        explicit operator bool() const { return held; }
    };
    // Kernel/firmware stop and initialization are the explicit mock boundary.
    int iwx_init_hw(iwx_softc *) {
        assert(loop.inGate() && !loop.onThread() && firmwareOwner.owns(1));
        ++initializations; return initError;
    }
    void iwx_stop_device(iwx_softc *) { ++stopped; }
    int iwx_native_stop() {
        if (stopError) return stopError;
        FirmwareLease lease(this);
        if (!lease) return EBUSY;
        iwx_stop_device(&com);
        nativeCommandTransport = false;
        com.sc_ic.ic_if.if_flags &= ~IFF_RUNNING;
        nativeError = 0;
        return 0;
    }
    void iwx_native_reset_runtime() {}
    void iwx_setup_ht_rates(iwx_softc *) { ++ht; }
    void iwx_setup_vht_rates(iwx_softc *) { ++vht; }
    void iwx_setup_he_rates(iwx_softc *) { ++he; }
    void wakeupOn(bool *done) { assert(done == &nativeScanDone); ++wakeups; }
    void iwx_clear_oactive(iwx_softc *, iwx_tx_ring *);
    IOReturn nativeEnable(IONetworkInterface *);
    IOReturn nativeDisable(IONetworkInterface *);
};

#include "sequoia_radio_production_methods.inc"

int main()
{
    for (bool legacy : {false, true}) {
        ItlIwx driver;
        IONetworkInterface interface;
        auto &ifp = driver.com.sc_ic.ic_if;
        ifp.iface = legacy ? &interface : nullptr;
        ifp.if_snd = 1;
        driver.com.qfullmsk = 1;
        iwx_tx_ring ring;
        driver.iwx_clear_oactive(&driver.com, &ring);
        assert(ifp.starts == 1 && !driver.com.qfullmsk);
        assert(interface.signals == unsigned(legacy));
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        ItlIwx driver;
        auto &flags = driver.com.sc_ic.ic_if.if_flags;
        IOReturn expected = 0;
        if (fault == 1) { driver.nativeDetached = true; expected = kIOReturnNotReady; }
        if (fault == 2) { driver.nativeNet80211Attached = false; expected = kIOReturnNotReady; }
        if (fault == 3) { driver.loop.on = true; expected = kIOReturnCannotLock; }
        if (fault == 4) { assert(driver.firmwareOwner.acquire(2)); expected = kIOReturnBusy; }
        if (fault == 5) { driver.initError = EIO; expected = kIOReturnError; }
        if (fault == 6) { driver.com.sc_flags = IWX_FLAG_RFKILL; expected = kIOReturnNotReady; }
        if (fault == 7) { driver.nativeError = EIO; driver.stopError = EBUSY; expected = kIOReturnBusy; }
        if (fault == 8) { driver.nativeError = EIO; driver.stopError = EIO; expected = kIOReturnError; }
        if (fault == 9) { driver.nativeStateSerial = UINT64_MAX; expected = kIOReturnError; }
        assert(driver.nativeEnable(nullptr) == expected);
        assert(driver.nativeCommandTransport == !fault);
        assert(flags == (fault ? 0u : IFF_UP | IFF_RUNNING));
        if (fault == 4) assert(driver.firmwareOwner.release(2));
        assert(!driver.firmwareOwner.busy());
        if (fault == 5 || fault == 6) assert(driver.stopped == 1);
        if (!fault) {
            assert(driver.initializations == 1 && driver.ht == 1 && driver.vht == 1 && driver.he == 1);
            assert(driver.nativeEnable(nullptr) == 0 && driver.initializations == 1);
            driver.nativeScanSink.active = true;
            assert(driver.nativeDisable(nullptr) == kIOReturnBusy);
            assert(driver.nativeScanCancel == ECANCELED && driver.wakeups == 1);
            driver.nativeScanSink.active = false;
            driver.stopError = EIO;
            assert(driver.nativeDisable(nullptr) == kIOReturnError && driver.nativeCommandTransport);
            driver.stopError = EBUSY;
            assert(driver.nativeDisable(nullptr) == kIOReturnBusy);
            driver.stopError = 0;
            assert(driver.nativeDisable(nullptr) == 0 && !flags && !driver.nativeCommandTransport);
            // A firmware error is drained before a subsequent enable.
            driver.nativeError = EIO;
            assert(driver.nativeEnable(nullptr) == 0 && driver.initializations == 2);
            assert(driver.nativeDisable(nullptr) == 0);
        }
    }
    puts("IWX radio production methods: optional legacy child, gate/lease, init/stop faults, RFKILL, scan drain and restart passed");
}
