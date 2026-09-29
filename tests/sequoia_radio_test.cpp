#include <HAL/IwmOperationOwner.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#define __PRIVATE_SPI__ 1
#define IWM_TX_RING_LOMARK 4
#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define XYLog(...) ((void)0)
#define KASSERT(test, message) assert(test)
#define container_of(sc, type, member) ((sc)->driver)
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnCannotLock = 1000, kIOReturnNotReady,
       kIOReturnBusy, kIOReturnTimeout, kIOReturnIOError };
enum { IFF_UP = 1, IFF_RUNNING = 2, IWM_FLAG_RFKILL = 4, IEEE80211_M_STA = 1 };
struct OSObject {};
struct IONetworkInterface { unsigned signals = 0; void signalOutputThread() { ++signals; } };
struct WorkLoop {
    bool gated = false, on = false;
    bool inGate() { return gated; }
    bool onThread() { return on; }
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *owner, void *arg) {
        const bool saved = gated;
        gated = true;
        auto result = action(owner, arg, nullptr, nullptr, nullptr);
        gated = saved;
        return result;
    }
};
struct _ifnet {
    void *if_softc = nullptr;
    unsigned if_flags = 0;
    int if_snd = 0;
    IONetworkInterface *iface = nullptr;
    unsigned starts = 0;
    void (*if_start)(_ifnet *) = [](_ifnet *p) { ++p->starts; };
};
struct iwm_tx_ring { unsigned queued = 0, qid = 0; };
struct ieee80211com { struct { _ifnet ac_if; } ic_ac; int ic_opmode = IEEE80211_M_STA; };
#define ic_if ic_ac.ac_if
class ItlIwm;
struct iwm_softc {
    ieee80211com sc_ic;
    ItlIwm *driver = nullptr;
    unsigned sc_flags = 0, qfullmsk = 0;
    int sc_generation = 0, agg_tid_disable = 0, agg_queue_mask = 0, sc_tx_ba[4] = {};
    struct { unsigned refs = 0; } task_refs;
    struct { bool sku_cap_11n_enable = true, sku_cap_11ac_enable = true; } sc_nvm;
};
static void ifq_clr_oactive(int *q) { *q = 0; }
static bool ifq_is_oactive(int *q) { return *q != 0; }
static void ifq_flush(int *) {}

class ItlIwm : public OSObject {
public:
    iwm_softc com;
    WorkLoop loop;
    IwmOperationOwner firmwareOwner;
    bool nativeDetached = false, nativeNet80211Attached = true, trial = false;
    bool detachDuringInit = false;
    int resumeError = 0, initError = 0;
    int nativeStateError = 0;
    unsigned resumes = 0, initializations = 0, stopped = 0, ht = 0, vht = 0;
    ItlIwm() { com.driver = this; com.sc_ic.ic_ac.ac_if.if_softc = &com; }
    WorkLoop *getMainWorkLoop() { return &loop; }
    bool isHardwareTrial() { return trial; }
    class FirmwareLease {
        ItlIwm *driver;
        bool held;
    public:
        explicit FirmwareLease(ItlIwm *d) : driver(d), held(d->loop.inGate() && !d->loop.onThread() &&
            !d->nativeDetached && d->firmwareOwner.acquire(1)) {}
        ~FirmwareLease() { if (held) assert(driver->firmwareOwner.release(1)); }
        explicit operator bool() const { return held; }
    };
    int iwm_resume(iwm_softc *) { ++resumes; return resumeError; }
    int iwm_init_hw(iwm_softc *) {
        assert(loop.inGate() && !loop.onThread() && firmwareOwner.owns(1));
        ++initializations;
        if (detachDuringInit) { nativeDetached = true; ++com.sc_generation; return ENXIO; }
        return initError;
    }
    void iwm_stop_device(iwm_softc *) { ++stopped; }
    void iwm_stop(_ifnet *ifp) { iwm_stop_device(&com); ifp->if_flags &= ~IFF_RUNNING; }
    void iwm_setup_ht_rates(iwm_softc *) { ++ht; }
    void iwm_setup_vht_rates(iwm_softc *) { ++vht; }
    void iwm_clear_oactive(iwm_softc *, iwm_tx_ring *);
    IOReturn enable(IONetworkInterface *);
    IOReturn disable(IONetworkInterface *);
    int iwm_init(_ifnet *);
    int iwm_scan(iwm_softc *);
    static int iwm_bgscan(ieee80211com *);
};

#include "sequoia_radio_production_methods.inc"

int main()
{
    IONetworkInterface interface;
    {
        ItlIwm driver;
        auto &ifp = driver.com.sc_ic.ic_ac.ac_if;
        ifp.if_snd = 1;
        driver.com.qfullmsk = 1;
        iwm_tx_ring ring;
        driver.iwm_clear_oactive(&driver.com, &ring);
        assert(ifp.starts == 1 && !driver.com.qfullmsk);
        ifp.iface = &interface;
        driver.iwm_clear_oactive(&driver.com, &ring);
        assert(interface.signals == 1);
    }
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        ItlIwm driver;
        auto &flags = driver.com.sc_ic.ic_ac.ac_if.if_flags;
        // The Skywalk path has no custom IONetworkInterface.
        assert(driver.enable(nullptr) == kIOReturnSuccess);
        assert(driver.disable(nullptr) == kIOReturnSuccess && !flags);
        driver.resumes = driver.initializations = driver.stopped = driver.ht = driver.vht = 0;
        driver.loop.on = true;
        assert(driver.enable(&interface) == kIOReturnCannotLock);
        assert(driver.disable(&interface) == kIOReturnCannotLock);
        driver.loop.on = false;
        driver.trial = true;
        assert(driver.enable(&interface) == kIOReturnNotReady);
        driver.trial = false;
        driver.nativeNet80211Attached = false;
        assert(driver.enable(&interface) == kIOReturnNotReady);
        driver.nativeNet80211Attached = true;
        assert(driver.firmwareOwner.acquire(2));
        assert(driver.enable(&interface) == kIOReturnBusy);
        assert(driver.disable(&interface) == kIOReturnBusy);
        assert(!driver.resumes && !driver.initializations && !driver.stopped);
        assert(driver.firmwareOwner.release(2));
        driver.com.sc_flags = IWM_FLAG_RFKILL;
        assert(driver.enable(&interface) == kIOReturnNotReady);
        assert(!flags && !driver.initializations);
        driver.com.sc_flags = 0;
        driver.resumeError = EIO;
        assert(driver.enable(&interface) == kIOReturnIOError);
        assert(!flags && driver.stopped == 1);
        driver.resumeError = 0;
        driver.initError = ETIMEDOUT;
        assert(driver.enable(&interface) == kIOReturnTimeout);
        assert(!flags && driver.stopped == 2 && !driver.firmwareOwner.busy());
        driver.initError = 0;
        assert(driver.enable(&interface) == kIOReturnSuccess);
        assert(flags == (IFF_UP | IFF_RUNNING) && driver.ht == 1 && driver.vht == 1);
        const auto initializations = driver.initializations;
        assert(driver.enable(&interface) == kIOReturnSuccess);
        assert(driver.initializations == initializations);
        assert(driver.iwm_scan(&driver.com) == EOPNOTSUPP);
        assert(driver.iwm_bgscan(&driver.com.sc_ic) == EOPNOTSUPP);
        assert(driver.disable(&interface) == kIOReturnSuccess && !flags);
        assert(driver.enable(&interface) == kIOReturnSuccess);
        assert(driver.initializations == initializations + 1);
        assert(driver.disable(&interface) == kIOReturnSuccess);
        driver.detachDuringInit = true;
        const auto stopped = driver.stopped;
        assert(driver.enable(&interface) == kIOReturnIOError);
        assert(driver.stopped == stopped && !flags && !driver.firmwareOwner.busy());
        assert(driver.disable(&interface) == kIOReturnNotReady);
    }
    puts("Native radio production methods: exclusion, errors, restart, detach during init, no legacy scans passed");
}
