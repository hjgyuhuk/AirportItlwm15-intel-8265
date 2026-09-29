#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

using UInt = unsigned;
using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBusy = -1, kIOReturnBadArgument = -2,
              kIOReturnNoMemory = -3, Injected = -4, kIOReturnNotReady = -5;
static int step, failAt, deregistrations;
static bool infraCallbackUsed;
static bool failure() { return ++step == failAt; }
static std::set<void *> live;
static std::vector<int> cleanup;
struct OSObject {
    int refs = 1, kind;
    explicit OSObject(int k = 0) : kind(k) { live.insert(this); }
    virtual ~OSObject() { assert(live.erase(this) == 1); }
    void retain() { ++refs; }
    void release() { assert(refs > 0); if (!--refs) { cleanup.push_back(kind); delete this; } }
};
struct IOSkywalkPacket {};
struct IOSkywalkPacketBufferPool : OSObject {
    struct PoolOptions { UInt packetCount, bufferCount, bufferSize, maxBuffersPerPacket,
                        memorySegmentSize, poolFlags; uint64_t pad; };
    IOSkywalkPacketBufferPool() : OSObject(1) {}
    static IOSkywalkPacketBufferPool *withName(const char *, OSObject *, UInt type, const PoolOptions *o)
    {
        // Apple netif admits PACKET/RAW pools, produced by factory type 1.
        // Type 0 produces QUANTUM/PAYLOAD and fails before a BSD ifnet exists.
        assert(type == 1 && o->packetCount == 128 && o->bufferCount == 128 &&
               o->bufferSize == 4096 && o->maxBuffersPerPacket == 1 && o->poolFlags == 0);
        return failure() ? nullptr : new IOSkywalkPacketBufferPool;
    }
};
struct IOSkywalkPacketQueue : OSObject {
    IOSkywalkPacketBufferPool *pool;
    bool enabled = false;
    bool rejectEnable = false;
    IOSkywalkPacketQueue(IOSkywalkPacketBufferPool *p, int k) : OSObject(k), pool(p) { pool->retain(); }
    ~IOSkywalkPacketQueue() override { assert(!enabled); pool->release(); }
    void disable() { enabled = false; }
    void enable() { enabled = !rejectEnable; }
    bool isEnabled() const { return enabled; }
};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {
    using Action = UInt (*)(OSObject *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, UInt, void *);
    explicit IOSkywalkTxSubmissionQueue(IOSkywalkPacketBufferPool *p) : IOSkywalkPacketQueue(p, 2) {}
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *p, UInt count, UInt index,
                                               OSObject *, Action action, void *ctx, UInt flags)
    {
        assert(count == 64 && index == 0 && action && !ctx && !flags);
        return failure() ? nullptr : new IOSkywalkTxSubmissionQueue(p);
    }
};
struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {
    using Action = UInt (*)(OSObject *, IOSkywalkRxCompletionQueue *, IOSkywalkPacket **, UInt, void *);
    explicit IOSkywalkRxCompletionQueue(IOSkywalkPacketBufferPool *p) : IOSkywalkPacketQueue(p, 3) {}
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *p, UInt count, UInt index,
                                               OSObject *, Action, void *ctx, UInt flags)
    {
        assert(count == 64 && index == 0 && !ctx && !flags);
        return failure() ? nullptr : new IOSkywalkRxCompletionQueue(p);
    }
};
struct IOWorkLoop {
    std::set<IOSkywalkPacketQueue *> sources;
    int addEventSource(IOSkywalkPacketQueue *q)
    {
        if (failure()) return Injected;
        assert(sources.insert(q).second); q->retain(); return 0;
    }
    void removeEventSource(IOSkywalkPacketQueue *q) { assert(sources.erase(q) == 1); q->release(); }
};
struct IOSkywalkEthernetInterface {
    struct RegistrationInfo { uint8_t pad[0x130]; };
    bool deferred = false, registered = false;
    std::function<bool()> prepared;
    IOSkywalkPacketQueue *queues[2] = {};
    IOSkywalkPacketBufferPool *pools[2] = {};
    bool initRegistrationInfo(RegistrationInfo *info, UInt version, size_t size)
    {
        assert(version == 1 && size == sizeof(*info));
        // Audited Apple defaults before registration: Ethernet family,
        // unspecified subtype and system-assigned BSD unit.
        const uint32_t family = 2, unassignedUnit = UINT32_MAX;
        memcpy(info->pad + 0x08, &family, sizeof(family));
        memcpy(info->pad + 0x38, &unassignedUnit, sizeof(unassignedUnit));
        return !failure();
    }
    void deferBSDAttach(bool value) { deferred = value; }
    int registerEthernetInterface(const RegistrationInfo *info, IOSkywalkPacketQueue **qs, UInt count,
                                  IOSkywalkPacketBufferPool *tx, IOSkywalkPacketBufferPool *rx, UInt flags)
    {
        // Apple 24G830's audited registerInfraEthernetInterface forwards to
        // this generic routine without setting IODeferBSDAttach.  The native
        // controller keeps the child private through attachInterface(...,
        // false), so registration must preserve the normal publication state.
        assert(!deferred && !registered && count == 2 && flags == 0 && info->pad[0x108] == 2);
        uint32_t family, subfamily, unit;
        memcpy(&family, info->pad + 0x08, sizeof(family));
        memcpy(&subfamily, info->pad + 0x0c, sizeof(subfamily));
        memcpy(&unit, info->pad + 0x38, sizeof(unit));
        assert(family == 2 && subfamily == 3 && unit == UINT32_MAX);
        assert(prepared && !prepared()); // In-progress registration is not ready.
        registered = true;
        for (unsigned i = 0; i < 2; ++i) { queues[i] = qs[i]; qs[i]->retain(); }
        pools[0] = tx; pools[1] = rx; tx->retain(); rx->retain();
        return failure() ? Injected : 0; // Error after partial system allocation.
    }
    int deregisterEthernetInterface(UInt)
    {
        assert(registered); registered = false; ++deregistrations;
        for (auto *q : queues) q->release();
        for (auto *p : pools) p->release();
        return 0;
    }
};

static IOReturn fakeInfraBoundary(
    IOSkywalkEthernetInterface *interface,
    IOSkywalkEthernetInterface::RegistrationInfo *info,
    IOSkywalkPacketQueue **queues, unsigned count,
    IOSkywalkPacketBufferPool *tx, IOSkywalkPacketBufferPool *rx)
{
    assert(interface && info && queues && tx && rx);
    infraCallbackUsed = true;
    // Keep the fixture's allocation/error behavior identical to the audited
    // Apple boundary: it normalizes the infra registration, then forwards to
    // the generic Ethernet API, which may allocate before returning an error.
    return interface->registerEthernetInterface(info, queues, count, tx, rx, 0);
}

#define SEQUOIA_SKYWALK_QUEUES_H
#include <Airport/SequoiaSkywalkRegistration.h>

static UInt txAction(OSObject *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, UInt, void *) { return 0; }

int main()
{
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (int fail = 0; fail <= 7; ++fail) {
            step = 0; failAt = fail; deregistrations = 0; cleanup.clear();
            OSObject owner;
            IOSkywalkEthernetInterface interface;
            IOWorkLoop loop;
            const uint8_t mac[] = {2, 1, 2, 3, 4, 5};
            SequoiaSkywalkRegistration registration;
            interface.prepared = [&registration] { return registration.prepared(); };
            const auto result = registration.prepare(&interface, &loop, &owner, mac, txAction, nullptr);
            if (!fail) {
                assert(result == 0 && registration.prepared() && interface.registered);
                assert(!registration.packetQueuesEnabled());
                for (unsigned cycle = 0; cycle < 4; ++cycle) {
                    assert(registration.setPacketQueuesEnabled(true) == 0);
                    assert(registration.packetQueuesEnabled());
                    assert(registration.setPacketQueuesEnabled(false) == 0);
                    assert(!registration.packetQueuesEnabled());
                }
                for (auto *q : interface.queues) {
                    q->rejectEnable = true;
                    assert(registration.setPacketQueuesEnabled(true) == kIOReturnNotReady);
                    assert(!interface.queues[0]->enabled && !interface.queues[1]->enabled);
                    q->rejectEnable = false;
                }
                assert(registration.setPacketQueuesEnabled(true) == 0);
                assert(registration.prepare(&interface, &loop, &owner, mac, txAction, nullptr) == kIOReturnBusy);
            } else {
                assert(result != 0 && !registration.prepared() && !interface.registered);
                if (fail >= 5) assert(result == Injected);
            }
            registration.reset();
            registration.reset();
            assert(!interface.registered && loop.sources.empty() && live.size() == 1);
            assert(deregistrations == ((!fail || fail == 7) ? 1 : 0));
            if (!fail || fail >= 5) assert((cleanup == std::vector<int>{3, 2, 1}));
            step = 0; failAt = 0;
            assert(registration.prepare(&interface, &loop, &owner, mac, txAction, nullptr) == 0);
            registration.reset();
            assert(live.size() == 1);
        }
    }
    assert(live.empty());

    // Exercise the production-only infra registration callback separately from
    // the generic path above.  Both success and an error after the system has
    // accepted the queues must report the infra boundary and deregister exactly
    // once before releasing the queue/pool references.
    for (int fail = 0; fail <= 7; ++fail) {
        step = 0; failAt = fail; deregistrations = 0; cleanup.clear();
        infraCallbackUsed = false;
        OSObject owner;
        IOSkywalkEthernetInterface interface;
        IOWorkLoop loop;
        const uint8_t mac[] = {2, 1, 2, 3, 4, 5};
        SequoiaSkywalkRegistration registration;
        interface.prepared = [&registration] { return registration.prepared(); };
        const auto result = registration.prepare(&interface, &loop, &owner, mac, txAction, nullptr,
                                                 fakeInfraBoundary);
        assert(infraCallbackUsed == (fail == 0 || fail == 7));
        if (!fail) {
            assert(result == 0 && registration.prepared() && registration.usedInfraBoundary());
            assert(interface.registered);
        } else {
            assert(result != 0 && !registration.prepared() && !registration.usedInfraBoundary());
            if (fail >= 5) assert(result == Injected);
        }
        registration.reset();
        assert(!interface.registered && loop.sources.empty() && live.size() == 1);
        assert(deregistrations == ((!fail || fail == 7) ? 1 : 0));
        step = 0; failAt = 0;
    }
    assert(live.empty());

    SequoiaSkywalkRegistration empty;
    assert(empty.setPacketQueuesEnabled(true) == kIOReturnNotReady);
    assert(empty.setPacketQueuesEnabled(false) == 0 && !empty.packetQueuesEnabled());
    assert(empty.prepare(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == kIOReturnBadArgument);
    empty.reset();
}
