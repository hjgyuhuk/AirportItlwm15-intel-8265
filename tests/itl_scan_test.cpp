#include <HAL/IwmScanNotification.h>
#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace ItlScan;

static void expectCompletion(const Lifecycle &scan, uint64_t token, Outcome outcome, bool accepted)
{
    Completion first = {}, retry = {};
    assert(scan.completion(first));
    assert(first.token == token && first.outcome == outcome && first.accepted == accepted);
    assert(scan.completion(retry));
    assert(retry.token == first.token && retry.outcome == first.outcome && retry.accepted == first.accepted);
}

static void expectNoCompletion(const Lifecycle &scan)
{
    Completion value = {42, Outcome::DeviceStopped, true};
    assert(!scan.completion(value));
    assert(value.token == 42 && value.outcome == Outcome::DeviceStopped && value.accepted);
}

static void lifecycleBasics()
{
    Lifecycle scan;
    assert(!scan.busy() && scan.drained() && scan.token() == 0);
    assert(!scan.begin(static_cast<Firmware>(99)));
    assert(!scan.begin(Firmware::LMAC, 1));
    assert(!scan.submitted(0, Submission::Accepted));
    assert(!scan.cancel(1) && !scan.timeout(1));
    assert(!scan.beginAbort(1) && !scan.aborted(1, true));
    assert(!scan.terminal(Firmware::LMAC, 0, Outcome::Success));
    assert(!scan.acknowledge(0));
    scan.hardwareStopped();
    expectNoCompletion(scan);

    for (Firmware firmware : {Firmware::LMAC, Firmware::UMAC}) {
        const uint32_t uid = firmware == Firmware::LMAC ? 0 : 3;
        const auto token = scan.begin(firmware, uid);
        assert(token && scan.busy() && !scan.drained());
        assert(!scan.begin(firmware, uid));
        assert(!scan.submitted(token + 1, Submission::Accepted));
        assert(!scan.submitted(token, static_cast<Submission>(99)));
        assert(!scan.terminal(firmware, uid, Outcome::TimedOut));
        assert(!scan.terminal(firmware, uid + 1, Outcome::Success));
        assert(!scan.terminal(firmware == Firmware::LMAC ? Firmware::UMAC : Firmware::LMAC,
                              uid, Outcome::Success));
        expectNoCompletion(scan);
        assert(!scan.beginAbort(token));
        assert(scan.submitted(token, Submission::Accepted));
        assert(!scan.submitted(token, Submission::Accepted));
        expectNoCompletion(scan);
        assert(scan.terminal(firmware, uid, Outcome::Success));
        assert(!scan.terminal(firmware, uid, Outcome::Aborted));
        assert(scan.drained() && scan.busy());
        expectCompletion(scan, token, Outcome::Success, true);
        assert(!scan.begin(firmware, uid)); // Delivery must succeed before reuse.
        assert(!scan.acknowledge(token + 1));
        assert(scan.acknowledge(token));
        assert(!scan.acknowledge(token));
        assert(!scan.busy());
    }
}

static void submissionRaces()
{
    for (Submission result : {Submission::Accepted, Submission::NotSent, Submission::Indeterminate}) {
        Lifecycle scan;
        auto token = scan.begin(Firmware::UMAC, 1);
        assert(scan.terminal(Firmware::UMAC, 1, Outcome::Success));
        expectNoCompletion(scan); // Submission still owns the command argument storage.
        assert(!scan.acknowledge(token) && !scan.begin(Firmware::LMAC));
        assert(scan.submitted(token, result));
        expectCompletion(scan, token, result == Submission::NotSent ? Outcome::TransportError : Outcome::Success,
                         result == Submission::Accepted);
        assert(scan.acknowledge(token));
        if (result == Submission::NotSent) {
            assert(scan.busy() && !scan.drained() && !scan.begin(Firmware::LMAC));
            assert(!scan.beginAbort(token));
            assert(!scan.terminal(Firmware::UMAC, 1, Outcome::Success));
            scan.hardwareStopped();
        }
        assert(!scan.busy());

        token = scan.begin(Firmware::LMAC);
        scan.hardwareStopped();
        assert(scan.busy() && scan.drained());
        expectNoCompletion(scan);
        assert(scan.submitted(token, result));
        expectCompletion(scan, token, Outcome::DeviceStopped, result == Submission::Accepted);
        assert(scan.acknowledge(token) && !scan.busy());
    }

    Lifecycle scan;
    auto token = scan.begin(Firmware::LMAC);
    assert(scan.submitted(token, Submission::NotSent));
    assert(scan.drained());
    expectCompletion(scan, token, Outcome::StartFailed, false);
    assert(!scan.beginAbort(token));
    assert(scan.acknowledge(token) && !scan.busy());

    token = scan.begin(Firmware::UMAC, 2);
    assert(scan.submitted(token, Submission::Indeterminate));
    expectCompletion(scan, token, Outcome::TransportError, false);
    assert(scan.acknowledge(token));
    assert(scan.busy() && !scan.drained());
    assert(scan.beginAbort(token));
    assert(scan.aborted(token, false));
    expectNoCompletion(scan); // No second notification for the same host request.
    assert(!scan.begin(Firmware::LMAC));
    assert(scan.terminal(Firmware::UMAC, 2, Outcome::Success));
    assert(!scan.busy());
}

static void cancellationAndTimeout()
{
    for (bool abortAccepted : {false, true}) {
        Lifecycle scan;
        auto token = scan.begin(Firmware::LMAC);
        assert(scan.cancel(token) && !scan.cancel(token));
        assert(!scan.beginAbort(token));
        assert(scan.submitted(token, Submission::Accepted));
        assert(scan.beginAbort(token) && !scan.beginAbort(token));
        assert(!scan.aborted(token + 1, abortAccepted));
        assert(scan.aborted(token, abortAccepted));
        assert(!scan.drained() && !scan.beginAbort(token));
        if (abortAccepted)
            expectNoCompletion(scan); // Abort ACK is not the terminal notification.
        else
            expectCompletion(scan, token, Outcome::TransportError, true);
        assert(scan.terminal(Firmware::LMAC, 0, Outcome::Success));
        expectCompletion(scan, token, abortAccepted ? Outcome::Aborted : Outcome::TransportError, true);
        assert(scan.acknowledge(token) && !scan.busy());

        token = scan.begin(Firmware::UMAC, 1);
        assert(scan.submitted(token, Submission::Accepted));
        assert(scan.cancel(token) && scan.beginAbort(token));
        assert(scan.terminal(Firmware::UMAC, 1, Outcome::Aborted));
        expectNoCompletion(scan); // Terminal RX raced with the abort return.
        assert(scan.aborted(token, abortAccepted));
        expectCompletion(scan, token, Outcome::Aborted, true);
        assert(scan.acknowledge(token) && !scan.busy());
    }

    Lifecycle scan;
    auto token = scan.begin(Firmware::LMAC);
    assert(scan.timeout(token));
    expectNoCompletion(scan);
    assert(scan.submitted(token, Submission::Accepted));
    expectCompletion(scan, token, Outcome::TimedOut, true);
    assert(scan.acknowledge(token));
    assert(scan.busy() && !scan.drained() && !scan.begin(Firmware::LMAC));
    assert(!scan.timeout(token) && !scan.cancel(token));
    assert(scan.beginAbort(token));
    scan.hardwareStopped();
    assert(scan.busy()); // Stop/reset must also wait for the abort caller.
    assert(scan.aborted(token, true));
    assert(!scan.busy());
    auto next = scan.begin(Firmware::LMAC);
    assert(next > token);
    assert(!scan.cancel(token) && !scan.timeout(token) && !scan.acknowledge(token));
    assert(!scan.submitted(token, Submission::Accepted));
    assert(!scan.aborted(token, true) && !scan.beginAbort(token));
    assert(scan.token() == next && !scan.drained());
    assert(scan.submitted(next, Submission::Accepted));
    assert(scan.terminal(Firmware::LMAC, 0, Outcome::Aborted));
    expectCompletion(scan, next, Outcome::Aborted, true);
    assert(scan.acknowledge(next));
}

static uint32_t nextRandom(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void lifecycleSequences()
{
    uint32_t random = 0x5316cafe;
    size_t starts = 0, deliveries = 0;
    for (size_t trace = 0; trace < 20000; ++trace) {
        Lifecycle scan;
        bool drained = true, delivered = false, commandRunning = false, terminalSeen = false;
        uint64_t lastToken = 0;
        for (size_t step = 0; step < 64; ++step) {
            const bool wasBusy = scan.busy();
            const auto token = scan.token();
            switch (nextRandom(random) % 15) {
                case 0: {
                    const auto newToken = scan.begin(Firmware::UMAC, 2);
                    if (wasBusy) {
                        assert(!newToken);
                    } else {
                        assert(newToken > lastToken);
                        lastToken = newToken;
                        drained = delivered = false;
                        terminalSeen = false;
                        commandRunning = true;
                        ++starts;
                    }
                    break;
                }
                case 1:
                case 2:
                case 3: {
                    const auto result = static_cast<Submission>(nextRandom(random) % 3);
                    if (scan.submitted(token, result)) {
                        commandRunning = false;
                        if (result == Submission::NotSent)
                            drained = !terminalSeen;
                    }
                    break;
                }
                case 4:
                    if (scan.terminal(Firmware::UMAC, 2, Outcome::Success))
                        terminalSeen = drained = true;
                    break;
                case 5:
                    assert(!scan.terminal(Firmware::LMAC, 0, Outcome::Success));
                    assert(!scan.terminal(Firmware::UMAC, 3, Outcome::Success));
                    break;
                case 6: scan.cancel(token); break;
                case 7: scan.timeout(token); break;
                case 8:
                    if (scan.beginAbort(token)) {
                        assert(!commandRunning && !drained);
                        commandRunning = true;
                    }
                    break;
                case 9:
                    if (scan.aborted(token, nextRandom(random) & 1))
                        commandRunning = false;
                    break;
                case 10:
                    scan.hardwareStopped();
                    drained = true;
                    break;
                case 11:
                    if (scan.acknowledge(token)) {
                        assert(!delivered && !commandRunning);
                        delivered = true;
                        ++deliveries;
                    }
                    break;
                case 12: {
                    Completion value;
                    if (scan.completion(value)) {
                        assert(!delivered && !commandRunning && value.token == token);
                        expectCompletion(scan, token, value.outcome, value.accepted);
                    }
                    break;
                }
                case 13:
                    assert(!scan.timeout(token + 1) && !scan.acknowledge(token + 1));
                    assert(!scan.submitted(token + 1, Submission::NotSent));
                    break;
                case 14:
                    if (scan.terminal(Firmware::UMAC, 2, Outcome::TransportError))
                        terminalSeen = drained = true;
                    break;
            }
            if (scan.busy()) {
                assert(scan.drained() == drained);
                assert(!(drained && delivered && !commandRunning));
            } else if (wasBusy) {
                assert(drained && delivered && !commandRunning);
            }
        }
    }
    assert(starts > 20000 && deliveries > 10000);
    printf("scan lifecycle: 20000 randomized traces, %zu starts, %zu deliveries\n", starts, deliveries);
}

static bool sameNotification(const ScanNotification &left, const ScanNotification &right)
{
    return left.firmware == right.firmware && left.terminal == right.terminal &&
           left.uid == right.uid && left.status == right.status && left.channels == right.channels;
}

static void notifications()
{
    struct Layout { IwmNotification kind; size_t header, status, count; bool terminal, umac; };
    const Layout layouts[] = {
        {IwmNotification::LmacComplete, 12, 2, 0, true, false},
        {IwmNotification::LmacIteration, 12, 1, 0, false, false},
        {IwmNotification::UmacComplete, 16, 6, 4, true, true},
        {IwmNotification::UmacIteration, 16, 5, 4, false, true},
    };
    const ScanNotification sentinel = {Firmware::UMAC, true, 0xabcdef01, 99, 98};
    for (const auto &layout : layouts) {
        for (size_t count : {size_t(0), size_t(1), size_t(255)}) {
            const size_t length = layout.header + (layout.terminal ? 0 : count * 8);
            std::vector<uint8_t> bytes(length + 2, 0xa5);
            auto *payload = bytes.data() + 1; // Deliberately unaligned.
            if (layout.umac) {
                payload[0] = 0x78; payload[1] = 0x56; payload[2] = 0x34; payload[3] = 0x12;
            }
            if (!layout.terminal)
                payload[layout.count] = count;
            payload[layout.status] = 1;
            for (size_t truncated = 0; truncated < length; ++truncated) {
                // An exact allocation lets ASan catch reads beyond the supplied bound.
                std::vector<uint8_t> shortBytes(payload, payload + truncated);
                auto result = sentinel;
                assert(!decodeIwmNotification(layout.kind, shortBytes.data(), truncated, result));
                assert(sameNotification(result, sentinel));
            }
            for (unsigned status = 0; status < 256; ++status) {
                payload[layout.status] = status;
                const auto saved = bytes;
                auto result = sentinel;
                assert(decodeIwmNotification(layout.kind, payload, length, result));
                assert(result.firmware == (layout.umac ? Firmware::UMAC : Firmware::LMAC));
                assert(result.terminal == layout.terminal && result.status == status);
                assert(result.uid == (layout.umac ? 0x12345678 : 0));
                assert(result.channels == (layout.terminal ? 0 : count));
                Outcome outcome = Outcome::DeviceStopped;
                assert(terminalOutcome(result, outcome) == layout.terminal);
                assert(outcome == (!layout.terminal ? Outcome::DeviceStopped : status == 1 ? Outcome::Success :
                                   status == 2 ? Outcome::Aborted : Outcome::TransportError));
                assert(bytes == saved);
            }
            auto result = sentinel;
            assert(!decodeIwmNotification(layout.kind, nullptr, length, result));
            // Group 1 is used by scan commands, but NOT by these notifications.
            assert(!decodeIwmNotification(static_cast<IwmNotification>(uint16_t(layout.kind) | 0x100),
                                           payload, length, result));
            assert(sameNotification(result, sentinel));
        }
    }

    uint8_t payload[16] = {};
    auto result = sentinel;
    assert(!decodeIwmNotification(static_cast<IwmNotification>(0xffff), payload, sizeof(payload), result));
    assert(sameNotification(result, sentinel));
    Lifecycle scan;
    const auto token = scan.begin(Firmware::UMAC, 2);
    assert(scan.submitted(token, Submission::Accepted));
    payload[0] = 2;
    payload[5] = 1;
    assert(decodeIwmNotification(IwmNotification::UmacIteration, payload, sizeof(payload), result));
    Outcome outcome;
    if (terminalOutcome(result, outcome))
        scan.terminal(result.firmware, result.uid, outcome);
    assert(!scan.drained());
    expectNoCompletion(scan);
    payload[6] = 1;
    assert(decodeIwmNotification(IwmNotification::UmacComplete, payload, sizeof(payload), result));
    assert(terminalOutcome(result, outcome));
    assert(scan.terminal(result.firmware, result.uid, outcome));
    expectCompletion(scan, token, Outcome::Success, true);
}

static void probeIdentities()
{
    const uint8_t interfaceAddress[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    const uint8_t multicast[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t zero[6] = {};
    ProbeIdentity identity = {};
    for (unsigned first = 0; first < 256; ++first) {
        uint8_t entropy[6] = {static_cast<uint8_t>(first), 9, 8, 7, 6, 5};
        assert(privateProbeIdentity(entropy, interfaceAddress, broadcast, identity));
        assert(identity.source[0] == ((first & 0xfc) | 2));
        assert(memcmp(identity.source + 1, entropy + 1, 5) == 0);
        assert(validProbeIdentity(identity) && memcmp(identity.bssid, broadcast, 6) == 0);
    }
    const auto saved = identity;
    assert(!privateProbeIdentity(interfaceAddress, interfaceAddress, broadcast, identity));
    assert(!privateProbeIdentity(nullptr, interfaceAddress, broadcast, identity));
    assert(!privateProbeIdentity(broadcast, nullptr, broadcast, identity));
    assert(!privateProbeIdentity(broadcast, interfaceAddress, nullptr, identity));
    assert(!privateProbeIdentity(broadcast, zero, broadcast, identity));
    assert(!privateProbeIdentity(broadcast, multicast, broadcast, identity));
    assert(!privateProbeIdentity(broadcast, interfaceAddress, multicast, identity));
    assert(!privateProbeIdentity(broadcast, interfaceAddress, zero, identity));
    assert(memcmp(&saved, &identity, sizeof(saved)) == 0);
    assert(!validUnicast(nullptr) && !validUnicast(zero) && !validUnicast(broadcast));
    assert(privateProbeIdentity(broadcast, interfaceAddress, interfaceAddress, identity));
    assert(memcmp(identity.bssid, interfaceAddress, 6) == 0);

    uint8_t frame[40];
    memset(frame, 0xa5, sizeof(frame));
    frame[0] = 0x40; frame[1] = 0;
    uint8_t original[sizeof(frame)];
    memcpy(original, frame, sizeof(frame));
    for (size_t length = 0; length < 24; ++length) {
        std::vector<uint8_t> shortFrame(frame, frame + length);
        const auto unchanged = shortFrame;
        assert(!applyProbeIdentity(shortFrame.data(), length, identity));
        assert(shortFrame == unchanged);
    }
    assert(!applyProbeIdentity(nullptr, sizeof(frame), identity));
    auto badIdentity = identity;
    badIdentity.source[0] |= 1;
    assert(!applyProbeIdentity(frame, sizeof(frame), badIdentity));
    assert(memcmp(frame, original, sizeof(frame)) == 0);
    for (size_t offset : {size_t(0), size_t(1)}) {
        frame[offset] = 0xff;
        uint8_t unchanged[sizeof(frame)];
        memcpy(unchanged, frame, sizeof(frame));
        assert(!applyProbeIdentity(frame, sizeof(frame), identity));
        assert(memcmp(frame, unchanged, sizeof(frame)) == 0);
        memcpy(frame, original, sizeof(frame));
    }
    assert(applyProbeIdentity(frame, sizeof(frame), identity));
    assert(memcmp(frame + 4, broadcast, 6) == 0);
    assert(memcmp(frame + 10, identity.source, 6) == 0);
    assert(memcmp(frame + 16, identity.bssid, 6) == 0);
    assert(memcmp(frame, original, 4) == 0 && memcmp(frame + 22, original + 22, sizeof(frame) - 22) == 0);

    struct AliasedFrame { uint8_t prefix[4]; ProbeIdentity identity; uint8_t rest[24]; } aliased = {};
    static_assert(offsetof(AliasedFrame, identity) == 4, "alias test layout");
    aliased.prefix[0] = 0x40;
    aliased.identity = identity;
    assert(applyProbeIdentity(&aliased, sizeof(aliased), aliased.identity));
    const auto *bytes = reinterpret_cast<const uint8_t *>(&aliased);
    assert(memcmp(bytes + 10, identity.source, 6) == 0);
    assert(memcmp(bytes + 16, identity.bssid, 6) == 0);
}

int main()
{
    lifecycleBasics();
    submissionRaces();
    cancellationAndTimeout();
    lifecycleSequences();
    notifications();
    probeIdentities();
    puts("scan lifecycle, IWM notifications and probe identity tests passed");
}
