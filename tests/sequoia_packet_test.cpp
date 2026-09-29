#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <functional>
#include <vector>

using UInt = unsigned;
using UInt32 = unsigned;
using IOReturn = int;
struct ether_addr { uint8_t octet[6]; };
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNoMemory = 2,
    kIOReturnError = 3, kIOReturnNotReady = 4, kIOReturnCannotLock = 5;
constexpr unsigned kIO80211NetworkLinkUp = 2, kIO80211NetworkLinkDown = 1;
struct DefaultCompletionPanic {};
struct IOSkywalkPacketQueue {
    bool supportsCompletion = false;
    unsigned direction = 2, credits = 0;
    void packetCompletion() {
        if (!supportsCompletion) throw DefaultCompletionPanic{};
        ++credits;
    }
};
static IOSkywalkPacketQueue txSource{true, 1, 0};
struct IOSkywalkPacket {
    std::vector<uint8_t> storage = std::vector<uint8_t>(4096, 0);
    UInt buffers = 1, capacity = 4096, size = 64;
    long long offset = 0;
    bool prepared = true, live = true, nullAddress = false;
    unsigned direction = 1;
    IOSkywalkPacketQueue *source = &txSource;
    unsigned completions = 0;
    IOReturn prepareError = 0, sizeError = 0;
    UInt getPacketBufferCount() const { assert(live); return buffers; }
    UInt getBufferSize() const { assert(live); return capacity; }
    UInt getDataLength() const { assert(live); return size; }
    long long getDataOff() const { assert(live); return offset; }
    void *getDataVirtualAddress() const { assert(live && buffers); return nullAddress ? nullptr : const_cast<uint8_t *>(storage.data()); }
    IOReturn prepare(IOSkywalkPacketQueue *q, unsigned long long options, UInt flags) {
        assert(live && !options && !flags && !prepared);
        // prepare stores the source; it does not set getTransferDirection.
        prepared = true; source = q; return prepareError;
    }
    void completeWithDirection(unsigned completionDirection) {
        assert(live && prepared);
        if (completionDirection == 2 && source) source->packetCompletion();
        prepared = false; ++completions;
    }
    IOReturn setDataOffsetAndLength(unsigned short start, UInt length) {
        assert(live && prepared && !start && length <= capacity);
        if (sizeError) return sizeError;
        offset = start; size = length; return 0;
    }
};
struct IOSkywalkPacketBufferPool {
    IOSkywalkPacket next;
    IOReturn allocateError = 0;
    unsigned allocations = 0, freed = 0, txCredits = 0;
    IOReturn allocatePacket(UInt buffers, IOSkywalkPacket **packet, UInt flags) {
        assert(buffers == 1 && flags == 1 && packet && !*packet);
        ++allocations;
        if (allocateError) return allocateError;
        next.prepared = false; next.live = true; next.source = nullptr;
        next.direction = 0; next.completions = 0; *packet = &next; return 0;
    }
    void deallocatePacket(IOSkywalkPacket *packet) {
        if (!packet) return;
        assert(packet->live);
        if (packet->prepared) {
            packet->completeWithDirection(packet->direction);
            if (packet->direction != 2 && packet->source) {
                packet->source->packetCompletion(); ++txCredits;
            }
        }
        packet->prepared = packet->live = false;
        ++freed;
    }
};
struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {
    bool enabled = true, initialized = true;
    IOReturn enqueueError = 0;
    unsigned enqueued = 0;
    std::vector<uint8_t> received;
    IOReturn enqueuePackets(IOSkywalkPacket *const *packets, UInt count, UInt flags) {
        assert(packets && count == 1 && !flags);
        auto *p = packets[0]; assert(p && p->live && p->prepared);
        if (!enabled || !initialized) return kIOReturnNotReady;
        if (enqueueError) return enqueueError;
        received.assign(p->storage.begin(), p->storage.begin() + p->size);
        p->completeWithDirection(2);
        p->prepared = p->live = false; ++enqueued; return 0;
    }
};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {
    bool enabled = true;
    std::function<IOReturn()> submission;
    unsigned kicks = 0;
    IOReturn packetSubmission(bool fromEvent) { assert(!fromEvent); ++kicks; return submission ? submission() : 0; }
};
#include <Airport/SequoiaPacketIO.h>
#include <Airport/SequoiaTrafficCounters.h>

constexpr unsigned MBUF_PKTHDR = 1, MBUF_DONTWAIT = 1, IFF_RUNNING = 0x40;
constexpr int IEEE80211_S_RUN = 4, LINK_STATE_UP = 2;
struct Buffer { std::vector<uint8_t> bytes; unsigned flags = MBUF_PKTHDR; Buffer *nextPacket = nullptr; };
using mbuf_t = Buffer *;
static unsigned liveMbufs, failAllocation, failCopyback, failCopydata;
static int mbuf_allocpacket(unsigned flags, size_t size, void *, mbuf_t *p) {
    assert(flags == MBUF_DONTWAIT && p && !*p);
    if (failAllocation) return 1;
    *p = new Buffer; (*p)->bytes.resize(size); ++liveMbufs; return 0;
}
static int mbuf_copyback(mbuf_t m, size_t offset, size_t size, const void *data, unsigned flags) {
    assert(m && !offset && flags == MBUF_DONTWAIT && size == m->bytes.size());
    if (failCopyback) return 1;
    memcpy(m->bytes.data(), data, size); return 0;
}
static int mbuf_copydata(mbuf_t m, size_t offset, size_t size, void *data) {
    assert(m && !offset && size == m->bytes.size());
    if (failCopydata) return 1;
    memcpy(data, m->bytes.data(), size); return 0;
}
static void mbuf_freem(mbuf_t m) { assert(m && liveMbufs); --liveMbufs; delete m; }
static unsigned mbuf_flags(mbuf_t m) { return m->flags; }
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->bytes.size(); }
struct mbuf_list { mbuf_t ml_head = nullptr, ml_tail = nullptr; unsigned ml_len = 0; };
static mbuf_t ml_dequeue(mbuf_list *list) {
    auto *packet = list->ml_head;
    if (packet) {
        list->ml_head = packet->nextPacket;
        if (!list->ml_head) list->ml_tail = nullptr;
        packet->nextPacket = nullptr; --list->ml_len;
    }
    return packet;
}
struct InputInterface {
    std::function<UInt32(mbuf_t)> input;
    UInt32 inputPacket(mbuf_t packet, UInt length) {
        assert(!length && !packet->nextPacket);
        return input(packet);
    }
};
struct OutputQueue {
    bool reject = false;
    std::vector<mbuf_t> buffers;
    bool lockEnqueue(mbuf_t m) { if (reject) return false; buffers.push_back(m); return true; }
    ~OutputQueue() { for (auto m : buffers) mbuf_freem(m); }
};
struct AirportItlwm;
using IOEthernetController = AirportItlwm;
struct _ifnet {
    IOEthernetController *controller = nullptr;
    struct Stats { unsigned inputPackets = 0; } stats;
    Stats *netStat = &stats;
    InputInterface *iface = nullptr;
    unsigned if_flags = IFF_RUNNING;
    int if_link_state = LINK_STATE_UP;
    struct { OutputQueue *queue; } if_snd;
    unsigned starts = 0;
    void (*if_start)(_ifnet *) = [](_ifnet *p) { ++p->starts; };
};
struct ieee80211com { void *ic_bss; int ic_state = IEEE80211_S_RUN; _ifnet ic_if;
    uint8_t ic_myaddr[6] = {0xf6, 0x52, 0xda, 0xec, 0x66, 0xd2}; };
struct BSDIfnet {
    unsigned flags = 0x8822;
    uint8_t mac[6] = {0xac, 0x67, 0x5d, 0x69, 0xab, 0x7c};
    bool flagError = false, readError = false, macError = false;
    unsigned updates = 0;
};
static unsigned ifnet_flags(BSDIfnet *p) { return p->flags; }
static int ifnet_set_flags(BSDIfnet *p, unsigned flags, unsigned mask) {
    assert(mask == IFF_RUNNING);
    if (p->flagError) return 1;
    p->flags = (p->flags & ~mask) | (flags & mask); return 0;
}
static int ifnet_lladdr_copy_bytes(BSDIfnet *p, void *bytes, size_t length) {
    assert(length == 6); if (p->readError) return 1;
    memcpy(bytes, p->mac, length); return 0;
}
static int ifnet_set_lladdr(BSDIfnet *p, const void *bytes, size_t length) {
    assert(length == 6); if (p->macError) return 1;
    memcpy(p->mac, bytes, length); ++p->updates; return 0;
}
struct ItlIwm {
    bool portReady = true;
    bool iwm_native_port_ready() { return portReady; }
    bool nativeDetached = false, commandTransportReady = true;
    int nativeStateError = 0;
    uint64_t nativeStateSerial = 1, nativeStateCompleted = 1;
    struct { bool isActive = true, isFailed = false;
        bool active() const { return isActive; } bool failed() const { return isFailed; }
    } nativeJoinProgress;
    struct { bool isBusy = false; bool busy() const { return isBusy; } } nativeFirmwareOwner;
    ieee80211com ic;
    OutputQueue output;
    ItlIwm() { ic.ic_bss = this; ic.ic_if.if_snd.queue = &output; }
    ieee80211com *get80211Controller() { return &ic; }
};
#define OSDynamicCast(type, p) (p)
struct OSObject {};
static unsigned liveTimers;
static bool failTimer;
struct IOTimerEventSource {
    using Action = void (*)(OSObject *, IOTimerEventSource *);
    OSObject *owner;
    Action action;
    bool armed = false, enabled = true;
    UInt delay = 0;
    IOReturn error = 0;
    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action) {
        if (failTimer) return nullptr;
        ++liveTimers; return new IOTimerEventSource{owner, action};
    }
    IOReturn setTimeoutMS(UInt ms) { assert(enabled); if (error) return error; armed = true; delay = ms; return 0; }
    void cancelTimeout() { armed = false; }
    void disable() { enabled = false; }
    void release() { assert(liveTimers); --liveTimers; delete this; }
    void fire() { assert(armed && enabled); armed = false; action(owner, this); }
};
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
struct TestWorkLoop {
    bool gate = true;
    IOReturn addError = 0;
    unsigned added = 0, removed = 0;
    bool inGate() const { return gate; }
    IOReturn addEventSource(IOTimerEventSource *) { if (addError) return addError; ++added; return 0; }
    void removeEventSource(IOTimerEventSource *) { ++removed; }
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *owner) {
        bool old = gate; gate = true; auto result = action(owner, nullptr, nullptr, nullptr, nullptr); gate = old; return result;
    }
};
struct AirportItlwm : OSObject {
    _ifnet::Stats nativeNetworkStats;
    ItlIwm driver;
    ItlIwm *fHalService = &driver;
    bool radioAvailable = true, nativeHalAttached = true;
    bool available = true;
    bool nativeInterfaceStarted = true;
    bool nativeSkywalkBSDOwner = false;
    struct Interface {
        BSDIfnet value;
        bool attached = true;
        BSDIfnet *getBSDInterface() { return attached ? &value : nullptr; }
        bool stateError = false;
        IOReturn runningError = 0, reportError = 0;
        unsigned state = 1, reason = 0, status = 1, changes = 0, runs = 0, reports = 0;
        bool running = false;
        bool hasAP = false;
        void setCurrentApAddress(ether_addr *address) { hasAP = address != nullptr; }
        bool setLinkState(unsigned next, UInt why, bool debounce, UInt code) {
            assert(!debounce && !code); ++changes;
            if (stateError) return false;
            state = next; reason = why; return true;
        }
        IOReturn setRunningState(bool next) { ++runs; if (runningError) return runningError; running = next; return 0; }
        IOReturn reportLinkStatus(UInt next, UInt media) {
            assert(media == 0x80 && (next & 0x80000000u)); ++reports;
            if (reportError) return reportError;
            status = next; return 0;
        }
    } interface;
    Interface *fNetIf = &interface;
    struct BSDInterface {
        BSDIfnet value;
        bool attached = true;
        unsigned outputThreadStops = 0, outputThreadStarts = 0;
        void stopOutputThread() { ++outputThreadStops; }
        void startOutputThread() { ++outputThreadStarts; }
        BSDIfnet *getIfnet() { return attached ? &value : nullptr; }
    } bsd;
    BSDInterface *bsdInterface = &bsd;
    TestWorkLoop loop;
    TestWorkLoop *_fWorkloop = &loop;
    IOTimerEventSource *nativePacketTimer = nullptr;
    bool nativePacketTimerAdded = false, nativeTxRetry = false;
    IOReturn nativePacketError = kIOReturnSuccess;
    SequoiaPacketIO::ReceiveDiagnostics nativeRxDiagnostics;
    SequoiaTrafficCounters nativeTrafficCounters;
    struct {
        IOSkywalkPacketBufferPool p;
        IOSkywalkTxSubmissionQueue tx;
        IOSkywalkRxCompletionQueue rx;
        bool ready = true;
        bool enableError = false;
        bool packetQueuesEnabled() const { return ready && tx.enabled && rx.enabled; }
        IOReturn setPacketQueuesEnabled(bool enabled) {
            tx.enabled = rx.enabled = enabled && ready && !enableError;
            return enabled && (!ready || enableError) ? kIOReturnNotReady : 0;
        }
        bool prepared() const { return ready; }
        IOSkywalkPacketBufferPool *pool() { return &p; }
        IOSkywalkTxSubmissionQueue *txQueue() { return &tx; }
        IOSkywalkRxCompletionQueue *rxQueue() { return &rx; }
    } nativeRegistration;
    bool nativeRadioAvailable() { return radioAvailable; }
    bool nativeAvailable() { return available; }
    TestWorkLoop *getWorkQueue() { return &loop; }
    bool initNativePacket();
    void stopNativePacket();
    void retryNativeTransmit(UInt);
    static void nativePacketTimerAction(OSObject *, IOTimerEventSource *);
    bool nativeTrafficAvailable();
    bool nativePortAvailable();
    struct {
        bool connected = true, completionPosted = true, cancel = false;
        int error = 0;
        struct { struct { uint8_t bssid[6] = {2}; } candidates[1]; } completion;
    } nativeJoin;
    IOReturn publishNativeLink(bool, UInt);
    IOReturn setNativeDataPath(bool);
    UInt nativeTransmitPackets(IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, UInt);
    UInt32 nativeReceivePacket(mbuf_t);
    ~AirportItlwm() { available = false; stopNativePacket(); }
};
#include "../AirportItlwm/native_packet_sequoia.inc"
using IO80211InfraInterface = AirportItlwm::Interface;
#include "../AirportItlwm/native_link_sequoia.inc"
#include "../itl80211/openbsd/sys/mbuf_input_sequoia.inc"

int main()
{
    // 64-bit accounting and lifetime deltas, including unsigned rollover.
    {
        SequoiaTrafficCounters c;
        c.tx = UINT32_MAX; c.transmitted();
        c.rx = UINT64_MAX; c.received();
        uint64_t out[7]; c.snapshot(out, 1234567890123ull);
        assert(out[2] == uint64_t(UINT32_MAX)+1 && out[3] == 0);
        assert(out[0] == 0 && out[1] == 0 && out[4] == 0 && out[5] == 0);
        assert(out[6] == 1234567890123ull);
    }
    using namespace SequoiaPacketIO;
    unsigned traces = 0;
    // Reproduce r202 in the model of Apple's actual completion callbacks.
    // Both pool error cleanup and successful RX enqueue notify the bogus source.
    for (bool enqueue : {false, true}) {
        IOSkywalkPacketBufferPool pool; IOSkywalkRxCompletionQueue queue;
        IOSkywalkPacket *p = nullptr;
        assert(!pool.allocatePacket(1, &p, 1));
        assert(!p->prepare(&queue, 0, 0));
        bool trapped = false;
        try {
            if (enqueue) queue.enqueuePackets(&p, 1, 0);
            else pool.deallocatePacket(p);
        } catch (const DefaultCompletionPanic &) { trapped = true; }
        assert(trapped);
        // Dispose the simulated failed attempt without leaving owned memory.
        p->source = nullptr; pool.deallocatePacket(p);
        assert(pool.freed == 1);
    }
    // Production RX dispatch must not need the missing legacy child. Test
    // successful copy, queue failure, closed admission and absent controller.
    for (unsigned failure = 0; failure < 4; ++failure) {
        AirportItlwm controller;
        auto &ifp = controller.driver.ic.ic_if;
        ifp.controller = &controller;
        ifp.netStat = nullptr;
        assert(controller.initNativePacket());
        assert(ifp.netStat == &controller.nativeNetworkStats);
        if (failure == 1) controller.nativeRegistration.p.allocateError = kIOReturnNoMemory;
        if (failure == 2) controller.nativeJoin.connected = false;
        if (failure == 3) ifp.controller = nullptr;
        mbuf_list list;
        for (unsigned i = 0; i < 64; ++i) {
            mbuf_t packet = nullptr;
            assert(!mbuf_allocpacket(MBUF_DONTWAIT, 64, nullptr, &packet));
            if (list.ml_tail) list.ml_tail->nextPacket = packet;
            else list.ml_head = packet;
            list.ml_tail = packet; ++list.ml_len;
        }
        assert(_if_input(nullptr, &ifp, &list, nullptr, nullptr) == 0);
        assert(!liveMbufs && !list.ml_len && !list.ml_head && !list.ml_tail);
        assert(ifp.netStat->inputPackets == (failure ? 0u : 64u));
        assert(controller.nativeRegistration.rx.enqueued == (failure ? 0u : 64u));
        ++traces;
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        AirportItlwm controller;
        auto &ifp = controller.driver.ic.ic_if;
        InputInterface interface;
        unsigned legacyInputs = 0;
        // r207 contract: inputPacket tries nativeReceivePacket first and hands
        // the mbuf to the legacy IOEthernetInterface input path when the native
        // Skywalk delivery is not ready; the legacy path consumes the mbuf.
        interface.input = [&](mbuf_t packet) {
            if (controller.nativeReceivePacket(packet)) return 1;
            ++legacyInputs;
            mbuf_freem(packet);
            return 1;
        };
        ifp.iface = &interface;
        if (failure == 1) ifp.iface = nullptr;
        if (failure == 2) controller.nativeJoin.connected = false;
        if (failure == 3) controller.nativeRegistration.p.allocateError = kIOReturnNoMemory;
        if (failure == 4) ifp.netStat = nullptr;
        mbuf_list list;
        for (unsigned i = 0; i < 64; ++i) {
            mbuf_t packet = nullptr;
            assert(!mbuf_allocpacket(MBUF_DONTWAIT, 64, nullptr, &packet));
            packet->bytes[0] = static_cast<uint8_t>(i);
            if (list.ml_tail) list.ml_tail->nextPacket = packet;
            else list.ml_head = packet;
            list.ml_tail = packet; ++list.ml_len;
        }
        assert(_if_input(nullptr, &ifp, &list, nullptr, nullptr) == kIOReturnSuccess);
        assert(!liveMbufs && !list.ml_head && !list.ml_tail && !list.ml_len);
        assert(ifp.stats.inputPackets == (failure == 1 || failure == 4 ? 0 : 64));
        assert(legacyInputs == (failure == 2 || failure == 3 ? 64 : 0));
        assert(controller.nativeRegistration.rx.enqueued == (failure == 0 || failure == 4 ? 64 : 0));
        ++traces;
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        AirportItlwm controller;
        if (failure == 1) controller.nativeJoin.connected = false;
        if (failure == 2) controller.nativeJoin.completionPosted = false;
        if (failure == 3) controller.nativeJoin.cancel = true;
        if (failure == 4) controller.nativeJoin.error = 5;
        assert(controller.nativePortAvailable());
        assert(controller.nativeTrafficAvailable() == !failure);
        // Publishing the link must not depend on an already-open packet gate.
        assert(controller.publishNativeLink(true, 0) == kIOReturnSuccess);
        ++traces;
    }
    // Exercise the Apple-owned ifnet without a custom BSD output thread.
    for (unsigned failure = 0; failure < 6; ++failure) {
        AirportItlwm controller;
        controller.nativeSkywalkBSDOwner = true;
        controller.bsdInterface = nullptr;
        auto &bsd = controller.interface.value;
        controller.nativeRegistration.setPacketQueuesEnabled(false);
        if (failure == 1) controller.interface.attached = false;
        if (failure == 2) bsd.readError = true;
        if (failure == 3) bsd.macError = true;
        if (failure == 4) bsd.flagError = true;
        if (failure == 5) controller.nativeRegistration.enableError = true;
        assert((controller.publishNativeLink(true, 0) == 0) == !failure);
        assert(controller.nativeRegistration.packetQueuesEnabled() == !failure);
        assert(bool(bsd.flags & IFF_RUNNING) == !failure);
        assert((bsd.flags & ~IFF_RUNNING) == 0x8822);
        assert(!controller.bsd.outputThreadStarts && !controller.bsd.outputThreadStops);
        if (!failure) {
            assert(!memcmp(bsd.mac, controller.driver.ic.ic_myaddr, 6));
            controller.available = false; // Teardown closes queues after admission closes.
            assert(controller.setNativeDataPath(false) == 0);
            assert(!controller.nativeRegistration.packetQueuesEnabled());
            assert(bsd.flags == 0x8822);
        }
        ++traces;
    }
    for (unsigned failure = 0; failure < 16; ++failure) {
        AirportItlwm controller;
        controller.nativeRegistration.setPacketQueuesEnabled(false);
        switch (failure) {
            case 1: controller.loop.gate = false; break;
            case 2: controller.available = false; break;
            case 3: controller.nativeInterfaceStarted = false; break;
            case 4: controller.fNetIf = nullptr; break;
            case 5: controller.nativeRegistration.ready = false; break;
            case 6: controller.driver.ic.ic_if.if_link_state = 0; break;
            case 7: controller.interface.stateError = true; break;
            case 8: controller.interface.runningError = kIOReturnError; break;
            case 9: controller.interface.reportError = kIOReturnNotReady; break;
            case 10: controller.bsdInterface = nullptr; break;
            case 11: controller.bsd.attached = false; break;
            case 12: controller.bsd.value.readError = true; break;
            case 13: controller.bsd.value.macError = true; break;
            case 14: controller.nativeRegistration.enableError = true; break;
            case 15: controller.bsd.value.flagError = true; break;
        }
        assert((controller.publishNativeLink(true, 0) == 0) == !failure);
        assert(controller.nativeRegistration.packetQueuesEnabled() == !failure);
        assert(bool(ifnet_flags(&controller.bsd.value) & IFF_RUNNING) == !failure);
        assert((controller.bsd.value.flags & ~IFF_RUNNING) == 0x8822);
        if (failure == 9) {
            controller.interface.reportError = 0;
            assert(controller.publishNativeLink(true, 0) == 0);
        }
        if (!failure || failure == 9) {
            assert(controller.nativeRegistration.packetQueuesEnabled());
            assert(!memcmp(controller.bsd.value.mac, controller.driver.ic.ic_myaddr, 6));
            assert(controller.bsd.value.updates == 1);
            assert(controller.interface.running && controller.interface.state == 2 && controller.interface.status == 0x80000003u);
            controller.radioAvailable = false;
            assert(controller.publishNativeLink(false, 3) == 0);
            assert(!controller.interface.running && controller.interface.state == 1 && controller.interface.reason == 3);
            assert(controller.interface.status == 0x80000001u);
            assert(!controller.nativeRegistration.packetQueuesEnabled());
            assert(controller.bsd.value.flags == 0x8822);
        }
        ++traces;
    }
    // Reproduce the actual r203 receive refusal: enabled and initialized are
    // separate Apple admission gates. Null-source cleanup remains safe for both.
    for (unsigned disabled = 0; disabled < 2; ++disabled) {
        IOSkywalkPacketBufferPool pool; IOSkywalkRxCompletionQueue queue;
        queue.enabled = disabled != 0; queue.initialized = disabled == 0;
        assert(receive(&pool, &queue, 64, [](uint8_t *, size_t) { return true; }) == kIOReturnNotReady);
        assert(pool.freed == 1 && !queue.enqueued);
    }
    {
        AirportItlwm controller;
        assert(controller.initNativePacket());
        assert(controller.publishNativeLink(true, 0) == 0);
        controller.available = false;
        controller.stopNativePacket();
        assert(!controller.nativeRegistration.packetQueuesEnabled());
        assert(controller.bsd.value.flags == 0x8822);
    }
    // Exact allocation ASan checks ensure offsets never leave the owned buffer.
    for (unsigned capacity : {0u, 13u, 14u, 1518u, 4096u, 4097u, UINT_MAX})
        for (long long offset : {-1LL, 0LL, 1LL, 14LL, 2578LL, 4096LL, LLONG_MAX})
            for (unsigned length : {0u, 13u, 14u, 1518u, 1519u, UINT_MAX})
                for (unsigned buffers : {0u, 1u, 2u}) {
                    IOSkywalkPacket packet;
                    packet.capacity = capacity; packet.offset = offset; packet.size = length; packet.buffers = buffers;
                    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(1); size_t size = 99;
                    const bool valid = buffers == 1 && capacity && capacity <= 4096 && offset >= 0 &&
                        static_cast<uint64_t>(offset) <= capacity && length >= 14 && length <= 1518 &&
                        length <= capacity - static_cast<uint64_t>(offset);
                    assert(transmitView(&packet, bytes, size) == valid);
                    if (valid) { assert(bytes == packet.storage.data() + offset && size == length); volatile uint8_t last = bytes[size - 1]; (void)last; }
                    else assert(!bytes && !size);
                    ++traces;
                }
    for (unsigned fault = 0; fault < 12; ++fault) {
        IOSkywalkPacketBufferPool pool; IOSkywalkRxCompletionQueue queue;
        switch (fault) {
            case 1: pool.allocateError = kIOReturnNoMemory; break;
            case 2: pool.next.prepareError = kIOReturnError; break;
            case 3: pool.next.buffers = 2; break;
            case 4: pool.next.capacity = 32; break;
            case 5: pool.next.sizeError = kIOReturnError; break;
            case 6: pool.next.nullAddress = true; break;
            case 8: queue.enqueueError = kIOReturnNotReady; break;
            case 9: pool.next.buffers = 0; break;
            case 10: pool.next.capacity = 4097; break;
            case 11: pool.next.capacity = 0; break;
        }
        unsigned copies = 0;
        ReceiveDiagnostics diagnostic;
        auto result = receive(&pool, &queue, 64, [&](uint8_t *bytes, size_t size) {
            assert(size == 64); ++copies; memset(bytes, 0xa5, size); return fault != 7;
        }, &diagnostic);
        assert((result == 0) == !fault && pool.allocations == 1);
        assert(pool.freed == (fault > 1) && queue.enqueued == !fault && !pool.txCredits);
        if (!fault) assert(queue.received == std::vector<uint8_t>(64, 0xa5));
        assert(copies == (!fault || fault == 7 || fault == 8));
        const unsigned stages[] = {7, 2, 3, 4, 4, 5, 6, 6, 7, 4, 4, 4};
        assert(diagnostic.stage == stages[fault] && diagnostic.result == unsigned(result));
        assert(diagnostic.attempts == 1 && diagnostic.delivered == !fault && diagnostic.failed == !!fault);
        if (fault) assert(diagnostic.failureStage == stages[fault]);
        if (fault != 1) assert(pool.next.completions == 1 && !pool.next.source);
        ++traces;
    }
    {
        IOSkywalkPacketBufferPool pool; IOSkywalkRxCompletionQueue queue;
        ReceiveDiagnostics d;
        for (unsigned i = 0; i < 1000; ++i) {
            queue.enqueueError = i % 2 ? kIOReturnNotReady : 0;
            assert(receive(&pool, &queue, 1518, [](uint8_t *bytes, size_t n) {
                memset(bytes, 0x5a, n); return true;
            }, &d) == queue.enqueueError);
            assert(!pool.next.live && !pool.next.prepared && pool.next.completions == 1);
        }
        assert(d.attempts == 1000 && d.delivered == 500 && d.failed == 500);
        assert(pool.freed == 500 && queue.enqueued == 500 && !pool.txCredits);
        queue.enqueueError = 0;
        assert(!receive(&pool, &queue, 14, [](uint8_t *, size_t) { return true; }, &d));
        assert(d.result == 0 && d.failureResult == kIOReturnNotReady && d.failureStage == 7);
        d.attempts = d.delivered = d.failed = UINT32_MAX;
        assert(receive(&pool, &queue, 1519, [](uint8_t *, size_t) { assert(false); return false; }, &d) == kIOReturnBadArgument);
        assert(d.stage == 1 && d.attempts == UINT32_MAX && d.failed == UINT32_MAX);
        assert(!receive(&pool, &queue, 14, [](uint8_t *, size_t) { return true; }, &d));
        assert(d.delivered == UINT32_MAX);
        traces += 1003;
    }
    for (unsigned retry = 0; retry <= 4; ++retry) {
        IOSkywalkPacketBufferPool pool;
        IOSkywalkPacket p[4]; IOSkywalkPacket *packets[4] = {&p[0], &p[1], &p[2], &p[3]};
        unsigned callbacks = 0;
        assert(transmit(&pool, packets, 4, [&](const uint8_t *, size_t) {
            return callbacks++ == retry ? Submission::Retry : Submission::Accepted;
        }) == retry);
        assert(pool.freed == retry && pool.txCredits == retry);
        for (unsigned i = 0; i < 4; ++i) assert(p[i].live == (i >= retry));
        assert(transmit(&pool, packets + retry, 4 - retry, [](const uint8_t *, size_t) { return Submission::Dropped; }) == 4 - retry);
        assert(pool.freed == 4 && pool.txCredits == 4);
        ++traces;
    }
    for (unsigned fault = 0; fault < 20; ++fault) {
        AirportItlwm d; IOSkywalkPacket packet; IOSkywalkPacket *packets[] = {&packet};
        assert(d.initNativePacket());
        auto *queue = d.nativeRegistration.txQueue(); IOSkywalkTxSubmissionQueue wrong;
        switch (fault) {
            case 1: d.radioAvailable = false; break;
            case 2: d.nativeHalAttached = false; break;
            case 3: d.nativeRegistration.ready = false; break;
            case 4: d.fHalService = nullptr; break;
            case 5: d.driver.nativeDetached = true; break;
            case 6: d.driver.nativeStateError = 1; break;
            case 7: d.driver.commandTransportReady = false; break;
            case 8: d.driver.nativeJoinProgress.isActive = false; break;
            case 9: d.driver.nativeJoinProgress.isFailed = true; break;
            case 10: ++d.driver.nativeStateSerial; break;
            case 11: d.driver.nativeFirmwareOwner.isBusy = true; break;
            case 12: d.driver.ic.ic_bss = nullptr; break;
            case 13: d.driver.ic.ic_state = 0; break;
            case 14: d.driver.ic.ic_if.if_flags = 0; break;
            case 15: d.driver.ic.ic_if.if_link_state = 0; break;
            case 16: queue = &wrong; break;
            case 17: failAllocation = 1; break;
            case 18: failCopyback = 1; break;
            case 19: d.driver.output.reject = true; break;
        }
        packet.offset = 37; memset(packet.storage.data() + 37, 0xab, 64);
        assert(d.nativeTransmitPackets(queue, packets, 1) == !fault);
        assert(d.nativeRegistration.p.txCredits == !fault && packet.live == bool(fault));
        assert(d.driver.ic.ic_if.starts == !fault);
        assert(d.nativeTrafficCounters.tx == !fault);
        assert(d.nativeTxRetry == (fault != 0 && fault != 16));
        if (!fault) {
            assert(d.driver.output.buffers.size() == 1);
            memset(packet.storage.data(), 0, packet.storage.size());
            assert(d.driver.output.buffers[0]->bytes == std::vector<uint8_t>(64, 0xab));
        }
        failAllocation = failCopyback = 0;
        ++traces;
    }
    assert(!liveMbufs);
    for (unsigned fault = 0; fault < 5; ++fault) {
        AirportItlwm d; mbuf_t packet = nullptr;
        assert(!mbuf_allocpacket(MBUF_DONTWAIT, 64, nullptr, &packet));
        memset(packet->bytes.data(), 0xcd, 64);
        switch (fault) {
            case 1: d.radioAvailable = false; break;
            case 2: packet->flags = 0; break;
            case 3: failCopydata = 1; break;
            case 4: d.nativeRegistration.rx.enqueueError = kIOReturnNotReady; break;
        }
        // r207 contract: on failure nativeReceivePacket returns the mbuf to the
        // caller for the legacy input path, so the caller frees it here.
        const auto delivered = d.nativeReceivePacket(packet);
        assert(delivered == !fault);
        assert(d.nativeTrafficCounters.rx == !fault);
        if (!delivered) mbuf_freem(packet);
        assert(!liveMbufs);
        if (!fault) assert(d.nativeRegistration.rx.received == std::vector<uint8_t>(64, 0xcd));
        failCopydata = 0; ++traces;
    }
    for (unsigned fault = 0; fault < 2; ++fault) {
        AirportItlwm d;
        failTimer = fault == 0;
        if (fault == 1) d.loop.addError = kIOReturnError;
        assert(!d.initNativePacket());
        d.stopNativePacket(); d.stopNativePacket();
        assert(!d.nativePacketTimer && !d.nativePacketTimerAdded && !d.loop.removed);
        failTimer = false; ++traces;
    }
    assert(!liveTimers);
    {
        AirportItlwm d; IOSkywalkPacket packet; IOSkywalkPacket *packets[] = {&packet};
        assert(d.initNativePacket());
        auto *queue = d.nativeRegistration.txQueue();
        queue->submission = [&] { d.nativeTransmitPackets(queue, packets, 1); return 0; };
        failAllocation = 1;
        assert(!d.nativeTransmitPackets(queue, packets, 1) && d.nativeTxRetry && d.nativePacketTimer->armed);
        d.nativePacketTimer->fire();
        assert(queue->kicks == 1 && packet.live && d.nativePacketTimer->armed);
        failAllocation = 0;
        d.driver.nativeFirmwareOwner.isBusy = true;
        d.nativePacketTimer->fire();
        assert(queue->kicks == 1 && d.nativePacketTimer->delay == 100);
        d.driver.nativeFirmwareOwner.isBusy = false;
        d.nativePacketTimer->fire();
        assert(queue->kicks == 2 && !packet.live && !d.nativeTxRetry && !d.nativePacketTimer->armed);
        assert(d.driver.output.buffers.size() == 1 && d.nativeRegistration.p.txCredits == 1);
        d.retryNativeTransmit(10);
        d.available = false;
        d.nativePacketTimer->fire();
        assert(queue->kicks == 2 && !d.nativePacketTimer->armed);
        d.stopNativePacket(); d.stopNativePacket();
        assert(!d.nativeTxRetry && d.loop.added == 1 && d.loop.removed == 1);
        ++traces;
    }
    {
        AirportItlwm d;
        assert(d.initNativePacket());
        d.nativePacketTimer->error = kIOReturnError;
        d.retryNativeTransmit(10);
        assert(d.nativePacketError == kIOReturnError && !d.nativeTrafficAvailable());
        ++traces;
    }
    assert(!liveTimers && !liveMbufs);
    printf("Native packet adapter/controller: %u length/offset/ownership/retry/admission/fault traces; no double free, source queue credits preserved, independent mbuf copies; Skywalk and hardware mocked\n", traces);
}
