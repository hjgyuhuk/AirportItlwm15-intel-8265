#include <Airport/SequoiaScanPlan.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace SequoiaWCL;
extern "C" void apple_copy_ssid_list(void *, void *);

int main(int argc, char **argv)
{
    assert(argc == 2);
    const Channel inventory[] = {{1, 1, Width20MHz | Band2GHz | ActiveScan},
                                 {1, 52, Width20MHz | Band5GHz | DFS}};
    const uint8_t mac[] = {2,1,2,3,4,5}, entropy[] = {4,5,6,7,8,9};
    const uint8_t unrelated[] = {'z'}, peer[] = {2,6,7,8,9,10};
    ItlScan::BeaconView beacon;
    beacon.bssid = peer; beacon.ssid = unrelated; beacon.ssidLength = 1; beacon.capability = 1;
    unsigned cases = 0;
    for (unsigned count = 0; count <= 9; ++count) {
        std::vector<uint8_t> stored(0x15a0);
        std::ifstream file(std::string(argv[1]) + "/list-" + std::to_string(count) + ".bin", std::ios::binary);
        assert(file.read(reinterpret_cast<char *>(stored.data() + 8), 0x1598));
        uint8_t object[24] = {};
        const auto pointer = stored.data();
        memcpy(object + 0x10, &pointer, sizeof(pointer));
        ScanRequest request = {};
        // Execute Apple's original list-copy instructions on the actual parser
        // output. The remainder is a synthetic ordinary active scan, not an
        // execution model of the complete IO80211ScanRequest converter.
        apple_copy_ssid_list(object, &request);
        assert(request.ssidCount == count + 1);
        for (unsigned i = 0; i < count; ++i) {
            assert(request.ssids[i].version == 1 && request.ssids[i].length == i + 1);
            for (unsigned j = 0; j <= i; ++j) assert(request.ssids[i].bytes[j] == 'x');
        }
        request.version = 1; request.privateMAC = 1;
        request.scanFlags = 8; request.scanType = 1;
        request.activeDwell = 40; request.passiveDwell = 110;
        request.homeAwayTime = 110; request.restTime = 45; request.bssType = 3;
        ScanPlan plan;
        auto prepare = [&](const ScanRequest &r) {
            ++cases;
            return plan.prepare(&r, sizeof(r), inventory, 2, mac, entropy, 6, false);
        };
        for (unsigned alignment = 0; alignment < 8; ++alignment) {
            std::vector<uint8_t> unaligned(sizeof(request) + alignment);
            memcpy(unaligned.data() + alignment, &request, sizeof(request));
            assert(plan.prepare(unaligned.data() + alignment, sizeof(request), inventory, 2,
                                mac, entropy, 6, false) == ScanPlanStatus::Success);
            ++cases;
            assert(plan.matches({1,-40}, beacon)); // Trailing wildcard must survive.
            assert(!plan.matches({36,-40}, beacon)); // Still restrict channels.
            assert(plan.probeSSIDCount() == count + 1 && plan.probeSSIDLength(count) == 0);
            assert(plan.probes[0] && !plan.probes[1]); // DFS stays passive.
        }
        // Only an entirely zero, final entry is the audited version-zero form.
        for (unsigned byte = 4; byte < sizeof(ScanSSID); ++byte) {
            auto bad = request;
            reinterpret_cast<uint8_t *>(&bad.ssids[count])[byte] = 1;
            assert(prepare(bad) == ScanPlanStatus::BadArgument);
            assert(!plan.valid() && !plan.matches({1,-40}, beacon));
            assert(prepare(request) == ScanPlanStatus::Success);
        }
        auto bad = request;
        bad.ssids[count].version = 2;
        assert(prepare(bad) == ScanPlanStatus::BadArgument && !plan.valid());
        if (count) {
            bad = request; bad.ssids[0].version = 0;
            assert(prepare(bad) == ScanPlanStatus::BadArgument);
            bad = request; memset(&bad.ssids[0], 0, sizeof(ScanSSID));
            assert(prepare(bad) == ScanPlanStatus::BadArgument); // Not terminal.
            bad = request; bad.ssidCount = count; // Directed-only keeps filter.
            assert(prepare(bad) == ScanPlanStatus::Success && !plan.matches({1,-40}, beacon));
            bad.ssids[0].opaque[0] = 1;
            assert(prepare(bad) == ScanPlanStatus::Unsupported);
        }
        bad = request; bad.ssidCount = 11;
        assert(prepare(bad) == ScanPlanStatus::BadArgument);
        bad = request; bad.ssids[count].version = 1;
        assert(prepare(bad) == ScanPlanStatus::Success && plan.matches({1,-40}, beacon));
        request.scanType = 2;
        assert(prepare(request) == ScanPlanStatus::Success && !plan.probeSSIDCount());
        plan.markAssociatedScan(1);
        assert(plan.associatedScan() && plan.matches({1,-40}, beacon));
        memcpy(request.bssid, mac, 6);
        assert(prepare(request) == ScanPlanStatus::Success && !plan.matches({1,-40}, beacon));
        request.bssType = 2;
        memcpy(request.bssid, peer, 6);
        assert(prepare(request) == ScanPlanStatus::Success && !plan.matches({1,-40}, beacon));
    }
    printf("%u SSID-list cases passed with actual framework parser/list-copy bytes; hardware boundaries not executed\n", cases);
}
