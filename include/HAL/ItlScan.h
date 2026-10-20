#ifndef ITL_SCAN_H
#define ITL_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ItlScan {

enum class Firmware { LMAC, UMAC };
enum class Outcome { Success, Aborted, TimedOut, StartFailed, TransportError, DeviceStopped };
enum class Submission { Accepted, NotSent, Indeterminate };

struct Completion {
    uint64_t token;
    Outcome outcome;
    bool accepted; // Submission was acknowledged, not a WCL acceptance status.
};

// Serial state model for one firmware scan, not a lock or a radio implementation.
// Callers must serialize ALL entry points, including workers, RX and teardown.
// Tokens identify host commands/timers. LMAC notifications have no token; UMAC
// UIDs are firmware slots, not unbounded generations. Transport must guarantee
// ordered, single terminal notifications and drain/reset before reusing a slot.
class Lifecycle {
public:
    uint64_t begin(Firmware firmware, uint32_t uid = 0)
    {
        if (busy() || sequence_ == UINT64_MAX ||
            (firmware != Firmware::LMAC && firmware != Firmware::UMAC) ||
            (firmware == Firmware::LMAC && uid != 0))
            return 0;
        current_ = {};
        current_.token = ++sequence_;
        current_.firmware = firmware;
        current_.uid = uid;
        current_.command = Command::Submit;
        return current_.token;
    }

    bool submitted(uint64_t token, Submission result)
    {
        if (!matches(token) || current_.command != Command::Submit ||
            (result != Submission::Accepted && result != Submission::NotSent &&
             result != Submission::Indeterminate))
            return false;
        current_.command = Command::None;
        current_.accepted = result == Submission::Accepted;
        if (result == Submission::NotSent && current_.terminalSeen) {
            // Contradictory evidence invalidates our terminal ownership.
            // Quarantine this slot until an explicit hardware drain/reset.
            current_.resetRequired = true;
            current_.drained = false;
            finish(Outcome::TransportError);
        } else if (!current_.outcomeKnown && result != Submission::Accepted)
            finish(result == Submission::NotSent ? Outcome::StartFailed : Outcome::TransportError);
        if (result == Submission::NotSent && !current_.resetRequired)
            current_.drained = true;
        retireIfDone();
        return true;
    }

    bool terminal(Firmware firmware, uint32_t uid, Outcome outcome)
    {
        if (!busy() || firmware != current_.firmware || uid != current_.uid ||
            current_.drained || current_.resetRequired ||
            (outcome != Outcome::Success && outcome != Outcome::Aborted &&
             outcome != Outcome::TransportError))
            return false;
        current_.drained = true;
        current_.terminalSeen = true;
        if (!current_.outcomeKnown)
            finish(current_.stopRequested ? current_.stopReason : outcome);
        retireIfDone();
        return true;
    }

    bool cancel(uint64_t token)
    {
        if (!matches(token) || current_.outcomeKnown || current_.stopRequested)
            return false;
        current_.stopRequested = true;
        current_.stopReason = Outcome::Aborted;
        return true;
    }

    // A timeout can complete the host request, but cannot release the radio.
    bool timeout(uint64_t token)
    {
        if (!matches(token) || current_.outcomeKnown)
            return false;
        current_.stopRequested = true;
        current_.stopReason = Outcome::TimedOut;
        finish(Outcome::TimedOut);
        return true;
    }

    bool transportFailed(uint64_t token)
    {
        if (!matches(token) || current_.outcomeKnown) return false;
        finish(Outcome::TransportError);
        return true;
    }

    // The scan ended without the firmware reporting it. The host side is
    // done with the request, so the slot can be reused. A late notification
    // is not ours any more: the slot is drained.
    bool abandon(uint64_t token)
    {
        if (!matches(token) || current_.drained) return false;
        current_.drained = true;
        current_.terminalSeen = true;
        if (!current_.outcomeKnown) finish(Outcome::TransportError);
        retireIfDone();
        return true;
    }

    bool beginAbort(uint64_t token)
    {
        if (!matches(token) || current_.drained || current_.resetRequired ||
            current_.command != Command::None ||
            current_.abortIssued || (!current_.stopRequested && !current_.outcomeKnown))
            return false;
        current_.abortIssued = true;
        current_.command = Command::Abort;
        return true;
    }

    bool aborted(uint64_t token, bool commandAccepted)
    {
        if (!matches(token) || current_.command != Command::Abort)
            return false;
        current_.command = Command::None;
        if (!commandAccepted && !current_.outcomeKnown)
            finish(Outcome::TransportError);
        // Neither an abort ACK nor a failed command proves the scan has stopped.
        retireIfDone();
        return true;
    }

    // Call only after hardware shutdown AND pending RX have been drained,
    // with a transport barrier preventing any later command-ring submission.
    // A still-running submission/abort must return before this slot is reusable.
    void hardwareStopped()
    {
        if (!busy())
            return;
        current_.drained = true;
        current_.resetRequired = false;
        if (!current_.outcomeKnown)
            finish(Outcome::DeviceStopped);
        retireIfDone();
    }

    bool completion(Completion &output) const
    {
        if (!busy() || !current_.outcomeKnown || current_.delivered ||
            current_.command != Command::None)
            return false;
        output = {current_.token, current_.outcome, current_.accepted};
        return true;
    }

    // A failed event allocation must leave the completion pending for retry.
    bool acknowledge(uint64_t token)
    {
        Completion pending;
        if (!matches(token) || !completion(pending))
            return false;
        current_.delivered = true;
        retireIfDone();
        return true;
    }

    bool busy() const { return current_.token != 0; }
    bool drained() const { return !busy() || current_.drained; }
    uint64_t token() const { return current_.token; }

private:
    enum class Command { None, Submit, Abort };
    struct State {
        uint64_t token;
        uint32_t uid;
        Firmware firmware;
        Command command;
        Outcome outcome;
        Outcome stopReason;
        bool accepted;
        bool stopRequested;
        bool abortIssued;
        bool drained;
        bool terminalSeen;
        bool resetRequired;
        bool outcomeKnown;
        bool delivered;
    } current_ = {};
    uint64_t sequence_ = 0;

    bool matches(uint64_t token) const { return token && current_.token == token; }
    void finish(Outcome outcome) { current_.outcome = outcome; current_.outcomeKnown = true; }
    void retireIfDone()
    {
        if (current_.drained && current_.delivered && current_.command == Command::None)
            current_ = {};
    }
};

struct ProbeIdentity {
    uint8_t source[6];
    uint8_t bssid[6];
};

inline bool validUnicast(const uint8_t *address)
{
    if (!address || (address[0] & 1))
        return false;
    uint8_t bits = 0;
    for (size_t i = 0; i < 6; ++i)
        bits |= address[i];
    return bits != 0;
}

inline bool validProbeIdentity(const ProbeIdentity &identity)
{
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    return validUnicast(identity.source) &&
           (validUnicast(identity.bssid) || memcmp(identity.bssid, broadcast, 6) == 0);
}

// Entropy must come from the kernel CSPRNG, never a timestamp or a PRNG seed.
// This prepares only the per-scan identity and never changes the interface MAC.
inline bool privateProbeIdentity(const uint8_t *entropy, const uint8_t *interfaceAddress,
                                 const uint8_t *bssid, ProbeIdentity &output)
{
    if (!entropy || !interfaceAddress || !bssid || !validUnicast(interfaceAddress))
        return false;
    ProbeIdentity identity;
    memcpy(identity.source, entropy, 6);
    identity.source[0] = (identity.source[0] & 0xfc) | 0x02;
    memcpy(identity.bssid, bssid, 6);
    if (!validProbeIdentity(identity) || memcmp(identity.source, interfaceAddress, 6) == 0)
        return false; // Caller retries with fresh entropy on a collision.
    output = identity;
    return true;
}

// Apply identity only to an existing 802.11 probe request header. Bad inputs
// leave the complete frame unchanged. The destination remains broadcast.
// Snapshot the identity first: it may alias addresses inside the input frame.
inline bool applyProbeIdentity(void *frame, size_t size, const ProbeIdentity &identity)
{
    if (!frame || size < 24 || !validProbeIdentity(identity))
        return false;
    auto *bytes = static_cast<uint8_t *>(frame);
    if (bytes[0] != 0x40 || bytes[1] != 0)
        return false;
    const ProbeIdentity saved = identity;
    memset(bytes + 4, 0xff, 6);
    memcpy(bytes + 10, saved.source, 6);
    memcpy(bytes + 16, saved.bssid, 6);
    return true;
}

} // namespace ItlScan
#endif
