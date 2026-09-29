#ifndef SEQUOIA_SCAN_PLAN_H
#define SEQUOIA_SCAN_PLAN_H

#include <Airport/SequoiaWCL.h>
#include <HAL/IwmScanFrame.h>

namespace SequoiaWCL {

enum class ScanPlanStatus { Success, BadArgument, Unsupported, NoSpace };

// Stable post-boot codes. A rejection remains an ordinary IOReturn; no live
// probing of unsupported private controls is needed to identify its cause.
enum class ScanPlanReason : uint32_t {
    None = 0, Bounds = 1, Inventory = 2, Identity = 3, Version = 4,
    Associated = 5, Type = 6, PolicyFlags = 7, Control = 8, HomeAway = 9,
    ShortSSID = 10, ActiveDwell = 11, RestTime = 12, PassiveDwell = 13,
    BssType = 14, Bssid = 15, SsidForms = 16, SsidVersion = 17,
    SsidControl = 18, InventoryChannel = 19, InventoryDuplicate = 20,
    ChannelCapacity = 21, ChannelVersion = 22, ChannelFlags = 23,
    ChannelAbsent = 24, ChannelCapabilities = 25, ChannelDuplicate = 26,
    Entropy = 27
};

struct ScanPlanDiagnostic {
    ScanPlanReason reason = ScanPlanReason::None;
    uint32_t channelIndex = UINT32_MAX;
    uint32_t channel = UINT32_MAX;
    uint32_t requestedFlags = 0;
    uint32_t inventoryFlags = 0;
};

// Shared IWM/IWX scan plan. Per-channel probe permission is owned
// separately from the requested scan mode and never inferred from PHY mode.
// Unknown controls are rejected, never silently downgraded to a generic scan.
class ScanPlan {
public:
    // The 7000-series LMAC firmware advertises up to 52 scan channels. Keep
    // the WCL request and the first-generation wire builder at that same
    // bound; a smaller value rejects a complete regulatory inventory before
    // the worker can even start.
    static constexpr size_t MaxChannels = 52;
    uint16_t channels[MaxChannels] = {};
    size_t channelCount = 0;
    uint8_t source[6] = {};
    uint8_t passiveDwell = 110;
    uint8_t activeDwell = 10;
    bool probes[MaxChannels] = {};
    ScanPlanDiagnostic diagnostic;
    bool passiveFallback = false;

    // Caller owns stable request, NVM inventory and fresh CSPRNG entropy.
    // No allocations; failure invalidates this plan so it cannot reuse a prior scan.
    ScanPlanStatus prepare(const void *input, size_t bytes, const Channel *inventory,
                           size_t count, const uint8_t *mac, const uint8_t *entropy,
                           size_t entropyBytes, bool associated)
    {
        *this = ScanPlan{};
        if (!validScanRequestBounds(input, bytes))
            return fail(ScanPlanStatus::BadArgument, ScanPlanReason::Bounds);
        if (!inventory || !count || count > MaxChannels)
            return fail(ScanPlanStatus::BadArgument, ScanPlanReason::Inventory);
        if (!ItlScan::validUnicast(mac))
            return fail(ScanPlanStatus::BadArgument, ScanPlanReason::Identity);
        // Input may be unaligned; avoid a 5 KB request copy on the kernel stack.
        const auto *raw = static_cast<const uint8_t *>(input);
        auto u32 = [raw](size_t offset) { uint32_t value; memcpy(&value, raw + offset, 4); return value; };
        uint16_t flags;
        memcpy(&flags, raw + offsetof(ScanRequest, scanFlags), sizeof(flags));
        if (u32(0) != 1 || raw[4] > 1)
            return fail(ScanPlanStatus::BadArgument, ScanPlanReason::Version);
        const auto type = u32(offsetof(ScanRequest, scanType));
        // 24G830's IO80211ScanRequest converter supplies 0x8 for the normal
        // WCL scan policy and adds 0x2 for its ordinary passive/unassociated
        // request. These are Apple request-policy bits, not firmware flags;
        // the native path derives probe/passive behavior from scanType and
        // the per-channel NVM inventory below. Keep the accepted set bounded
        // so an unrecognized policy cannot be silently downgraded.
        constexpr uint16_t AppleScanPolicyFlags = 0x000a;
        const auto homeAway = u32(offsetof(ScanRequest, homeAwayTime));
        // 24G830 uses 110 ms as the ordinary home/away dwell default for
        // active airportd scans. It is only a scheduling hint for the native
        // one-pass request, so accept that observed value and the unset/
        // sentinel forms, while rejecting arbitrary private controls.
        // The one-pass LMAC scan historically only served IEEE80211_S_INIT, and
        // r213 showed an associated request is not free to admit: 106 requests
        // produced one real scan of 17 beacons, mostly the associated AP itself,
        // which overwrote the cached neighbour list and emptied the menu bar's
        // "other networks". The caller therefore passes `associated && !
        // admitAssociated`: unassociated requests always proceed, and associated
        // ones are refused here unless the caller opted into the bgscan-style
        // associated mode (itlwm_assoc_scan boot-arg), in which case the plan is
        // marked through markAssociatedScan below and its results pass a health
        // gate that withholds an unhealthy set instead of replacing the cache.
        if (associated) return unsupported(ScanPlanReason::Associated);
        if (type != 1 && type != 2) return unsupported(ScanPlanReason::Type);
        if (flags & ~AppleScanPolicyFlags) return unsupported(ScanPlanReason::PolicyFlags);
        if (u32(offsetof(ScanRequest, control0c))) return unsupported(ScanPlanReason::Control);
        if (homeAway && homeAway != UINT32_MAX && homeAway != 110)
            return unsupported(ScanPlanReason::HomeAway);
        if (u32(offsetof(ScanRequest, shortSSID))) return unsupported(ScanPlanReason::ShortSSID);
        active_ = type == 1;
        if (active_) {
            const auto dwell = u32(offsetof(ScanRequest, activeDwell));
            if (!dwell || (dwell > 255 && dwell != UINT32_MAX)) return unsupported(ScanPlanReason::ActiveDwell);
            activeDwell = dwell == UINT32_MAX ? 10 : static_cast<uint8_t>(dwell);
        }
        const auto rest = u32(offsetof(ScanRequest, restTime));
        // Apple substitutes its 45 ms default when the caller leaves the
        // rest-time field unset. It is a scheduler hint and is not needed by
        // the one-pass native LMAC request, so retain only that observed
        // system value (plus the unset sentinel) rather than rejecting every
        // normal airportd scan.
        if (rest && rest != UINT32_MAX && rest != 45) return unsupported(ScanPlanReason::RestTime);
        const auto dwell = u32(offsetof(ScanRequest, passiveDwell));
        if (!dwell || (dwell > 255 && dwell != UINT32_MAX)) return unsupported(ScanPlanReason::PassiveDwell);
        passiveDwell = dwell == UINT32_MAX ? 110 : static_cast<uint8_t>(dwell);
        // r163 clamped these to 25 ms active and 60 ms passive to shorten the window
        // in which a scan holds the WCL scan-manager FSM - every client request
        // inside it is rejected with EBUSY - and the boot showed the clamp did what
        // it promised: a scan went from about 4 s to 2.1 s (13:43:29.968 IN_PROGRESS
        // to 13:43:32.074 IDLE). But it also cost results, which is the trade being
        // reversed here: r162 at the caller's own dwells reported 81..91 networks per
        // completed scan and r163 reports 62..72. A 60 ms passive dwell is already
        // below the roughly 100 ms beacon interval, so shortening it further can only
        // lose more, and the shorter window did not put a network list in front of
        // the menu either. The caller's dwells are therefore used unchanged again.
        const auto bss = u32(offsetof(ScanRequest, bssType));
        // 3 is Apple's wildcard BSS type used by a normal full scan; 1 and 2
        // remain infrastructure/IBSS filters and 0 means unspecified.
        if (bss > 3) return unsupported(ScanPlanReason::BssType);
        bssType_ = bss;
        memcpy(bssid_, raw + offsetof(ScanRequest, bssid), 6);
        const uint8_t zero[6] = {}, broadcast[6] = {255,255,255,255,255,255};
        filterBssid_ = memcmp(bssid_, zero, 6) && memcmp(bssid_, broadcast, 6);
        if (filterBssid_ && !ItlScan::validUnicast(bssid_))
            return fail(ScanPlanStatus::BadArgument, ScanPlanReason::Bssid);
        const auto primaryLength = u32(offsetof(ScanRequest, ssidLength));
        const auto listCount = u32(offsetof(ScanRequest, ssidCount));
        // The two forms have different precedence in the stock adapter.
        if (primaryLength && listCount) return unsupported(ScanPlanReason::SsidForms);
        if (primaryLength) {
            ssidCount_ = 1;
            ssids_[0].length = primaryLength;
            memcpy(ssids_[0].bytes, raw + offsetof(ScanRequest, ssid), primaryLength);
        } else {
            ssidCount_ = listCount;
            for (size_t i = 0; i < listCount; ++i) {
                const size_t offset = offsetof(ScanRequest, ssids) + i * sizeof(ScanSSID);
                // 24H23's Apple80211Scan argument parser appends one zeroed
                // wildcard entry after its version-1 directed SSIDs (including
                // an empty input list). IO80211ScanRequest copies it unchanged.
                // Admit only that terminal, entirely zero version-0 form; keep
                // it in the plan so matches() preserves wildcard discovery.
                const auto version = u32(offset);
                if (version == 0 && i + 1 == listCount) {
                    for (size_t j = 4; j < sizeof(ScanSSID); ++j)
                        if (raw[offset + j])
                            return fail(ScanPlanStatus::BadArgument, ScanPlanReason::SsidVersion);
                } else if (version != 1) {
                    return fail(ScanPlanStatus::BadArgument, ScanPlanReason::SsidVersion);
                }
                for (size_t j = 0; j < sizeof(ScanSSID::opaque); ++j)
                    if (raw[offset + offsetof(ScanSSID, opaque) + j]) return unsupported(ScanPlanReason::SsidControl);
                ssids_[i].length = u32(offset + offsetof(ScanSSID, length));
                memcpy(ssids_[i].bytes, raw + offset + offsetof(ScanSSID, bytes), ssids_[i].length);
            }
        }
        // Hardware exclusions belong to the HAL inventory: AX210 supports
        // channel 144, while the 7000-series IWM PHY map excludes it there.
        const uint32_t knownFlags = Width20MHz | Band2GHz | Band5GHz | ActiveScan | DFS;
        for (size_t i = 0; i < count; ++i) {
            const auto &channel = inventory[i];
            const auto band = channel.number <= 14 ? Band2GHz : Band5GHz;
            if (channel.version != 1 || !channel.number || channel.number > 196 ||
                (channel.flags & ~knownFlags) || !(channel.flags & Width20MHz) ||
                (channel.flags & (Band2GHz | Band5GHz)) != band) {
                recordChannel(i, channel, channel.flags);
                return fail(ScanPlanStatus::BadArgument, ScanPlanReason::InventoryChannel);
            }
            for (size_t j = 0; j < i; ++j)
                if (inventory[j].number == channel.number) {
                    recordChannel(i, channel, channel.flags);
                    return fail(ScanPlanStatus::BadArgument, ScanPlanReason::InventoryDuplicate);
                }
        }
        const auto requested = u32(offsetof(ScanRequest, channelCount));
        if (requested > MaxChannels) return fail(ScanPlanStatus::NoSpace, ScanPlanReason::ChannelCapacity);
        if (!requested) {
            for (size_t i = 0; i < count; ++i) {
                probes[channelCount] = active_ && (inventory[i].flags & ActiveScan) && !(inventory[i].flags & DFS);
                channels[channelCount++] = inventory[i].number;
            }
        } else {
            for (size_t i = 0; i < requested; ++i) {
                Channel channel;
                memcpy(&channel, raw + offsetof(ScanRequest, channels) + i * sizeof(Channel), sizeof(channel));
                recordChannel(i, channel, 0);
                if (channel.version != 1) return fail(ScanPlanStatus::BadArgument, ScanPlanReason::ChannelVersion);
                if (channel.flags & ~knownFlags) return unsupported(ScanPlanReason::ChannelFlags);
                const Channel *found = nullptr;
                for (size_t j = 0; j < count; ++j)
                    if (inventory[j].number == channel.number) found = inventory + j;
                if (!found) return unsupported(ScanPlanReason::ChannelAbsent);
                diagnostic.inventoryFlags = found->flags;
                if (channel.flags & ~found->flags) return unsupported(ScanPlanReason::ChannelCapabilities);
                for (size_t j = 0; j < channelCount; ++j)
                    if (channels[j] == channel.number)
                        return fail(ScanPlanStatus::BadArgument, ScanPlanReason::ChannelDuplicate);
                probes[channelCount] = active_ && (found->flags & ActiveScan) && !(found->flags & DFS);
                channels[channelCount++] = channel.number;
            }
        }
        if (active_) {
            bool permitted = false;
            for (size_t i = 0; i < channelCount; ++i) permitted |= probes[i];
            // An active scan already listens passively on NVM/DFS-restricted
            // channels within a mixed plan. The same channel requested alone
            // must remain listen-only too. Preserve filters and passive dwell,
            // and use the HAL's fully passive wire mode with zero probe SSIDs.
            if (!permitted) { active_ = false; passiveFallback = true; }
        }
        if (raw[4]) {
            ItlScan::ProbeIdentity identity;
            if (entropyBytes < 6 || !ItlScan::privateProbeIdentity(entropy, mac, broadcast, identity))
                return fail(ScanPlanStatus::BadArgument, ScanPlanReason::Entropy);
            memcpy(source, identity.source, 6);
        } else memcpy(source, mac, 6);
        valid_ = true;
        diagnostic = {};
        return ScanPlanStatus::Success;
    }

    bool valid() const { return valid_; }
    bool active() const { return active_; }
    size_t probeSSIDCount() const { return active_ ? (ssidCount_ ? ssidCount_ : 1) : 0; }
    size_t probeSSIDLength(size_t i) const { return i < ssidCount_ ? ssids_[i].length : 0; }
    const uint8_t *probeSSID(size_t i) const { return i < ssidCount_ ? ssids_[i].bytes : nullptr; }
    const uint8_t *probeBSSID() const { return filterBssid_ ? bssid_ : nullptr; }
    // Associated bgscan-style mode. Called only after a successful prepare, so
    // the `*this = ScanPlan{}` reset inside prepare cannot clear it. The channel
    // is the serving BSS channel (ic_bss->ni_chan): the HAL derives the
    // fragmented passive LMAC request from it and the health gate measures
    // non-AP channel evidence against it.
    void markAssociatedScan(uint16_t channel)
    {
        associated_ = true;
        assocChannel_ = channel;
    }
    bool associatedScan() const { return associated_; }
    uint16_t assocChannel() const { return assocChannel_; }

    bool matches(const ItlScan::LegacyPhy &phy, const ItlScan::BeaconView &beacon) const
    {
        if (!valid_ || !beacon.bssid || (beacon.ssidLength && !beacon.ssid) || beacon.ssidLength > 32)
            return false;
        bool channel = false;
        for (size_t i = 0; i < channelCount; ++i) channel |= channels[i] == phy.channel;
        if (!channel || (filterBssid_ && memcmp(bssid_, beacon.bssid, 6))) return false;
        if ((bssType_ == 1 && (beacon.capability & 3) != 1) ||
            (bssType_ == 2 && (beacon.capability & 3) != 2)) return false;
        if (!ssidCount_) return true;
        for (size_t i = 0; i < ssidCount_; ++i)
            if (!ssids_[i].length || (ssids_[i].length == beacon.ssidLength &&
                !memcmp(ssids_[i].bytes, beacon.ssid, beacon.ssidLength))) return true;
        return false;
    }

private:
    ScanPlanStatus fail(ScanPlanStatus status, ScanPlanReason reason)
    {
        diagnostic.reason = reason;
        return status;
    }
    ScanPlanStatus unsupported(ScanPlanReason reason) { return fail(ScanPlanStatus::Unsupported, reason); }
    void recordChannel(size_t index, const Channel &channel, uint32_t flags)
    {
        diagnostic.channelIndex = static_cast<uint32_t>(index);
        diagnostic.channel = channel.number;
        diagnostic.requestedFlags = channel.flags;
        diagnostic.inventoryFlags = flags;
    }
    bool valid_ = false, filterBssid_ = false, active_ = false, associated_ = false;
    uint16_t assocChannel_ = 0;
    uint32_t bssType_ = 0;
    uint8_t bssid_[6] = {};
    struct SSID { uint32_t length = 0; uint8_t bytes[32] = {}; } ssids_[kMaxScanSSIDs];
    size_t ssidCount_ = 0;
};

} // namespace SequoiaWCL
#endif
