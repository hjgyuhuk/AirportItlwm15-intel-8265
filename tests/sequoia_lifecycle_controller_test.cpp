#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

using IOReturn = int;
using UInt = unsigned;
using UInt64 = unsigned long long;
using UInt8 = unsigned char;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNotReady = -1;
constexpr unsigned IEEE80211_F_NOVHT = 1, IEEE80211_F_NOHT40 = 2, IEEE80211_F_AUTO_JOIN = 4;
constexpr unsigned kIONetworkLinkValid = 1, kIONetworkLinkActive = 2;
static const void *kOSBooleanFalse = nullptr;
static std::string failure;
static std::vector<std::string> events;
static unsigned live = 0, lockLive = 0, storageLive = 0, gateDepth = 0;
static bool fail(const char *point) { events.emplace_back(point); return failure == point; }
static void mark(const char *point) { events.emplace_back(point); }
static bool happened(const char *point) {
    for (const auto &event : events) if (event == point) return true;
    return false;
}
static void before(const char *first, const char *second) {
    size_t a = events.size(), b = events.size();
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i] == first && a == events.size()) a = i;
        if (events[i] == second && b == events.size()) b = i;
    }
    assert(a < b && b < events.size());
}

struct OSObject {
    unsigned references = 1;
    OSObject() { ++live; }
    virtual ~OSObject() { assert(references == 0); --live; }
    void retain() { assert(references); ++references; }
    void *metaCast(const char *) { return this; }
    void release() { assert(references); if (!--references) delete this; }
};
struct OSString : OSObject {
    static OSString *withCString(const char *) { return new OSString; }
};
struct OSNumber : OSObject {
    static OSNumber *withNumber(unsigned long long, unsigned) { return new OSNumber; }
};
#define OSSafeReleaseNULL(value) do { if (value) { (value)->release(); (value) = nullptr; } } while (0)
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
struct IOService : OSObject {
    template <typename T> bool setProperty(const char *, T) { return true; }
    void setProperty(const char *, const void *) {}
    void setProperty(const char *, const void *, size_t) {}
    void setProperty(const char *, UInt64, unsigned) {}
    template <typename T> bool setProperty(const char *, T *) { return true; }
    virtual void registerService(unsigned = 0) { mark("publish"); }
};
static IOService *gItlwmStageProvider = nullptr;
struct IOPCIDevice : IOService {};
struct IOLock { bool locked = false; };
static IOLock *IOLockAlloc() {
    if (fail("pm-lock")) return nullptr;
    ++lockLive; return new IOLock;
}
static void IOLockLock(IOLock *lock) { assert(lock && !lock->locked); lock->locked = true; }
static void IOLockUnlock(IOLock *lock) { assert(lock && lock->locked); lock->locked = false; }
static void IOLockFree(IOLock *lock) { assert(lock && !lock->locked); --lockLive; delete lock; }
struct IOEventSource : OSObject { virtual void cancelTimeout() {} virtual void disable() {} };
struct IOCommandGate : IOEventSource {
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    static IOCommandGate *commandGate(OSObject *, Action) {
        return fail("gate-allocate") ? nullptr : new IOCommandGate;
    }
};
struct IOTimerEventSource : IOEventSource {
    using Action = void (*)(OSObject *, IOTimerEventSource *);
    static IOTimerEventSource *timerEventSource(OSObject *, Action) {
        return fail("watchdog-allocate") ? nullptr : new IOTimerEventSource;
    }
    void cancelTimeout() override { assert(gateDepth); mark("watchdog-cancel"); }
    void disable() override { assert(gateDepth); mark("watchdog-disable"); }
};
struct IOWorkLoop : OSObject {
    std::set<IOEventSource *> sources;
    bool watchdog = false;
    ~IOWorkLoop() override { assert(sources.empty()); }
    static IOWorkLoop *workLoop() {
        if (fail("watchdog-loop")) return nullptr;
        auto *loop = new IOWorkLoop; loop->watchdog = true; return loop;
    }
    IOReturn addEventSource(IOEventSource *source) {
        assert(source);
        if (fail(watchdog ? "watchdog-add" : "gate-add")) return -1;
        assert(sources.insert(source).second); source->retain(); return 0;
    }
    void removeEventSource(IOEventSource *source) {
        assert(sources.erase(source) == 1); source->release();
    }
    IOReturn runAction(IOCommandGate::Action action, OSObject *owner) {
        ++gateDepth;
        auto result = action(owner, nullptr, nullptr, nullptr, nullptr);
        --gateDepth; return result;
    }
};
#define OSMemberFunctionCast(type, owner, method) static_cast<type>(nullptr)
struct IONetworkMedium {};
struct IONetworkInterface : IOService {};
struct AirportItlwm;
struct AirportItlwmEthernetInterface : IONetworkInterface {
    AirportItlwm *owner = nullptr;
    void *ifnet = nullptr;
    void *getIfnet() { return ifnet; }
    IOReturn attachToDataLinkLayer(unsigned, void *);
    void registerService(unsigned options = 0) override;
};
struct Ifnet { IONetworkInterface *iface = nullptr; void *netStat = nullptr; };
struct Ieee80211 {
    unsigned ic_userflags = 0, ic_flags = IEEE80211_F_AUTO_JOIN;
    unsigned char ic_myaddr[6] = {2, 1, 2, 3, 4, 5};
    void (*ic_event_handler)() = nullptr;
    struct { Ifnet ac_if; } ic_ac;
};
struct ItlHalService : OSObject {
    Ieee80211 ic;
    bool attached = false;
    ~ItlHalService() override {
        assert(!attached && !ic.ic_ac.ac_if.iface && !ic.ic_ac.ac_if.netStat); mark("hal-release");
    }
    bool initWithController(IOService *, IOWorkLoop *, IOCommandGate *) { return !fail("hal-init"); }
    Ieee80211 *get80211Controller() { assert(references); return &ic; }
    bool attach(IOPCIDevice *) { attached = !fail("hal-attach"); return attached; }
    void detach(IOPCIDevice *) {
        assert(attached && happened("events-quiesce") && happened("packet-stop") &&
               happened("scan-stop") && happened("join-stop") && happened("pm-stop"));
        attached = false; mark("hal-detach");
    }
};
struct IOPCIEDeviceWrapper : IOService {
    ItlHalService *fHalService = new ItlHalService;
    ~IOPCIEDeviceWrapper() override { OSSafeReleaseNULL(fHalService); }
};
struct IO80211SkywalkInterface : IOService {
    bool attached = false, started = false, attempted = false;
    bool bsdAttachDeferred = false;
    IOService *controller = nullptr;
    virtual bool init(IOService *provider, void *) {
        controller = provider;
        return !fail("interface-init");
    }
    virtual bool start(IOService *) { return false; }
    virtual void stop(IOService *) {}
    void setInterfaceRole(unsigned role) { assert(role == 1); }
    void setInterfaceId(unsigned id) { assert(id == 1); }
    void deferBSDAttach(bool value) { bsdAttachDeferred = value; mark(value ? "bsd-defer" : "bsd-release"); }
    bool setProperty(const char *, const void *, size_t) { return true; }
    template <typename T> bool setProperty(const char *, T *) { return true; }
    void registerService(unsigned options = 0) override;
    IOReturn setRunningState(bool running) {
        assert(!running);
        mark("interface-running-reset");
        return kIOReturnSuccess;
    }
    bool attach(IOService *) { attached = !fail("interface-attach"); return attached; }
    void detach(IOService *) { assert(attached && !started); attached = false; mark("interface-detach"); }
    ~IO80211SkywalkInterface() override { assert(!attached && !started); mark("interface-release"); }
};
struct AirportItlwmSkywalkInterface : IO80211SkywalkInterface {
    static void *operator new(size_t size) noexcept {
        return fail("interface-allocate") ? nullptr : ::operator new(size);
    }
    static void operator delete(void *ptr) noexcept { ::operator delete(ptr); }
    bool start(IOService *) override {
        attempted = true;
        if (fail("interface-start")) { mark("interface-local-rollback"); return false; }
        started = true; return true;
    }
    void quiesceNativeTimers() { mark("interface-timers-quiesce"); }
    void stop(IOService *) override {
        assert(started && happened("interface-timers-quiesce") && happened("hal-detach"));
        started = false; mark("interface-stop");
    }
};
struct IONetworkController : IOService {
    bool attachInterface(IONetworkInterface **result, bool publish);
    void detachInterface(IONetworkInterface *interface, bool sync) {
        assert(interface && sync); mark("bsd-detach");
    }
};
struct SystemController : IONetworkController {
    bool baseStarted = false;
    bool start(IOService *) {
        if (fail("system-start-early")) return false;
        baseStarted = true;
        return !fail("system-start-late");
    }
    void stop(IOService *) {
        assert(baseStarted); baseStarted = false; mark("system-stop");
    }
    bool attachInterface(IO80211SkywalkInterface *, IOService *) { return !fail("controller-attach"); }
    void detachInterface(IO80211SkywalkInterface *interface, bool sync) {
        assert(interface && sync); mark("controller-detach");
    }
};
namespace SequoiaInterfaceLifecycle {
static bool controllerBaseStarted(SystemController *owner, IOService *) { return owner->baseStarted; }
}

// The production family's parent service publication releases matching, but
// a third-party child may still need the explicit post-registration callback.
// Model the actual child attach and publication separately so the lifecycle
// test rejects a fake child publish with no attach.
void IO80211SkywalkInterface::registerService(unsigned options)
{
    assert(options == 8);
    mark("interface-publish");
}

static OSObject *itlwmCreateFaultAdapter(OSObject *stream, IOWorkLoop *, OSObject **raw) {
    assert(stream);
    if (fail("fault-raw")) return nullptr;
    if (fail("fault-adapter")) return nullptr;
    *raw = new OSObject;
    return new OSObject;
}
static bool itlwmFaultAdapterInstalled(IOService *, OSObject *) { return !fail("fault-install"); }
static bool itlwmAttachFaultReporter(OSObject *raw, IOService *owner) {
    assert(raw && owner && static_cast<SystemController *>(owner)->baseStarted);
    if (fail("fault-attach")) return false;
    mark("fault-attached"); return true;
}
static void itlwmReleaseFaultAdapter(OSObject **adapter, OSObject **raw, IOService *owner) {
    if (owner) {
        assert(!static_cast<SystemController *>(owner)->baseStarted);
        mark("fault-detach");
    }
    if (*adapter || *raw) mark("fault-release");
    OSSafeReleaseNULL(*adapter); OSSafeReleaseNULL(*raw);
}
namespace SequoiaWCL { namespace ScanDelivery {
struct Slot { unsigned value; };
constexpr unsigned MaxSlots = 4;
} }
static void IOFree(void *ptr, size_t size) {
    assert(size == sizeof(SequoiaWCL::ScanDelivery::Slot) * SequoiaWCL::ScanDelivery::MaxSlots);
    assert(happened("hal-detach")); --storageLive;
    delete[] static_cast<SequoiaWCL::ScanDelivery::Slot *>(ptr); mark("storage-free");
}
static void IOLog(const char *) { mark("preserved-storage"); }
static void ether_ifdetach(Ifnet *ifp) { ifp->iface = nullptr; mark("ether-detach"); }
struct ether_addr { unsigned char octet[6]; };
#define IEEE80211_ADDR_COPY(destination, source) memcpy(destination, source, 6)
static bool PE_parse_boot_argn(const char *name, void *value, size_t size)
{
    if ((!strcmp(name, "itlwm_publish_interface_service") ||
         !strcmp(name, "itlwm_clear_running_state")) &&
        value && size >= sizeof(int)) {
        *static_cast<int *>(value) = 1;
        return true;
    }
    return false;
}
static bool itlwmEnsureInterfaceGlue(void *, void **installed) {
    if (installed) *installed = nullptr;
    mark("glue-check");
    return true;
}
static bool itlwmControllerDeliveryChain(void *, void **first, void **second) {
    if (first) *first = nullptr;
    if (second) *second = nullptr;
    mark("controller-chain-check");
    return true;
}
static bool itlwmGlueWakeState(void *, void **gate, unsigned *wake) {
    if (gate) *gate = nullptr;
    if (wake) *wake = 0;
    mark("glue-wake-check");
    return true;
}
static bool itlwmGlueDeliveryChain(void *, void **objectA, void **objectB, void **delivery) {
    if (objectA) *objectA = nullptr;
    if (objectB) *objectB = nullptr;
    if (delivery) *delivery = nullptr;
    mark("glue-delivery-chain-check");
    return true;
}
static bool itlwmGlueMsgBitmap(void *, void **board, uint8_t *bytes, size_t len) {
    if (board) *board = nullptr;
    if (bytes && len) memset(bytes, 0, len);
    mark("glue-msg-bitmap-check");
    return true;
}
struct IOSkywalkTxSubmissionQueue {};
struct IOSkywalkPacket {};
// The production registration helper receives an optional kernel-only
// IO80211InfraInterface callback.  The lifecycle fixture deliberately keeps
// that boundary mocked and records only startup/teardown ordering.
static void *registerNativeInfraEthernet = nullptr;
struct Registration {
    bool prepared = false;
    using Action = UInt (*)(OSObject *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, UInt, void *);
    IOReturn prepare(IO80211SkywalkInterface *, IOWorkLoop *, OSObject *, unsigned char *, Action, void *, void * = nullptr) {
        if (fail("registration")) { mark("registration-local-rollback"); return -1; }
        prepared = true; return 0;
    }
    void reset() {
        if (prepared) {
            assert(happened("bsd-detach") && happened("packet-stop"));
            prepared = false; mark("registration-reset");
        }
    }
};
struct Scan {
    struct Delivery { bool releaseStorage() { return true; } } delivery;
    void hardwareStopped() { assert(happened("hal-detach")); mark("scan-hardware-stopped"); }
};
struct Join { void reset() { assert(happened("hal-detach")); mark("join-reset"); } };
struct AirportItlwm : SystemController {
    IOPCIDevice *pciNub = nullptr;
    IOWorkLoop *_fWorkloop = nullptr, *fWatchdogWorkLoop = nullptr;
    IOCommandGate *_fCommandGate = nullptr;
    IOTimerEventSource *watchdogTimer = nullptr;
    IO80211SkywalkInterface *fNetIf = nullptr;
    AirportItlwmEthernetInterface *bsdInterface = nullptr;
    ItlHalService *fHalService = nullptr;
    void *fpNetStats = nullptr;
    IOLock *nativePMLock = nullptr;
    bool nativeSuperStarted = false, nativeHalAttached = false, nativeLoopRetained = false;
    bool nativeStopping = false, nativeReady = false, nativeGateAdded = false, nativeWatchdogAdded = false;
    bool nativeInterfaceServicePublished = false;
    unsigned nativeBSDAttachAttempts = 0;
    IOReturn nativeBSDAttachLastResult = kIOReturnNotReady;
    IOReturn nativeRunningStateResult = kIOReturnNotReady;
    bool nativeRunningStateApplied = false;
    OSObject *nativeFaultAdapter = nullptr, *nativeRawFaultReporter = nullptr;
    bool nativeFaultReporterAttached = false;
    bool nativeServiceAttached = false, nativeControllerAttached = false, nativeInterfaceStarted = false;
    bool scanActive = false, joinActive = false, packetActive = false, logsActive = false;
    struct FaultReporter : OSObject {
        bool attached = false, started = false;
        bool attach(IOService *) { if (fail("fault-attach")) return false; attached = true; mark("fault-attach"); return true; }
        bool start(IOService *) { if (fail("fault-start")) return false; started = true; mark("fault-start"); return true; }
        void stop(IOService *) { assert(started); started = false; mark("fault-stop"); }
        void detach(IOService *) { assert(attached && !started); attached = false; mark("fault-detach"); }
        ~FaultReporter() override { assert(!attached && !started); }
    } *driverFaultReporter = new FaultReporter;
    Registration nativeRegistration;
    Scan nativeScan;
    Join nativeJoin;
    SequoiaWCL::ScanDelivery::Slot *nativeScanStorage = nullptr;
    bool startNative(IOService *);
    void stopNative(IOService *);
    static IOReturn tsleepHandler(OSObject *, void *, void *, void *, void *) { return 0; }
    static void eventHandler() {}
    UInt nativeTransmitPackets(IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, UInt) { return 0; }
    bool initCCLogs() { logsActive = true; return !fail("logs"); }
    void releaseCCLogs() { logsActive = false; OSSafeReleaseNULL(driverFaultReporter); mark("logs-release"); }
    bool initPCIPowerManagment(IOPCIDevice *) { return !fail("pci-power"); }
    bool createMediumTables(const IONetworkMedium **medium) {
        static IONetworkMedium value; *medium = &value; return !fail("medium-create");
    }
    bool setCurrentMedium(const IONetworkMedium *) { return !fail("medium-current"); }
    bool setSelectedMedium(const IONetworkMedium *) { return !fail("medium-selected"); }
    bool initNativeScan() {
        scanActive = true;
        nativeScanStorage = new SequoiaWCL::ScanDelivery::Slot[SequoiaWCL::ScanDelivery::MaxSlots];
        ++storageLive; return !fail("scan-init");
    }
    bool initNativeJoin() { joinActive = true; return !fail("join-init"); }
    bool initNativePacket() { packetActive = true; return !fail("packet-init"); }
    void stopNativeScan() { scanActive = false; mark("scan-stop"); }
    void stopNativeJoin() { joinActive = false; mark("join-stop"); }
    void stopNativePacket() { packetActive = false; mark("packet-stop"); }
    void unregistPM() { mark("pm-stop"); }
    void quiesceNativeSystemEvents() {
        if (nativeLoopRetained) mark("events-quiesce");
    }
    void releaseNativeSystemEvents() {
        if (nativeLoopRetained) {
            assert(happened("events-quiesce") && !fNetIf && !baseStarted);
            mark("events-release");
        }
    }
    void setLinkStatus(unsigned status) {
        assert(status == kIONetworkLinkValid ||
               status == (kIONetworkLinkValid | kIONetworkLinkActive));
        mark("link-down");
    }
    bool nativeWclCompatR162 = false;
    void publishNativeRegistrationDiagnostics(const char *) { mark("registration-diagnostics"); }
    IOReturn tryNativeBSDChildAttach();
    void announceNativeDriverAvailable(unsigned) { mark("driver-available"); }
    ~AirportItlwm() override {
        assert(!nativeHalAttached && !fHalService && !fNetIf && !bsdInterface && !baseStarted);
        assert(!scanActive && !joinActive && !packetActive && !logsActive && !nativeScanStorage);
        assert(!nativeFaultAdapter && !nativeRawFaultReporter && !driverFaultReporter);
        if (nativePMLock) IOLockFree(nativePMLock);
    }
};
bool IONetworkController::attachInterface(IONetworkInterface **result, bool publish)
{
    if (fail("bsd-attach")) return false;
    mark(publish ? "bsd-allocated-published" : "bsd-allocated-private");
    auto *interface = new AirportItlwmEthernetInterface;
    auto *owner = static_cast<AirportItlwm *>(this);
    interface->owner = owner;
    owner->fHalService->ic.ic_ac.ac_if.iface = interface;
    owner->fHalService->ic.ic_ac.ac_if.netStat = owner->fpNetStats = interface;
    // System attach configures first, then calls interface->attach. Its late
    // failure releases the object without assigning the caller's result.
    if (fail("bsd-attach-late")) { interface->release(); return false; }
    *result = interface;
    // Matching/configd may arrive at the very first publication, before this
    // call even returns. The old mock ignored this side effect (panic23).
    // A deferred Skywalk interface may be attached to its controller before
    // registration storage exists.  Model the real IOSkywalk deferral: the
    // parent relationship is established now, while matching/publication is
    // triggered by the explicit post-registration service notification.
    if (publish && !owner->fNetIf->bsdAttachDeferred) interface->registerService();
    return true;
}
void AirportItlwmEthernetInterface::registerService(unsigned options)
{
    assert(options == 0);
    assert(owner && owner->nativeRegistration.prepared);
    assert(owner->nativeInterfaceStarted && owner->nativeReady && !owner->nativeStopping);
    assert(!happened("bsd-publish"));
    mark("bsd-publish");
    IOService::registerService(options);
}
IOReturn AirportItlwmEthernetInterface::attachToDataLinkLayer(unsigned, void *)
{
    assert(owner && owner->nativeReady && owner->nativeInterfaceStarted &&
           owner->nativeRegistration.prepared && !ifnet);
    ifnet = this;
    mark("bsd-child-attach");
    registerService();
    return 0;
}
IOReturn AirportItlwm::tryNativeBSDChildAttach()
{
    if (!bsdInterface || bsdInterface->getIfnet()) {
        nativeBSDAttachLastResult = bsdInterface ? 0 : kIOReturnNotReady;
        return nativeBSDAttachLastResult;
    }
    ++nativeBSDAttachAttempts;
    nativeBSDAttachLastResult = bsdInterface->attachToDataLinkLayer(0, nullptr);
    return nativeBSDAttachLastResult;
}
#define super SystemController
#include "sequoia_lifecycle_controller_methods.inc"

int main()
{
    const char *points[] = {"pm-lock", "logs", "system-start-early", "system-start-late", "fault-raw", "fault-adapter", "fault-install", "fault-attach", "pci-power",
        "gate-allocate", "gate-add", "medium-create", "medium-current", "medium-selected", "hal-init",
        "hal-attach", "scan-init", "join-init", "packet-init", "watchdog-loop", "watchdog-allocate",
        "watchdog-add", "interface-allocate", "interface-init", "interface-attach", "controller-attach",
        "bsd-attach", "bsd-attach-late", "registration", "interface-start", "success"};
    unsigned traces = 0;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (const char *point : points) {
            events.clear(); failure = point;
            auto *wrapper = new IOPCIEDeviceWrapper;
            auto *pci = new IOPCIDevice;
            auto *loop = new IOWorkLoop;
            auto *owner = new AirportItlwm;
            owner->pciNub = pci; owner->_fWorkloop = loop;
            const bool success = owner->startNative(wrapper);
            assert(success == (failure == "success"));
            assert(owner->nativeReady == success);
            assert(happened("publish") == success);
            if (success) {
                before("registration", "bsd-publish");
                before("interface-start", "bsd-publish");
                assert(happened("bsd-allocated-private"));
                assert(!happened("bsd-allocated-published"));
                assert(happened("interface-publish") && happened("interface-running-reset"));
                before("registration", "bsd-publish");
                before("bsd-publish", "interface-publish");
                assert(owner->nativeHalAttached && owner->nativeRegistration.prepared);
                assert(owner->nativeInterfaceStarted && owner->scanActive && owner->joinActive);
                assert(owner->nativeInterfaceServicePublished);
                assert(owner->nativeRunningStateResult == kIOReturnSuccess);
                assert(owner->fHalService->ic.ic_flags == 0);
            }
            owner->stopNative(wrapper);
            const size_t stopped = events.size();
            owner->stopNative(wrapper);
            assert(events.size() == stopped);
            if (happened("hal-detach")) {
                before("events-quiesce", "hal-detach");
                before("join-stop", "hal-detach");
                if (happened("interface-timers-quiesce")) before("interface-timers-quiesce", "hal-detach");
            }
            if (happened("interface-stop")) before("bsd-detach", "interface-stop");
            if (happened("system-stop")) before("system-stop", "fault-release");
            if (happened("fault-release")) before("fault-release", "logs-release");
            assert(happened("fault-attached") == happened("fault-detach"));
            if (happened("registration-reset")) before("registration-reset", "controller-detach");
            if (happened("system-stop")) before("system-stop", "events-release");
            assert(loop->references == 1 && loop->sources.empty());
            owner->release(); wrapper->release(); pci->release(); loop->release();
            assert(!live && !lockLive && !storageLive && !gateDepth);
            ++traces;
        }
    }
    std::printf("Actual native controller lifecycle: %u startup/failure/duplicate-stop traces passed; Apple/HAL boundaries mocked\n", traces);
}
