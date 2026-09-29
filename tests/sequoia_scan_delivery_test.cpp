#include <Airport/SequoiaScanDelivery.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using Queue = SequoiaWCL::ScanDelivery;
using namespace ItlScan;

struct Event { uint32_t kind; std::vector<uint8_t> bytes; };

static std::vector<uint8_t> beacon(unsigned id)
{
    std::vector<uint8_t> bytes(39, 0);
    bytes[0] = 0x80; bytes[16] = 2; bytes[21] = id;
    bytes[32] = 100; bytes[37] = 1; bytes[38] = id;
    return bytes;
}

struct Fixture {
    Queue::Slot slots[4];
    Queue queue{slots, 4};
    LegacyPhy phy;
    std::vector<Event> events;
    unsigned failures = 0;
    Fixture() { phy.channel = 40; phy.rssi = -42; }
    bool post(uint32_t kind, const void *data, size_t size)
    {
        if (failures) { --failures; return false; }
        const auto *bytes = static_cast<const uint8_t *>(data);
        events.push_back({kind, {bytes, bytes + size}});
        return true;
    }
    Queue::Delivery send()
    {
        return queue.deliverOne([&](uint32_t kind, const void *data, size_t size) { return post(kind, data, size); },
            [](const Completion &, bool, uint32_t &) { return false; });
    }
    Queue::Delivery sendMapped()
    {
        return queue.deliverOne([&](uint32_t kind, const void *data, size_t size) { return post(kind, data, size); },
            [](const Completion &, bool, uint32_t &status) {
                status = 0xe3ff8100; return true; // Test-supplied mapping, not an Intel status implementation.
            });
    }
    void result(unsigned index, unsigned id)
    {
        assert(events[index].kind == SequoiaWCL::kScanResultMessage);
        assert(events[index].bytes.size() >= sizeof(SequoiaWCL::BeaconMetadata));
        assert(events[index].bytes.back() == id);
        SequoiaWCL::BeaconMetadata metadata;
        memcpy(&metadata, events[index].bytes.data(), sizeof(metadata));
        assert(metadata.ieLength == 3 && metadata.ssidLength == 1);
        assert(events[index].bytes.size() == sizeof(metadata) + metadata.ieLength);
        assert(events[index].bytes[sizeof(metadata)] == 0 &&
               events[index].bytes[sizeof(metadata) + 1] == 1);
    }
    void complete(unsigned index, uint32_t expected)
    {
        assert(events[index].kind == SequoiaWCL::kScanCompleteMessage && events[index].bytes.size() == 4);
        uint32_t actual; memcpy(&actual, events[index].bytes.data(), 4); assert(actual == expected);
    }
};

static void ownershipAndRetries()
{
    for (bool early : {false, true}) {
        Fixture f;
        const auto token = f.queue.begin(Firmware::LMAC);
        assert(token);
        for (unsigned id = 1; id <= 4; ++id) {
            auto frame = beacon(id);
            assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Queued);
            memset(frame.data(), 0, frame.size());
        }
        assert(f.send() == Queue::Delivery::Idle); // Firmware ACK still pending.
        if (early) assert(f.queue.terminal(Firmware::LMAC, 0, Outcome::Success));
        assert(f.queue.submitted(token, Submission::Accepted));
        if (!early) assert(f.queue.terminal(Firmware::LMAC, 0, Outcome::Success));
        assert(!f.queue.begin(Firmware::LMAC));
        for (unsigned id = 1; id <= 4; ++id) {
            f.failures = 3;
            for (unsigned i = 0; i < 3; ++i) {
                assert(f.send() == Queue::Delivery::Retry && f.queue.pending() == 5 - id);
                assert(f.events.size() == id - 1);
            }
            assert(f.send() == Queue::Delivery::Result); f.result(id - 1, id);
        }
        f.failures = 2;
        assert(f.send() == Queue::Delivery::Retry && f.queue.busy());
        assert(f.send() == Queue::Delivery::Retry && f.queue.busy());
        assert(f.send() == Queue::Delivery::Complete && !f.queue.busy()); f.complete(4, 0);
        assert(f.queue.lastCompletionStatus() == 0);
        assert(f.send() == Queue::Delivery::Idle && f.events.size() == 5);
        for (const auto &slot : f.slots) {
            assert(!slot.size); for (auto byte : slot.bytes) assert(!byte);
        }
        const auto next = f.queue.begin(Firmware::LMAC);
        assert(next > token);
        const auto frame = beacon(9);
        assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Closed);
        assert(!f.queue.timeout(token));
        assert(f.queue.submitted(next, Submission::NotSent));
        assert(f.send() == Queue::Delivery::NeedsStatusMapping);
        assert(f.sendMapped() == Queue::Delivery::Complete && !f.queue.busy());
        assert(f.queue.lastCompletionStatus() == 0xe3ff8100);
    }
}

static void failuresAndDrain()
{
    for (unsigned fault = 0; fault < 8; ++fault) {
        Fixture f;
        const auto token = f.queue.begin(Firmware::UMAC, 7);
        const auto frame = beacon(1);
        assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Queued);
        if (fault == 0) {
            assert(f.queue.submitted(token, Submission::NotSent));
        } else if (fault == 1 || fault == 2) {
            if (fault == 2) assert(f.queue.terminal(Firmware::UMAC, 7, Outcome::Success));
            assert(f.queue.submitted(token, Submission::Indeterminate));
        } else {
            assert(f.queue.submitted(token, Submission::Accepted));
            if (fault == 3) assert(f.queue.timeout(token));
            if (fault == 4) {
                assert(f.queue.cancel(token)); assert(f.queue.beginAbort(token));
                assert(f.queue.aborted(token, true));
                assert(!f.queue.drained());
                assert(f.send() == Queue::Delivery::Result);
                assert(f.send() == Queue::Delivery::Idle);
                assert(f.queue.terminal(Firmware::UMAC, 7, Outcome::Aborted));
            }
            if (fault == 5) assert(f.queue.hardwareStopped());
            if (fault == 6) assert(f.queue.terminal(Firmware::UMAC, 7, Outcome::TransportError));
            if (fault == 7) {
                for (unsigned i = 0; i < 3; ++i)
                    assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Queued);
                assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Full);
                assert(f.queue.overflowed() && f.queue.terminal(Firmware::UMAC, 7, Outcome::Success));
            }
        }
        assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Closed);
        while (f.queue.pending()) assert(f.send() == Queue::Delivery::Result);
        assert(f.send() == Queue::Delivery::NeedsStatusMapping);
        assert(f.queue.deliverOne([](uint32_t, const void *, size_t) { assert(false); return true; },
            [](const Completion &, bool, uint32_t &status) { status = 0; return true; }) == Queue::Delivery::NeedsStatusMapping);
        assert(f.sendMapped() == Queue::Delivery::Complete);
        f.complete(f.events.size() - 1, 0xe3ff8100);
        if (!f.queue.drained()) assert(!f.queue.begin(Firmware::LMAC));
        assert(f.queue.hardwareStopped());
        assert(!f.queue.busy() && f.send() == Queue::Delivery::Idle);
    }
}

static void boundsAndReentrancy()
{
    Fixture f;
    Queue missing(nullptr, 4), empty(f.slots, 0), oversized(f.slots, 129);
    assert(!missing.begin(Firmware::LMAC) && !empty.begin(Firmware::LMAC) && !oversized.begin(Firmware::LMAC));
    const auto token = f.queue.begin(Firmware::UMAC, 3);
    assert(f.queue.queueBeacon(token, f.phy, nullptr, 0) == Queue::Enqueue::Invalid);
    assert(!f.queue.pending() && !f.queue.overflowed());
    assert(f.queue.submitted(token, Submission::Accepted));
    assert(!f.queue.terminal(Firmware::LMAC, 0, Outcome::Success));
    assert(!f.queue.terminal(Firmware::UMAC, 4, Outcome::Success));
    assert(f.queue.terminal(Firmware::UMAC, 3, Outcome::Success));
    auto mapped = [](const Completion &, bool, uint32_t &) { return false; };
    assert(f.queue.deliverOne([&](uint32_t kind, const void *data, size_t size) {
        assert(!f.queue.begin(Firmware::LMAC) && !f.queue.hardwareStopped());
        assert(!f.queue.timeout(token) && !f.queue.cancel(token));
        assert(f.queue.deliverOne([](uint32_t, const void *, size_t) { return true; }, mapped) == Queue::Delivery::Idle);
        return f.post(kind, data, size);
    }, mapped) == Queue::Delivery::Complete);
    f.complete(0, 0);
}

static void serializedThreads()
{
    Fixture f;
    std::mutex mutex;
    const auto token = f.queue.begin(Firmware::LMAC);
    assert(f.queue.submitted(token, Submission::Accepted));
    unsigned produced = 0;
    bool finished = false;
    std::thread producer([&] {
        while (produced < 10000) {
            std::lock_guard<std::mutex> guard(mutex);
            if (f.queue.pending() == 4) continue;
            auto frame = beacon(produced % 251 + 1);
            assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Queued);
            ++produced;
        }
        std::lock_guard<std::mutex> guard(mutex);
        assert(f.queue.terminal(Firmware::LMAC, 0, Outcome::Success));
        finished = true;
    });
    std::thread consumer([&] {
        for (;;) {
            std::lock_guard<std::mutex> guard(mutex);
            if (finished && !f.queue.busy()) break;
            f.send();
        }
    });
    producer.join(); consumer.join();
    assert(f.events.size() == 10001 && !f.queue.overflowed());
    for (unsigned i = 0; i < 10000; ++i) f.result(i, i % 251 + 1);
    f.complete(10000, 0);
}

static void detachedRecipient()
{
    for (bool submitted : {false, true}) {
        Fixture f;
        const auto token = f.queue.begin(Firmware::LMAC);
        const auto frame = beacon(7);
        assert(f.queue.queueBeacon(token, f.phy, frame.data(), frame.size()) == Queue::Enqueue::Queued);
        if (submitted) assert(f.queue.submitted(token, Submission::Accepted));
        assert(!f.queue.discardForDetach());
        assert(f.queue.hardwareStopped());
        if (!submitted) {
            assert(!f.queue.discardForDetach());
            assert(f.queue.submitted(token, Submission::Indeterminate));
        }
        assert(f.queue.discardForDetach() && !f.queue.busy() && !f.queue.pending());
        assert(f.events.empty() && f.send() == Queue::Delivery::Idle);
        for (const auto &slot : f.slots) for (auto byte : slot.bytes) assert(!byte);
        assert(f.queue.begin(Firmware::LMAC) > token);
    }
}

static void workqueueAdapter()
{
    struct Workqueue { bool entered = false; bool inGate() { return entered; } } workqueue;
    struct Interface {} interface;
    struct Controller {
        Fixture *fixture; Workqueue *workqueue;
        Workqueue *getWorkQueue() { return workqueue; }
        int postMessage(Interface *, uint32_t kind, void *bytes, size_t size, bool dispatch)
        {
            // processScanResults and scanComplete both pass true on 24G830.
            assert(dispatch && workqueue->inGate());
            return fixture->post(kind, bytes, size) ? 0 : int(0xe00002bd);
        }
    };
    Fixture f;
    Controller controller{&f, nullptr};
    const auto token = f.queue.begin(Firmware::LMAC);
    assert(f.queue.submitted(token, Submission::Accepted));
    assert(f.queue.terminal(Firmware::LMAC, 0, Outcome::Success));
    auto unmapped = [](const Completion &, bool, uint32_t &) { return false; };
    using SequoiaWCL::postScanDeliveryOnWorkQueue;
    assert(postScanDeliveryOnWorkQueue(static_cast<Controller *>(nullptr), &interface, f.queue, unmapped) == Queue::Delivery::NotReady);
    assert(postScanDeliveryOnWorkQueue(&controller, static_cast<Interface *>(nullptr), f.queue, unmapped) == Queue::Delivery::NotReady);
    assert(postScanDeliveryOnWorkQueue(&controller, &interface, f.queue, unmapped) == Queue::Delivery::NotReady);
    controller.workqueue = &workqueue;
    assert(postScanDeliveryOnWorkQueue(&controller, &interface, f.queue, unmapped) == Queue::Delivery::NotReady);
    assert(f.events.empty() && f.queue.busy());
    workqueue.entered = true; f.failures = 1;
    assert(postScanDeliveryOnWorkQueue(&controller, &interface, f.queue, unmapped) == Queue::Delivery::Retry);
    assert(postScanDeliveryOnWorkQueue(&controller, &interface, f.queue, unmapped) == Queue::Delivery::Complete);
    f.complete(0, 0);
}

int main()
{
    ownershipAndRetries(); failuresAndDrain(); boundsAndReentrancy(); detachedRecipient(); workqueueAdapter(); serializedThreads();
    puts("WCL result delivery: ownership, ordering, retries, overflow, abort/reset and 10,000 serialized cross-thread results passed");
}
