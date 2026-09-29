#include <cassert>
#include <cstdio>
#include <cstring>
#include <Airport/SequoiaInterfaceLifecycle.h>

#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define XYLog(...) do {} while (0)
#define OSSafeReleaseNULL(object) do { if (object) { (object)->release(); (object) = nullptr; } } while (0)
using namespace SequoiaInterfaceLifecycle;
struct OSDictionary {};
struct CCPipe {
    unsigned stops = 0, releases = 0;
    void stopPipe() { assert(!releases); ++stops; }
    void release() { assert(stops == 1 && !releases); ++releases; }
};
struct CCStream {
    CCPipe *pipe = nullptr;
    unsigned releases = 0;
    void release() { assert(pipe && pipe->stops == 1 && !pipe->releases && !releases); ++releases; }
};
struct IORecursiveLock {};
static bool reserveFails;
static unsigned liveLocks;
static IORecursiveLock *IORecursiveLockAlloc()
{
    if (reserveFails) return nullptr;
    ++liveLocks;
    return new IORecursiveLock;
}
static void IORecursiveLockFree(IORecursiveLock *lock)
{
    assert(lock && liveLocks);
    --liveLocks;
    delete lock;
}
static void storePointer(void *storage, size_t offset, void *value)
{
    memcpy(static_cast<uint8_t *>(storage) + offset, &value, sizeof(value));
}
struct IO80211Controller {
    uint8_t object[0x128] = {}, ivars[0xb50] = {};
    unsigned stage = 0, calls = 0;
    bool init(OSDictionary *) {
        ++calls;
        if (!stage) return false;
        storePointer(this, ControllerIvars, ivars);
        if (stage == 1) return false; // The system's recursive-lock allocation failed.
        storePointer(ivars, ControllerRecursiveLock, IORecursiveLockAlloc());
        if (stage == 2) return false; // The cage allocation failed before list initialization.
        storePointer(ivars, ControllerReportList, ivars + ControllerReportList);
        storePointer(ivars, ControllerReportList + sizeof(void *), ivars + ControllerReportList);
        return stage == 5; // Later lock/base-init failures already have an empty list.
    }
    void free() {
        auto *data = read<uint8_t *>(this, ControllerIvars);
        if (!data) return;
        assert(read<void *>(data, ControllerReportList) == data + ControllerReportList);
        assert(read<void *>(data, ControllerReportList + sizeof(void *)) == data + ControllerReportList);
        IORecursiveLockFree(take<IORecursiveLock>(data, ControllerRecursiveLock));
        take<void>(this, ControllerIvars);
    }
};
struct AirportItlwm : IO80211Controller {
    bool awdlSyncEnable = false;
    unsigned power_state = 42;
    char geo_location_cc[3] = {1, 2, 3};
    CCPipe *driverLogPipe = nullptr, *driverDataPathPipe = nullptr, *driverSnapshotsPipe = nullptr;
    CCStream *driverLogger = nullptr, *driverFaultReporter = nullptr;
    bool init(OSDictionary *);
    void releaseCCLogs();
};
#define super IO80211Controller
#include "sequoia_controller_init.inc"

int main()
{
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (unsigned stage = 0; stage < 6; ++stage) {
            AirportItlwm controller;
            controller.stage = stage;
            assert(controller.init(nullptr) == (stage == 5));
            assert(controller.calls == 1);
            assert(liveLocks == (stage ? 1u : 0u));
            assert(controller.awdlSyncEnable && !controller.power_state);
            assert(!controller.geo_location_cc[0] && !controller.geo_location_cc[1] && !controller.geo_location_cc[2]);
            controller.free();
            assert(!liveLocks);
        }
        AirportItlwm controller;
        reserveFails = true;
        assert(!controller.init(nullptr));
        reserveFails = false;
        assert(controller.calls == 0);
        controller.free();
        assert(!liveLocks);
    }
    // Existing nonempty lists and already-owned locks must never be replaced.
    IO80211Controller controller;
    storePointer(&controller, ControllerIvars, controller.ivars);
    IORecursiveLock owned, reserved;
    storePointer(controller.ivars, ControllerRecursiveLock, &owned);
    void *next = &owned, *previous = &reserved;
    storePointer(controller.ivars, ControllerReportList, next);
    storePointer(controller.ivars, ControllerReportList + sizeof(void *), previous);
    assert(!prepareFailedControllerInit(&controller, &reserved));
    assert(read<void *>(controller.ivars, ControllerRecursiveLock) == &owned);
    assert(read<void *>(controller.ivars, ControllerReportList) == next);
    assert(read<void *>(controller.ivars, ControllerReportList + sizeof(void *)) == previous);
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (unsigned stage = 0; stage < 6; ++stage) {
            AirportItlwm owner;
            CCPipe log, events, snapshots;
            CCStream logger, fault;
            logger.pipe = &log;
            fault.pipe = &snapshots;
            if (stage >= 1) owner.driverLogPipe = &log;
            if (stage >= 2) owner.driverLogger = &logger;
            if (stage >= 3) owner.driverDataPathPipe = &events;
            if (stage >= 4) owner.driverSnapshotsPipe = &snapshots;
            if (stage >= 5) owner.driverFaultReporter = &fault;
            owner.releaseCCLogs();
            owner.releaseCCLogs();
            assert(log.stops == (stage >= 1) && log.releases == (stage >= 1));
            assert(logger.releases == (stage >= 2));
            assert(events.stops == (stage >= 3) && events.releases == (stage >= 3));
            assert(snapshots.stops == (stage >= 4) && snapshots.releases == (stage >= 4));
            assert(fault.releases == (stage >= 5));
        }
    }
    puts("Native controller init: 700 actual-init allocation traces and existing-ownership preservation passed; system init/free mocked");
    puts("Native controller logs: 600 partial-allocation stop/release traces passed; CoreCapture internals mocked");
}
