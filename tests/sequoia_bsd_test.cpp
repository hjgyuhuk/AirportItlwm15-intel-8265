#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using UInt8 = uint8_t;
using UInt64 = uint64_t;
using IOReturn = int;
using IOOptionBits = unsigned;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = -1, kIOReturnNoMemory = -2,
              kIOReturnBadArgument = -3, kIOReturnCannotLock = -4,
              Failure = static_cast<int>(0xe00002c2U);
constexpr unsigned IFNAMSIZ = 16;
const char *kIOInterfaceUnit = "unit", *kIOInterfaceNamePrefix = "prefix";
static std::vector<int> events;
static int propertyIndex, failProperty;
struct OSObject {
    virtual ~OSObject() = default;
};
struct Ifnet {};
using ifnet_t = Ifnet *;
struct IO80211WorkQueue;
struct IO80211SkywalkInterface;
struct IOService : OSObject {
    IOService *provider = nullptr;
    std::string stage;
    uint32_t result = 0;
    virtual ~IOService() = default;
    IOService *getProvider() { return provider; }
    void setProperty(const char *, const char *value) { stage = value; }
    void setProperty(const char *, UInt64 value, unsigned bits) { assert(bits == 32); result = value; }
};
struct AirportItlwm : IOService {
    bool ready = true, stopping = false, nativeInterfaceStarted = true;
    struct Registration { bool ready = true; bool prepared() { return ready; } } nativeRegistration;
    void *fNetIf = nullptr, *bsdInterface = nullptr;
    IO80211WorkQueue *_fWorkloop = nullptr;
    bool nativeAvailable() { return ready && !stopping; }
    IOReturn prepareNativeBSDInterface(IO80211SkywalkInterface *, ifnet_t);
};
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
const char *ifnet_name(Ifnet *) { return "en"; }
unsigned ifnet_unit(Ifnet *) { return 3; }
struct IOEthernetInterface {
    IOService *owner = nullptr;
    IOService *getController() { return owner; }
    Ifnet storage, *ifnet = nullptr;
    int result = 0;
    bool noIfnet = false, attached = false, rejectSubType = false;
    uint32_t subType = 0;
    std::string publishedName, publishedBSDName, publishedPrefix;
    unsigned publishedUnit = 0xffffffffU;
    bool publishedBuiltIn = false;
    bool setInterfaceSubType(uint32_t value)
    {
        assert(!ifnet && !attached);
        if (rejectSubType) return false;
        subType = value;
        return true;
    }
    IOReturn attachToDataLinkLayer(IOOptionBits, void *)
    {
        // Reproduce airportd's actual admission predicate for Ethernet ifnets.
        assert(subType == 3);
        events.push_back(1);
        if (result) return result;
        attached = true;
        if (!noIfnet) ifnet = &storage;
        return 0;
    }
    void detachFromDataLinkLayer(IOOptionBits, void *)
    {
        events.push_back(4); assert(attached); attached = false; ifnet = nullptr;
    }
    Ifnet *getIfnet() { return ifnet; }
    bool setProperty(const char *key, const char *value)
    {
        if (std::strcmp(key, "IOInterfaceName") == 0) publishedName = value;
        if (std::strcmp(key, "BSD Name") == 0) publishedBSDName = value;
        if (std::strcmp(key, kIOInterfaceNamePrefix) == 0) publishedPrefix = value;
        return true;
    }
    bool setProperty(const char *key, const void *, unsigned size)
    {
        if (std::strcmp(key, "built-in") == 0) publishedBuiltIn = size == sizeof(UInt8);
        return true;
    }
    bool setProperty(const char *key, unsigned value, unsigned bits)
    {
        if (std::strcmp(key, kIOInterfaceUnit) == 0 && bits == 32) publishedUnit = value;
        return true;
    }
    // The production child is an IOService and publishes itself after
    // IOSkywalkEthernetInterface::prepareBSDInterface succeeds.  Keep that
    // publication hook on the child test double as well; the previous test
    // only modeled the parent Skywalk registerService() and therefore could
    // not compile the corrected child registration path.
    virtual void registerService() { events.push_back(5); }
};
struct IOSkywalkNetworkInterface {
    struct Expansion { void *fRegistrationInfo = this; } storage;
    Expansion *mExpansionData = &storage;
};
struct IO80211SkywalkInterface : IOSkywalkNetworkInterface {
    int prepareResult = 0;
    bool prepared = false;
    std::string publishedName;
    bool setProperty(const char *key, const char *value) {
        if (++propertyIndex == failProperty) return false;
        if (std::strcmp(key, "IOInterfaceName") == 0) publishedName = value;
        return true;
    }
    bool setProperty(const char *key, char *value) {
        return setProperty(key, static_cast<const char *>(value));
    }
    template <typename... Args> bool setProperty(Args...) { return ++propertyIndex != failProperty; }
    int prepareBSDInterface(Ifnet *ifnet, unsigned)
    {
        events.push_back(2); assert(ifnet && !prepared); prepared = true; return prepareResult;
    }
    void finalizeBSDInterface(Ifnet *ifnet, unsigned)
    {
        events.push_back(3); assert(ifnet && prepared); prepared = false;
    }
    void registerService() { events.push_back(5); assert(prepared); }
};
struct IO80211WorkQueue {
    bool gate = false, thread = false;
    bool inGate() const { return gate; }
    bool onThread() const { return thread; }
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *owner, void *a = nullptr,
                       void *b = nullptr, void *c = nullptr, void *d = nullptr)
    {
        const bool wasGate = gate;
        gate = true;
        const auto result = action(owner, a, b, c, d);
        gate = wasGate;
        return result;
    }
};
struct AirportItlwmEthernetInterface : IOEthernetInterface {
    IO80211SkywalkInterface *interface = nullptr;
    bool isAttach = false, nativeBSDPrepared = false;
    IOReturn attachToDataLinkLayer(IOOptionBits, void *);
    void detachFromDataLinkLayer(IOOptionBits, void *);
    IOReturn recordNativeBSDResult(const char *, IOReturn);
};

#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define super IOEthernetInterface
#include "sequoia_bsd_production_methods.inc"

int main()
{
    for (int fail = 0; fail <= 9; ++fail) {
        IOService evidence;
        AirportItlwm owner;
        IO80211WorkQueue queue;
        owner._fWorkloop = &queue;
        owner.provider = &evidence;
        IO80211SkywalkInterface skywalk;
        AirportItlwmEthernetInterface bsd;
        bsd.owner = &owner; owner.fNetIf = &skywalk; owner.bsdInterface = &bsd;
        bsd.interface = &skywalk;
        events.clear(); propertyIndex = 0;
        failProperty = fail >= 3 && fail <= 6 ? fail - 2 : 0;
        if (fail == 9) failProperty = 5; // Skywalk control name publication.
        if (fail == 1) bsd.result = Failure;
        if (fail == 2) bsd.noIfnet = true;
        if (fail == 7) skywalk.prepareResult = Failure;
        if (fail == 8) bsd.rejectSubType = true;
        const auto result = bsd.attachToDataLinkLayer(0, nullptr);
        assert(evidence.result == static_cast<uint32_t>(result));
        assert(evidence.stage == owner.stage);
        if (!fail) {
            assert(result == 0 && bsd.isAttach && bsd.nativeBSDPrepared);
            // A property-only Apple80211 name lookup must have exactly one
            // candidate: Skywalk, regardless of service publication order.
            assert(bsd.publishedName.empty() && skywalk.publishedName == "en3");
            assert(bsd.publishedBSDName == "en3" &&
                   bsd.publishedPrefix == "en" &&
                   bsd.publishedUnit == 3 && bsd.publishedBuiltIn);
            assert((events == std::vector<int>{1, 2, 5}));
            assert(bsd.attachToDataLinkLayer(0, nullptr) == kIOReturnNotReady);
            bsd.detachFromDataLinkLayer(0, nullptr);
            assert((events == std::vector<int>{1, 2, 5, 3, 4}));
        } else {
            assert(result != 0 && !bsd.isAttach && !bsd.nativeBSDPrepared);
            if (fail == 1 || fail == 7) assert(result == Failure); // Preserve the full IOReturn.
            if (fail == 1) assert((events == std::vector<int>{1}));
            else if (fail == 7) assert((events == std::vector<int>{1, 2, 3, 4}));
            else if (fail == 8) {
                assert(events.empty());
                assert(evidence.stage == "bsd-wifi-subtype-failed");
            }
            else assert((events == std::vector<int>{1, 4}));
        }
        assert(!skywalk.prepared && !bsd.attached && !bsd.getIfnet());
    }
    // Early, mismatched and stop-time callbacks must never enter Apple BSD
    // preparation. A rejected early call leaves no side effects and can retry
    // once startup has really completed, without a reboot or fake ifnet.
    for (int mode = 0; mode < 9; ++mode) {
        IOService evidence;
        AirportItlwm owner;
        IO80211WorkQueue queue;
        owner._fWorkloop = &queue;
        owner.provider = &evidence;
        IO80211SkywalkInterface skywalk;
        AirportItlwmEthernetInterface bsd;
        bsd.interface = &skywalk; bsd.owner = &owner;
        owner.fNetIf = &skywalk; owner.bsdInterface = &bsd;
        propertyIndex = failProperty = 0; events.clear();
        if (mode == 0) bsd.owner = nullptr;
        if (mode == 1) owner.ready = false;
        if (mode == 2) owner.stopping = true;
        if (mode == 3) owner.nativeInterfaceStarted = false;
        if (mode == 4) owner.nativeRegistration.ready = false;
        if (mode == 5) owner.fNetIf = nullptr;
        if (mode == 6) owner.bsdInterface = nullptr;
        if (mode == 7) skywalk.mExpansionData = nullptr;
        if (mode == 8) skywalk.storage.fRegistrationInfo = nullptr;
        assert(bsd.attachToDataLinkLayer(0, nullptr) == kIOReturnNotReady);
        assert(events.empty() && !bsd.getIfnet() && !bsd.attached && !skywalk.prepared);
        bsd.owner = &owner; owner.ready = owner.nativeInterfaceStarted = owner.nativeRegistration.ready = true;
        owner.stopping = false; owner.fNetIf = &skywalk; owner.bsdInterface = &bsd;
        skywalk.mExpansionData = &skywalk.storage; skywalk.storage.fRegistrationInfo = &skywalk;
        assert(bsd.attachToDataLinkLayer(0, nullptr) == 0);
        assert(evidence.stage == "bsd-prepared-and-published");
        owner.stopping = true;
        bsd.detachFromDataLinkLayer(0, nullptr);
        assert((events == std::vector<int>{1, 2, 5, 3, 4}));
        assert(evidence.stage == "bsd-detached");
    }
    // The Apple WCL thread contract is independently exercised here.  A
    // caller outside the gate is bridged synchronously; a caller already in
    // the gate may prepare directly only when it is not the work queue's own
    // thread.  The latter must return without touching Apple's method.
    {
        IOService evidence;
        AirportItlwm owner;
        IO80211WorkQueue queue;
        owner.provider = &evidence;
        owner._fWorkloop = &queue;
        IO80211SkywalkInterface skywalk;
        owner.fNetIf = &skywalk;
        owner.bsdInterface = reinterpret_cast<void *>(0x1);
        Ifnet ifp;
        events.clear();
        assert(owner.prepareNativeBSDInterface(&skywalk, &ifp) == 0);
        assert(skywalk.prepared && events == std::vector<int>{2});
        skywalk.finalizeBSDInterface(&ifp, 0);

        queue.gate = true;
        queue.thread = false;
        events.clear();
        assert(owner.prepareNativeBSDInterface(&skywalk, &ifp) == 0);
        assert(skywalk.prepared && events == std::vector<int>{2});
        skywalk.finalizeBSDInterface(&ifp, 0);

        queue.thread = true;
        events.clear();
        assert(owner.prepareNativeBSDInterface(&skywalk, &ifp) == kIOReturnCannotLock);
        assert(!skywalk.prepared && events.empty());
        queue.gate = false;
        queue.thread = true;
        assert(owner.prepareNativeBSDInterface(&skywalk, &ifp) == kIOReturnCannotLock);
        assert(!skywalk.prepared && events.empty());

        owner._fWorkloop = nullptr;
        assert(owner.prepareNativeBSDInterface(&skywalk, &ifp) == kIOReturnNotReady);
        assert(owner.prepareNativeBSDInterface(nullptr, &ifp) == kIOReturnBadArgument);
        assert(owner.prepareNativeBSDInterface(&skywalk, nullptr) == kIOReturnBadArgument);
    }
    puts("Actual BSD bridge: 10 attach/rollback + 9 admission/retry/stop cases passed; unique Skywalk control name required; Apple boundaries mocked");
}
