#include <cassert>
#include <cstddef>
#include <cerrno>
#include <cstdio>
#include <vector>

#define IO80211FAMILY_V2
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define XYLog(...) ((void)0)
#define _KASSERT(x) assert(x)
#define container_of(p, type, member) reinterpret_cast<type *>(reinterpret_cast<char *>(p) - offsetof(type, member))
enum { IWM_FLAG_SHUTDOWN = 1, IWM_FLAG_HW_ERR = 2, IWM_FLAG_RFKILL = 4 };
enum { IFF_RUNNING = 1, IFF_UP = 2, LINK_STATE_DOWN = 0 };
enum { kIOReturnSuccess, kIOReturnCannotLock, kIOReturnNotReady, kIOReturnBusy,
       kIOReturnTimeout, kIOReturnIOError };
using IOReturn = int;
struct OSObject {};
struct IONetworkInterface {};
struct _ifnet { int if_flags = IFF_UP | IFF_RUNNING, if_link_state = 1; };
struct ieee80211_node { int ni_port_valid = 1; };
struct ieee80211com {
    struct { _ifnet ac_if; } ic_ac;
    ieee80211_node *ic_bss = nullptr;
    int ic_state = 4;
};
#define ic_if ic_ac.ac_if
struct iwm_softc { ieee80211com sc_ic; int sc_flags = 0, sc_generation = 7; };
struct Loop {
    bool gated = true, onWorkThread = false;
    bool inGate() { return gated; }
    bool onThread() { return onWorkThread; }
    template <typename F> IOReturn runAction(F f, OSObject *o, void *p) {
        gated = true; const auto result = f(o, p, nullptr, nullptr, nullptr); gated = false; return result;
    }
};
struct Gate { std::vector<void *> wakes; void commandWakeup(void *p, bool) { wakes.push_back(p); } };
struct Owner { bool held = false; bool busy() { return held; } };
static int splnet() { return 0; }
static void splx(int) {}
struct ItlIwm : OSObject {
    iwm_softc com;
    Loop loop;
    Gate gate;
    Owner nativeFirmwareOwner;
    bool nativeDetached = false, nativeNet80211Attached = true, trial = false;
    bool commandTransportReady = true, nativeWatchdogActive = true;
    int nativeStateError = 0, nativeScanCancel = 0, nativeScanDone = 0, nativeStateCompleted = 1;
    int stops = 0, resets = 0, inits = 0, resumes = 0, resumeError = 0, initError = 0;
    std::vector<int> events;
    Loop *getMainWorkLoop() { return &loop; }
    Gate *getMainCommandGate() { return &gate; }
    bool isHardwareTrial() { return trial; }
    struct FirmwareLease {
        ItlIwm *driver; bool acquired;
        explicit FirmwareLease(ItlIwm *p) : driver(p), acquired(!p->nativeFirmwareOwner.held) {
            if (acquired) p->nativeFirmwareOwner.held = true;
        }
        ~FirmwareLease() { if (acquired) driver->nativeFirmwareOwner.held = false; }
        explicit operator bool() const { return acquired; }
    };
    void iwm_stop_device(iwm_softc *) { ++resets; events.push_back(1); commandTransportReady = false; }
    void iwm_stop(_ifnet *ifp) {
        ++stops; events.push_back(2); ifp->if_flags &= ~IFF_RUNNING;
        nativeStateError = 0; ++com.sc_generation;
    }
    int iwm_resume(iwm_softc *) { ++resumes; events.push_back(3); return resumeError; }
    int iwm_init(_ifnet *ifp) {
        ++inits; events.push_back(4);
        if (!initError) { ifp->if_flags |= IFF_RUNNING; commandTransportReady = true; }
        return initError;
    }
    static void iwm_init_task(void *);
    IOReturn enable(IONetworkInterface *);
};
#include "iwm_fault_methods.inc"

int main() {
    unsigned cases = 0;
    for (bool running : {false, true}) for (bool owner : {false, true})
    for (bool transport : {false, true}) for (int flags : {0, int(IWM_FLAG_HW_ERR), int(IWM_FLAG_RFKILL)})
    for (int priorError : {0, ETIMEDOUT}) for (bool peer : {false, true}) {
        ItlIwm d; ieee80211_node node;
        d.com.sc_ic.ic_if.if_flags = IFF_UP | (running ? IFF_RUNNING : 0);
        d.com.sc_ic.ic_bss = peer ? &node : nullptr;
        d.com.sc_flags = flags; d.nativeStateError = priorError;
        d.nativeFirmwareOwner.held = owner; d.commandTransportReady = transport;
        ItlIwm::iwm_init_task(&d.com);
        assert(d.stops == 0 && d.inits == 0);
        if (running || owner || transport) {
            assert(d.resets == 1 && !d.commandTransportReady);
            assert(d.nativeStateError == (priorError ? priorError : (flags & IWM_FLAG_RFKILL ? ENETDOWN : EIO)));
            assert(!(d.com.sc_ic.ic_if.if_flags & IFF_RUNNING));
            assert(d.com.sc_ic.ic_if.if_link_state == LINK_STATE_DOWN);
            assert(!peer || !node.ni_port_valid);
            assert(d.com.sc_generation == 8 && !d.nativeWatchdogActive);
            assert(d.nativeScanCancel && d.gate.wakes.size() == 2);
        } else assert(!d.resets && d.nativeStateError == priorError);
        assert(d.nativeFirmwareOwner.held == owner);
        ++cases;
    }
    for (unsigned skipped = 0; skipped < 4; ++skipped) {
        ItlIwm d;
        if (skipped == 0) d.com.sc_flags = IWM_FLAG_SHUTDOWN;
        if (skipped == 1) d.nativeDetached = true;
        if (skipped == 2) d.nativeNet80211Attached = false;
        if (skipped == 3) d.trial = true;
        ItlIwm::iwm_init_task(&d.com);
        assert(!d.resets && !d.stops && !d.inits && !d.nativeStateError); ++cases;
    }
    { ItlIwm d; d.nativeScanCancel = ECANCELED; ItlIwm::iwm_init_task(&d.com);
      assert(d.nativeScanCancel == ECANCELED); ++cases; }
    IONetworkInterface netif;
    for (bool gated : {false, true}) for (bool busy : {false, true}) for (bool failed : {false, true}) {
        ItlIwm d; d.loop.gated = gated; d.nativeFirmwareOwner.held = busy;
        d.nativeStateError = failed ? EIO : 0;
        const auto result = d.enable(&netif);
        if (busy) assert(result == kIOReturnBusy && !d.stops && !d.inits);
        else if (failed) {
            assert(result == kIOReturnSuccess && !d.nativeStateError);
            assert((d.events == std::vector<int>{2, 3, 4}));
        } else assert(result == kIOReturnSuccess && d.events.empty());
        assert(d.loop.gated == gated && d.nativeFirmwareOwner.held == busy); ++cases;
    }
    std::printf("IWM fault/owned-reenable: %u cases passed\n", cases);
}
