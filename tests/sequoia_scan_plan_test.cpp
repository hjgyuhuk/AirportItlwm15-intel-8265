#include <Airport/SequoiaScanPlan.h>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace SequoiaWCL;

int main()
{
    const Channel inventory[] = {{1, 1, Width20MHz | Band2GHz | ActiveScan},
                                 {1, 36, Width20MHz | Band5GHz | ActiveScan},
                                 {1, 52, Width20MHz | Band5GHz | DFS}};
    const uint8_t mac[6] = {0x88,1,2,3,4,5}, entropy[6] = {0x55,6,7,8,9,10};
    ScanRequest request = {};
    request.version = 1;
    request.privateMAC = 1;
    request.scanType = 2;
    request.passiveDwell = UINT32_MAX;
    ScanPlan plan;
    auto prepare = [&](const ScanRequest &r, bool associated = false) {
        std::vector<uint8_t> unaligned(sizeof(r) + 1);
        memcpy(unaligned.data() + 1, &r, sizeof(r));
        return plan.prepare(unaligned.data() + 1, sizeof(r), inventory, 3, mac, entropy, 6, associated);
    };
    assert(prepare(request) == ScanPlanStatus::Success);
    assert(plan.valid() && plan.channelCount == 3 && plan.passiveDwell == 110);
    assert(plan.source[0] == 0x56 && memcmp(plan.source, mac, 6));
    assert(mac[0] == 0x88);
    // These are the policy bits and scheduler default emitted by the native
    // 24G830 IO80211ScanRequest converter for an ordinary passive scan.
    request.scanFlags = 0x000a;
    request.restTime = 45;
    assert(prepare(request) == ScanPlanStatus::Success);
    request.scanFlags = 0;
    request.restTime = 0;
    request.homeAwayTime = 110;
    request.bssType = 3;
    assert(prepare(request) == ScanPlanStatus::Success);
    request.homeAwayTime = 0;
    request.bssType = 0;
    const uint8_t bssid[] = {2,1,2,3,4,5};
    const uint8_t ssid[] = {'n',0,'t'};
    ItlScan::BeaconView beacon;
    beacon.bssid = bssid; beacon.ssid = ssid; beacon.ssidLength = 3; beacon.capability = 1;
    assert(plan.matches({36, -40}, beacon) && !plan.matches({44, -40}, beacon));
    request.bssType = 1;
    memcpy(request.bssid, bssid, 6);
    request.ssidLength = 3;
    memcpy(request.ssid, ssid, 3);
    request.channelCount = 1;
    request.channels[0] = inventory[1];
    request.passiveDwell = 90;
    assert(prepare(request) == ScanPlanStatus::Success && plan.passiveDwell == 90);
    assert(plan.channelCount == 1 && plan.channels[0] == 36);
    assert(plan.matches({36, -40}, beacon) && !plan.matches({1, -40}, beacon));
    beacon.capability = 2;
    assert(!plan.matches({36, -40}, beacon));
    beacon.capability = 1;
    beacon.ssidLength = 1;
    assert(!plan.matches({36, -40}, beacon));
    beacon.ssidLength = 3;
    const ScanPlanReason expectedReasons[] = {
        ScanPlanReason::Version, ScanPlanReason::Version, ScanPlanReason::Type,
        ScanPlanReason::PolicyFlags, ScanPlanReason::Control, ScanPlanReason::HomeAway,
        ScanPlanReason::ShortSSID, ScanPlanReason::RestTime, ScanPlanReason::PassiveDwell,
        ScanPlanReason::PassiveDwell, ScanPlanReason::BssType, ScanPlanReason::Bssid,
        ScanPlanReason::SsidForms, ScanPlanReason::ChannelVersion, ScanPlanReason::ChannelAbsent,
        ScanPlanReason::ChannelCapabilities, ScanPlanReason::ChannelVersion,
        ScanPlanReason::ChannelDuplicate, ScanPlanReason::ChannelFlags
    };
    for (unsigned fault = 0; fault < 19; ++fault) {
        auto bad = request;
        switch (fault) {
        case 0: bad.version = 2; break;
        case 1: bad.privateMAC = 2; break;
        case 2: bad.scanType = 3; break;
        case 3: bad.scanFlags = 1; break;
        case 4: bad.control0c = 1; break;
        case 5: bad.homeAwayTime = 1; break;
        case 6: bad.shortSSID = 1; break;
        case 7: bad.restTime = 10; break;
        case 8: bad.passiveDwell = 256; break;
        case 9: bad.passiveDwell = 0; break;
        case 10: bad.bssType = 4; break;
        case 11: bad.bssid[0] = 3; break;
        case 12: bad.ssidCount = 1; break;
        case 13: bad.channelCount = 41; break;
        case 14: bad.channels[0].number = 144; break;
        case 15: bad.channels[0].flags |= Band2GHz; break;
        case 16: bad.channels[0].version = 2; break;
        case 17: bad.channels[1] = bad.channels[0]; bad.channelCount = 2; break;
        case 18: bad.channels[0].flags |= 0x40000000; break;
        }
        assert(prepare(bad) != ScanPlanStatus::Success);
        assert(plan.diagnostic.reason == expectedReasons[fault]);
        if (fault == 15) {
            assert(plan.diagnostic.channel == 36 && plan.diagnostic.channelIndex == 0);
            assert(plan.diagnostic.requestedFlags == bad.channels[0].flags);
            assert(plan.diagnostic.inventoryFlags == inventory[1].flags);
        }
        assert(!plan.valid() && !plan.matches({36, -40}, beacon));
        assert(prepare(request) == ScanPlanStatus::Success);
        assert(plan.diagnostic.reason == ScanPlanReason::None);
    }
    // The HAL's LMAC scan only runs in IEEE80211_S_INIT, so an associated request is
    // refused here and the caller keeps the last unassociated scan. r213 tried
    // admitting it and emptied the menu bar's "other networks".
    assert(prepare(request, true) == ScanPlanStatus::Unsupported && !plan.valid());
    request.ssidLength = 0;
    request.ssidCount = 2;
    request.ssids[0].version = request.ssids[1].version = 1;
    request.ssids[0].length = 1; request.ssids[0].bytes[0] = 'x';
    request.ssids[1].length = 3; memcpy(request.ssids[1].bytes, ssid, 3);
    assert(prepare(request) == ScanPlanStatus::Success && plan.matches({36, -40}, beacon));
    request.ssids[0].opaque[0] = 1;
    assert(prepare(request) == ScanPlanStatus::Unsupported && !plan.valid());
    request.ssids[0].opaque[0] = 0;
    assert(plan.prepare(&request, sizeof(request), inventory, 3, mac, nullptr, 6, false) == ScanPlanStatus::BadArgument);
    assert(plan.prepare(&request, sizeof(request), inventory, 3, mac, entropy, 5, false) == ScanPlanStatus::BadArgument);
    request.privateMAC = 0;
    assert(plan.prepare(&request, sizeof(request), inventory, 3, mac, nullptr, 0, false) == ScanPlanStatus::Success);
    assert(!memcmp(plan.source, mac, 6));
    request.scanType = 1; request.activeDwell = UINT32_MAX;
    request.channelCount = 0;
    assert(prepare(request) == ScanPlanStatus::Success && plan.active() && plan.activeDwell == 10);
    assert(plan.probes[0] && plan.probes[1] && !plan.probes[2]);
    assert(plan.probeSSIDCount() == 2 && plan.probeSSIDLength(1) == 3);
    assert(!memcmp(plan.probeSSID(1), ssid, 3) && !memcmp(plan.probeBSSID(), bssid, 6));
    for (const auto dwell : {0u,256u,UINT32_MAX - 1}) {
        request.activeDwell = dwell;
        assert(prepare(request) == ScanPlanStatus::Unsupported && !plan.valid());
    }
    request.activeDwell = 40;
    request.channelCount = 1; request.channels[0] = inventory[2];
    assert(prepare(request) == ScanPlanStatus::Success);
    assert(plan.passiveFallback && !plan.active() && !plan.probeSSIDCount());
    assert(plan.channelCount == 1 && plan.channels[0] == 52 && !plan.probes[0]);
    // Listen-only fallback still enforces the directed SSID/BSSID filters.
    assert(plan.matches({52, -40}, beacon));
    beacon.ssidLength = 2;
    assert(!plan.matches({52, -40}, beacon));
    beacon.ssidLength = 3;
    request.channelCount = 0; request.ssidCount = 0;
    memset(request.bssid, 0, 6);
    assert(prepare(request) == ScanPlanStatus::Success && plan.probeSSIDCount() == 1);
    assert(!plan.passiveFallback);
    assert(plan.probeSSIDLength(0) == 0 && !plan.probeSSID(0) && !plan.probeBSSID());
    request.scanType = 2;
    assert(prepare(request) == ScanPlanStatus::Success && !plan.active() && !plan.probeSSIDCount());
    for (auto probe : plan.probes) assert(!probe);
    // r240 AX210 reports 38 available channels, with DFS channel 144 at
    // inventory index 32. A generation-neutral plan must trust the HAL's
    // admitted inventory instead of imposing the older IWM calibration map.
    std::vector<Channel> ax210;
    for (unsigned n = 1; n <= 13; ++n) ax210.push_back({1, n, Width20MHz | Band2GHz | ActiveScan});
    for (unsigned n = 36; n <= 64; n += 4)
        ax210.push_back({1, n, Width20MHz | Band5GHz | (n >= 52 ? DFS : ActiveScan)});
    for (unsigned n = 100; n <= 144; n += 4) ax210.push_back({1, n, Width20MHz | Band5GHz | DFS});
    for (unsigned n = 149; n <= 165; n += 4) ax210.push_back({1, n, Width20MHz | Band5GHz | ActiveScan});
    assert(ax210.size() == 38 && ax210[32].number == 144 && ax210[32].flags == 0x112);
    ScanRequest axRequest = {};
    axRequest.version = 1; axRequest.privateMAC = 1;
    axRequest.scanType = 2; axRequest.scanFlags = 8;
    axRequest.homeAwayTime = 110; axRequest.bssType = 3;
    axRequest.activeDwell = 40; axRequest.passiveDwell = 110; axRequest.restTime = 45;
    axRequest.channelCount = ax210.size();
    memcpy(axRequest.channels, ax210.data(), ax210.size() * sizeof(Channel));
    for (unsigned mode : {1u, 2u}) {
        axRequest.scanType = mode;
        assert(plan.prepare(&axRequest, sizeof(axRequest), ax210.data(), ax210.size(),
                            mac, entropy, 6, false) == ScanPlanStatus::Success);
        assert(plan.channelCount == 38 && plan.channels[32] == 144 && !plan.probes[32]);
        assert(plan.probes[0] == (mode == 1));
    }
    // DFS never gains active probing from request flags. A channel absent
    // from the hardware inventory remains rejected, including on older IWM.
    axRequest.channelCount = 1; axRequest.channels[0] = ax210[32];
    axRequest.scanType = 1;
    assert(plan.prepare(&axRequest, sizeof(axRequest), ax210.data(), ax210.size(),
                        mac, entropy, 6, false) == ScanPlanStatus::Success);
    assert(plan.passiveFallback && !plan.active() && !plan.probes[0]);
    assert(prepare(axRequest) == ScanPlanStatus::Unsupported);
    assert(plan.diagnostic.reason == ScanPlanReason::ChannelAbsent);
    puts("WCL scan plan: active/passive, NVM/DFS restriction, binary filters, private identity and rejection cases passed");
}
