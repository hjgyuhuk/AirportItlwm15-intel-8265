#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <future>
#include <thread>
#include <atomic>
#include <Airport/SequoiaInterfaceLifecycle.h>

using namespace SequoiaInterfaceLifecycle;
static void IOLog(const char *, ...) {}
template <typename T> static void put(void *storage, size_t offset, T value)
{
    memcpy(static_cast<uint8_t *>(storage) + offset, &value, sizeof(value));
}

struct OSObject {
    virtual void release() { assert(references == 1); --references; ++releases; }
    uint64_t references = 1;
    void *ivars = nullptr;
    unsigned releases = 0;
};
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
static thread_local unsigned gateDepth;
struct WorkQueue {
    std::recursive_mutex mutex;
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *owner) {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        ++gateDepth;
        auto result = action(owner, nullptr, nullptr, nullptr, nullptr);
        --gateDepth;
        return result;
    }
} workQueue;
static WorkQueue *_fWorkloop = &workQueue;
struct IOService : OSObject { WorkQueue *getWorkQueue() { return _fWorkloop; } };
struct IOTimerEventSource : OSObject {
    bool enabled = true;
    unsigned cancels = 0, disables = 0;
    void cancelTimeout() { assert(gateDepth); ++cancels; }
    void disable() { assert(gateDepth); enabled = false; ++disables; }
    void release() override { assert(!enabled && cancels); OSObject::release(); }
};
struct CCPipe : OSObject {
    unsigned stops = 0;
    void stopPipe() { assert(references == 1); ++stops; }
};
struct IOSkywalkEthernetInterface : IOService {
    unsigned ethernetStops = 0;
    void stop(IOService *) { ++ethernetStops; }
};
struct IO80211InfraProtocol : IOSkywalkEthernetInterface {
    uint8_t skywalkStorage[0xf0] = {}, infraStorage[0x3c68] = {};
    void *skywalkIvars = skywalkStorage, *infraIvars = infraStorage;
    OSObject peer, stream, scan, bonjour;
    CCPipe pipe;
    IOTimerEventSource linkTimer, dataTimer;
    unsigned failStage = 0, starts = 0, infraStops = 0, scanStops = 0;

    bool start(IOService *provider) {
        ++starts;
        if (failStage == 0) return false;
        put(skywalkIvars, Controller, provider);
        if (failStage == 1) return false;
        put(skywalkIvars, LogPipe, &pipe);
        if (failStage == 2) return false; // Log stream setup fails before startPipe.
        put(skywalkIvars, LogStream, &stream);
        if (failStage == 3) return false; // Peer factory fails.
        put(skywalkIvars, PeerManager, &peer);
        if (failStage <= 6) return false; // Flow DB, dictionary, glue/agent failure.
        put(infraIvars, BonjourAgent, &bonjour);
        if (failStage == 7) return false;
        put(infraIvars, ScanManager, &scan);
        if (failStage == 8) return false; // ScanManager::commonInit ivar failure.
        scan.ivars = &scan;
        if (failStage >= 10) put(infraIvars, InfraLinkTimer, &linkTimer);
        if (failStage >= 12) put(infraIvars, InfraDataTimer, &dataTimer);
        if (failStage <= 11) return false; // Scan timers/reporters or Infra timer.
        return true;
    }
    void stop(IOService *provider) {
        assert(read<void *>(skywalkIvars, Controller) == provider);
        assert(read<void *>(skywalkIvars, PeerManager));
        if (failStage >= 10) assert(!linkTimer.enabled && linkTimer.cancels);
        if (failStage >= 12) assert(!dataTimer.enabled && dataTimer.cancels);
        auto *currentScan = read<OSObject *>(infraIvars, ScanManager);
        if (currentScan) { assert(currentScan->ivars); ++scanStops; }
        ++infraStops;
        auto *currentPipe = take<CCPipe>(skywalkIvars, LogPipe);
        if (currentPipe) { currentPipe->stopPipe(); currentPipe->release(); }
        auto *currentStream = take<OSObject>(skywalkIvars, LogStream);
        if (currentStream) currentStream->release();
        take<OSObject>(skywalkIvars, PeerManager)->release();
        IOSkywalkEthernetInterface::stop(provider);
    }
    void releaseRemainingSystemFields() {
        auto *link = take<IOTimerEventSource>(infraIvars, InfraLinkTimer);
        auto *data = take<IOTimerEventSource>(infraIvars, InfraDataTimer);
        if (link) link->release();
        if (data) data->release();
        auto *currentScan = take<OSObject>(infraIvars, ScanManager);
        if (currentScan) currentScan->release();
        assert(!read<void *>(infraIvars, BonjourAgent));
        assert(!read<void *>(skywalkIvars, LogPipe));
        assert(!read<void *>(skywalkIvars, LogStream));
        assert(!read<void *>(skywalkIvars, PeerManager));
    }
};
struct AirportItlwmSkywalkInterface : IO80211InfraProtocol {
    IOService *instance = nullptr;
    bool nativeStartAttempted = false, nativeStopDone = false;
    bool nativeTimersQuiesced = false;
    bool start(IOService *);
    void stop(IOService *);
    void quiesceNativeTimers();
    void dumpNativeIvars(uint64_t *, uint64_t *, uint32_t *, uint64_t *, uint64_t *);
};
#define super IO80211InfraProtocol
#include "../AirportItlwm/native_interface_lifecycle_sequoia.inc"

struct AirportItlwm : IOService {
    uint8_t padding[0x120 - sizeof(IOService)] = {};
    void *controllerIvars = nullptr;
    bool nativeLoopRetained = true;
    void quiesceNativeSystemEvents();
    void releaseNativeSystemEvents();
};
#include "../AirportItlwm/native_system_events_sequoia.inc"

struct EventService : OSObject {
    uint8_t storage[0x28] = {};
    IOTimerEventSource timer;
    size_t timerOffset;
    EventService(size_t offset) : timerOffset(offset) {
        ivars = storage;
        put(storage, offset, &timer);
    }
    void release() override {
        assert(gateDepth && !timer.enabled && timer.cancels);
        timer.release();
        OSObject::release();
    }
};

struct BlockedTimerAction : OSObject {
    std::promise<void> entered, permit;
    std::future<void> resume = permit.get_future();
    std::atomic<bool> interfaceAlive{true};
    static IOReturn run(OSObject *owner, void *, void *, void *, void *) {
        auto *action = static_cast<BlockedTimerAction *>(owner);
        assert(gateDepth);
        action->entered.set_value();
        action->resume.wait();
        assert(action->interfaceAlive.load());
        return kIOReturnSuccess;
    }
};

int main()
{
    IOService controller, foreign;
    uint8_t controllerObject[0x128] = {}, controllerIvars[0xb50] = {};
    assert(!controllerBaseStarted(nullptr, &controller));
    assert(!controllerBaseStarted(controllerObject, &controller));
    put(controllerObject, ControllerIvars, static_cast<void *>(controllerIvars));
    assert(!controllerBaseStarted(controllerObject, nullptr));
    assert(!controllerBaseStarted(controllerObject, &controller));
    put(controllerIvars, RetainedProvider, &controller);
    assert(controllerBaseStarted(controllerObject, &controller));
    assert(!controllerBaseStarted(controllerObject, &foreign));
    put(controllerIvars, RetainedProvider, static_cast<void *>(nullptr));
    assert(!controllerBaseStarted(controllerObject, &controller));
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (unsigned stage = 0; stage <= 12; ++stage) {
            AirportItlwmSkywalkInterface interface;
            assert(reinterpret_cast<uint8_t *>(&interface.scan.ivars) -
                   reinterpret_cast<uint8_t *>(&interface.scan) == ScanManagerIvars);
            interface.instance = &controller;
            interface.failStage = stage;
            put(interface.skywalkIvars, Role, uint32_t(1));
            interface.stop(&controller); // Stop before start cannot consume ownership.
            assert(!interface.nativeStopDone);
            assert(!interface.start(&foreign));
            assert(interface.starts == 0);
            assert(interface.start(&controller) == (stage == 12));
            interface.quiesceNativeTimers(); // Controller does this before HAL detach.
            assert(!interface.start(&controller));
            interface.stop(&foreign);
            interface.stop(&controller);
            interface.stop(&controller);
            assert(interface.starts == 1 && interface.nativeStopDone);
            assert(interface.ethernetStops == (stage >= 1));
            assert(interface.infraStops == (stage >= 4));
            assert(interface.scanStops == (stage >= 9));
            assert(interface.pipe.stops == (stage >= 2));
            assert(interface.pipe.releases == (stage >= 2));
            assert(interface.linkTimer.disables == (stage >= 10));
            assert(interface.dataTimer.disables == (stage >= 12));
            assert(interface.stream.releases == (stage >= 3));
            assert(interface.peer.releases == (stage >= 4));
            assert(interface.bonjour.releases == (stage >= 7));
            interface.releaseRemainingSystemFields();
            assert(interface.scan.releases == (stage >= 8));
        }
        for (unsigned invalid = 0; invalid < 5; ++invalid) {
            AirportItlwmSkywalkInterface interface;
            interface.instance = &controller;
            put(interface.skywalkIvars, Role, uint32_t(1));
            if (invalid == 0) interface.skywalkIvars = nullptr;
            if (invalid == 1) interface.infraIvars = nullptr;
            if (invalid == 2) interface.instance = nullptr;
            if (invalid == 3) put(interface.skywalkIvars, Role, uint32_t(2));
            if (invalid == 4) interface.nativeStopDone = true;
            assert(!interface.start(&controller));
            assert(interface.starts == 0);
        }
    }
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        AirportItlwm owner;
        uint8_t ivars[0xb50] = {};
        EventService office(PostOfficeTimer);
        owner.controllerIvars = ivars;
        put(ivars, ControllerPostOffice, &office);
        BlockedTimerAction action;
        std::promise<void> stopping;
        auto entered = action.entered.get_future(), closing = stopping.get_future();
        std::thread callback([&] { workQueue.runAction(BlockedTimerAction::run, &action); });
        entered.wait();
        std::thread closer([&] {
            stopping.set_value();
            owner.quiesceNativeSystemEvents();
            action.interfaceAlive.store(false);
            owner.releaseNativeSystemEvents();
        });
        closing.wait();
        action.permit.set_value();
        callback.join();
        closer.join();
        assert(!action.interfaceAlive && office.releases == 1 && office.timer.disables == 1);
    }
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (unsigned mask = 0; mask < 8; ++mask) {
            AirportItlwm owner;
            assert(reinterpret_cast<uint8_t *>(&owner.controllerIvars) -
                   reinterpret_cast<uint8_t *>(&owner) == ControllerIvars);
            uint8_t ivars[0xb50] = {};
            EventService office(PostOfficeTimer), rng(RNGAgentTimer);
            OSObject factory;
            owner.controllerIvars = ivars;
            if (mask & 1) put(ivars, ControllerPostOffice, &office);
            if (mask & 2) put(ivars, ControllerRNGAgent, &rng);
            if (mask & 4) put(ivars, ControllerTimerFactory, &factory);
            owner.nativeLoopRetained = false;
            owner.quiesceNativeSystemEvents();
            owner.releaseNativeSystemEvents();
            assert(office.timer.enabled && rng.timer.enabled);
            owner.nativeLoopRetained = true;
            owner.quiesceNativeSystemEvents();
            // Quiescence retains the office while system stop can still post.
            assert(read<void *>(ivars, ControllerPostOffice) == ((mask & 1) ? &office : nullptr));
            owner.releaseNativeSystemEvents();
            owner.releaseNativeSystemEvents();
            assert(office.releases == bool(mask & 1));
            assert(rng.releases == bool(mask & 2));
            assert(factory.releases == bool(mask & 4));
            assert(!read<void *>(ivars, ControllerPostOffice));
            assert(!read<void *>(ivars, ControllerRNGAgent));
            assert(!read<void *>(ivars, ControllerTimerFactory));
        }
    }
    puts("Native interface lifecycle: 1300 startup/rollback traces and 500 invalid admissions passed; Apple internals mocked");
    puts("Native system events: 800 quiesce-before-release and partial-allocation traces passed; Apple internals mocked");
    puts("Native system events: 100 active-timer versus teardown races passed; timer/workqueue boundaries mocked");
}
