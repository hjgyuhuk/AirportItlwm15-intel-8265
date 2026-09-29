#ifndef IWM_SCAN_TRIAL_H
#define IWM_SCAN_TRIAL_H

#include <HAL/IwmScanNotification.h>
#include <HAL/IwmScanFrame.h>

namespace ItlScan {

// The unattached 7000-series trial retains counters only, never SSIDs, BSSIDs
// or packet contents. Its caller serializes command, RX and stop under a gate.
// NativeScan also uses this coverage tracker with normalized IWX notifications.
// Hardware-specific channel exclusions must be enforced before begin(), by HAL.
class IwmPassiveTrial {
public:
    // IWM_MAX_SCAN_CHANNELS is 52 for the firmware family used by the native
    // LMAC path. This must match the wire channel array and WCL plan bound.
    static constexpr size_t MaxChannels = 52;
    static constexpr unsigned WaitSeconds = 10;
    struct Evidence {
        uint32_t requested = 0, iterations = 0, scanned = 0;
        uint32_t terminalStatus = UINT32_MAX, iterationStatus = UINT32_MAX;
        uint32_t phy = 0, mpdu = 0, beacons = 0, probeResponses = 0;
        uint32_t malformed = 0, badCrc = 0, ignored = 0;
        uint32_t phySeen = 0, phyLastBytes = 0, phyLastCfg = UINT32_MAX, phyLastChannel = UINT32_MAX;
        uint32_t phyBadSize = 0, phyBadConfig = 0, phyBadChannel = 0, missingPhy = 0;
    } evidence;

    uint64_t begin(const uint16_t *channels, size_t count)
    {
        if (started_ || !channels || !count || count > MaxChannels) return 0;
        for (size_t i = 0; i < count; ++i) {
            if (!channels[i] || channels[i] > 196) return 0;
            for (size_t j = 0; j < i; ++j)
                if (channels[i] == channels[j]) return 0;
        }
        const auto token = lifecycle_.begin(Firmware::LMAC);
        if (!token) return 0;
        memcpy(channels_, channels, count * sizeof(*channels));
        evidence.requested = static_cast<uint32_t>(count);
        started_ = true;
        return token;
    }

    bool submitted(uint64_t token, Submission result) { return lifecycle_.submitted(token, result); }
    bool timeout(uint64_t token) { return lifecycle_.timeout(token); }
    bool completion(Completion &output) const { return lifecycle_.completion(output); }
    bool drained() const { return lifecycle_.drained(); }
    void hardwareStopped() { lifecycle_.hardwareStopped(); lastChannel_ = 0; }
    bool acknowledge(uint64_t token) { return lifecycle_.acknowledge(token); }

    bool notification(uint16_t kind, const void *payload, size_t size)
    {
        if (!active()) { increment(evidence.ignored); return false; }
        ScanNotification notification;
        if (!decodeIwmNotification(static_cast<IwmNotification>(kind), payload, size, notification) ||
            notification.firmware != Firmware::LMAC) {
            increment(evidence.malformed);
            return false;
        }
        if (notification.terminal) {
            Outcome outcome;
            if (!terminalOutcome(notification, outcome) ||
                !lifecycle_.terminal(Firmware::LMAC, 0, outcome)) return false;
            evidence.terminalStatus = notification.status;
            lastChannel_ = 0;
            return true;
        }
        // One requested iteration, with exactly one result per requested
        // channel. Do not infer coverage from the firmware's count alone.
        if (evidence.iterations || notification.channels > evidence.requested) {
            increment(evidence.malformed);
            return false;
        }
        bool seen[MaxChannels] = {};
        const auto *bytes = static_cast<const uint8_t *>(payload) + 12;
        for (size_t i = 0; i < notification.channels; ++i) {
            const auto channel = bytes[8 * i];
            const auto band = bytes[8 * i + 1];
            const size_t index = channelIndex(channel);
            if (index == MaxChannels || seen[index] || band != (channel <= 14 ? 1 : 0)) {
                increment(evidence.malformed);
                return false;
            }
            seen[index] = true;
        }
        if (notification.status != 1 && notification.status != 2) {
            increment(evidence.malformed);
            return false;
        }
        evidence.iterations = 1;
        evidence.iterationStatus = notification.status;
        evidence.scanned = notification.channels;
        return false; // An iteration is not the terminal notification.
    }

    // Legacy RX_PHY (68-byte prefix) and RX_MPDU (4-byte prefix, frame, 4-byte
    // status). ABI assertions in scan.cpp keep these offsets synchronized.
    void rxPhy(const void *payload, size_t size)
    {
        lastChannel_ = 0;
        if (!active()) return;
        const auto *bytes = static_cast<const uint8_t *>(payload);
        increment(evidence.phySeen);
        evidence.phyLastBytes = size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
        evidence.phyLastCfg = bytes && size >= 2 ? bytes[1] : UINT32_MAX;
        evidence.phyLastChannel = bytes && size >= 24 ? read16(bytes + 22) : UINT32_MAX;
        LegacyPhy phy;
        const auto result = decodeLegacyPhy(payload, size, phy);
        if (result != PhyDecode::Accepted || channelIndex(phy.channel) == MaxChannels) {
            increment(evidence.malformed);
            if (result == PhyDecode::BadSize) increment(evidence.phyBadSize);
            else if (result == PhyDecode::BadConfig) increment(evidence.phyBadConfig);
            else increment(evidence.phyBadChannel);
            return;
        }
        lastChannel_ = phy.channel;
        increment(evidence.phy);
    }

    void rxMpdu(const void *payload, size_t size)
    {
        if (!active()) return;
        const auto *bytes = static_cast<const uint8_t *>(payload);
        if (!bytes || size < 8 || read16(bytes) > size - 8 || read16(bytes) < 24) {
            increment(evidence.malformed);
            return;
        }
        const auto length = read16(bytes);
        const auto *frame = bytes + 4;
        if ((bytes[4 + length] & 3) != 3) { increment(evidence.badCrc); return; }
        increment(evidence.mpdu);
        if (!lastChannel_) { increment(evidence.missingPhy); return; }
        if (frame[0] != 0x80 && frame[0] != 0x50) return;
        BeaconView view;
        if (!decodeScanBeacon(frame, length, view)) { increment(evidence.malformed); return; }
        increment(view.probeResponse ? evidence.probeResponses : evidence.beacons);
    }

    bool completeCoverage() const
    {
        return evidence.terminalStatus == 1 && evidence.iterationStatus == 1 && evidence.iterations == 1 &&
            evidence.requested && evidence.scanned == evidence.requested;
    }

private:
    Lifecycle lifecycle_;
    uint16_t channels_[MaxChannels] = {}, lastChannel_ = 0;
    bool started_ = false;
    bool active() const { return lifecycle_.busy() && !lifecycle_.drained(); }
    static uint16_t read16(const uint8_t *bytes) { return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8); }
    static void increment(uint32_t &value) { if (value != UINT32_MAX) ++value; }
    size_t channelIndex(uint16_t channel) const
    {
        for (size_t i = 0; i < evidence.requested; ++i)
            if (channels_[i] == channel) return i;
        return MaxChannels;
    }
};

} // namespace ItlScan
#endif
