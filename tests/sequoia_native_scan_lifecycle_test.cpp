// The scan lifecycle on the layer above the HAL: it understands one shape of
// scan notification, and a completion has to exist however the scan ended.
#include <Airport/SequoiaNativeScan.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SequoiaWCL;
using namespace ItlScan;

static bool preparePlan(NativeScan &scan)
{
    const Channel inventory[] = {{1, 1, Width20MHz | Band2GHz | ActiveScan},
                                 {1, 36, Width20MHz | Band5GHz | ActiveScan}};
    const uint8_t mac[6] = {0x88, 1, 2, 3, 4, 5}, entropy[6] = {0x55, 6, 7, 8, 9, 10};
    ScanRequest request = {};
    request.version = 1;
    request.privateMAC = 1;
    request.scanType = 2;
    request.passiveDwell = UINT32_MAX;
    request.channelCount = 2;
    request.channels[0] = inventory[0];
    request.channels[1] = inventory[1];
    std::vector<uint8_t> unaligned(sizeof(request) + 1);
    memcpy(unaligned.data() + 1, &request, sizeof(request));
    return scan.plan.prepare(unaligned.data() + 1, sizeof(request), inventory, 2,
                             mac, entropy, 6, false) == ScanPlanStatus::Success;
}

// The layer reads the iteration as the count of scanned channels, its status
// and the per-channel results behind a twelve byte header.
static void iteration(uint8_t *bytes, unsigned count)
{
    memset(bytes, 0, 12 + 8 * count);
    bytes[0] = static_cast<uint8_t>(count);
    bytes[1] = 1;
    bytes[12] = 1; bytes[13] = 1;          // channel 1, 2.4 GHz
    bytes[20] = 36; bytes[21] = 0;         // channel 36, 5 GHz
}

static void terminalProducesCompletion()
{
    NativeScan scan;
    ScanDelivery::Slot slots[4];
    assert(scan.delivery.setStorage(slots, 4));
    assert(preparePlan(scan));
    assert(scan.begin());
    scan.submitted(Submission::Accepted);
    uint8_t iter[12 + 8 * 2] = {};
    iteration(iter, 2);
    scan.notification(0xe7, iter, sizeof(iter));
    uint8_t complete[12] = {};
    complete[2] = 1;                        // the status
    scan.notification(0x6d, complete, sizeof(complete));
    Completion completion;
    assert(scan.delivery.completion(completion));
    assert(completion.outcome == Outcome::Success);
    assert(scan.diagnostics.terminals == 1);
    assert(scan.diagnostics.lastOutcome == static_cast<uint32_t>(Outcome::Success));
    assert(scan.diagnostics.lastTerminalCode == 0x6d);
    // The coverage tracker saw the whole channel list.
    assert(scan.diagnostics.coverageRequested == 2);
    assert(scan.diagnostics.coverageIterations == 1);
    assert(scan.diagnostics.coverageScanned == 2);
    assert(scan.diagnostics.coverageTerminalStatus == 1);
    assert(scan.diagnostics.coverageIterationStatus == 1);
    assert(scan.diagnostics.coverageMalformed == 0);
}

// A scan that never reports its end must still leave a completion behind.
// Without one the delivery stays busy and every later request is refused,
// which leaves the caller waiting and the driver spinning.
static void missingTerminalStillCompletes()
{
    NativeScan scan;
    ScanDelivery::Slot slots[4];
    assert(scan.delivery.setStorage(slots, 4));
    assert(preparePlan(scan));
    assert(scan.begin());
    scan.submitted(Submission::Accepted);
    scan.workerFinished();
    Completion completion;
    assert(scan.delivery.completion(completion));
    assert(completion.outcome == Outcome::TransportError);
    // The delivery can drain it, so the next request is admitted.
    assert(scan.delivery.deliverOne([](uint32_t, const void *, size_t) { return true; },
        NativeScan::failureStatus) == ScanDelivery::Delivery::Complete);
    assert(!scan.delivery.busy());
    assert(scan.begin());
}

// The same, with a request that was never sent.
static void unsentRequestStillCompletes()
{
    NativeScan scan;
    ScanDelivery::Slot slots[4];
    assert(scan.delivery.setStorage(slots, 4));
    assert(preparePlan(scan));
    assert(scan.begin());
    scan.workerFinished();
    Completion completion;
    assert(scan.delivery.completion(completion));
    assert(completion.outcome == Outcome::StartFailed);
    assert(scan.delivery.deliverOne([](uint32_t, const void *, size_t) { return true; },
        NativeScan::failureStatus) == ScanDelivery::Delivery::Complete);
    assert(!scan.delivery.busy());
}

// A terminal whose coverage is short is a failure, but it is still a
// completion: the beacons that were collected are still delivered.
static void shortCoverageIsAFailure()
{
    NativeScan scan;
    ScanDelivery::Slot slots[4];
    assert(scan.delivery.setStorage(slots, 4));
    assert(preparePlan(scan));
    assert(scan.begin());
    scan.submitted(Submission::Accepted);
    uint8_t iter[12 + 8 * 1] = {};
    iteration(iter, 1);
    scan.notification(0xe7, iter, sizeof(iter));
    uint8_t complete[12] = {};
    complete[2] = 1;
    scan.notification(0x6d, complete, sizeof(complete));
    Completion completion;
    assert(scan.delivery.completion(completion));
    assert(completion.outcome == Outcome::TransportError);
}

int main()
{
    terminalProducesCompletion();
    missingTerminalStillCompletes();
    unsentRequestStillCompletes();
    shortCoverageIsAFailure();
    puts("Native scan lifecycle: terminal, missing terminal, unsent request and short coverage passed");
    return 0;
}
