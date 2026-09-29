#include <Airport/SequoiaScanFrame.h>
#include <HAL/IwmScanTrial.h>
#include <cassert>
#include <cstdio>
#include <vector>

static std::vector<uint8_t> beacon(bool probe, unsigned ssidLength)
{
    std::vector<uint8_t> frame(38 + ssidLength, 0);
    frame[0] = probe ? 0x50 : 0x80;
    frame[16] = 2; frame[21] = 9;
    frame[32] = 100; frame[34] = 0x11;
    frame[37] = ssidLength;
    for (size_t i = 0; i < ssidLength; ++i) frame[38 + i] = static_cast<uint8_t>(i * 17);
    return frame;
}

static void pipeline()
{
    for (size_t phySize : {68, 72, 88, 4096})
        for (unsigned channel : {1, 14, 36, 144, 165})
            for (bool probe : {false, true})
                for (unsigned ssidSize : {0, 1, 32}) {
                    std::vector<uint8_t> rawPhy(phySize + 1, 0);
                    auto *phyBytes = rawPhy.data() + 1;
                    phyBytes[22] = channel; phyBytes[28] = 67; phyBytes[29] = 42;
                    ItlScan::LegacyPhy phy;
                    assert(ItlScan::decodeLegacyPhy(phyBytes, phySize, phy) == ItlScan::PhyDecode::Accepted);
                    assert(phy.channel == channel && phy.rssi == -42);
                    auto frame = beacon(probe, ssidSize);
                    ItlScan::BeaconView view;
                    assert(ItlScan::decodeScanBeacon(frame.data(), frame.size(), view));
                    assert(view.ssidLength == ssidSize && view.probeResponse == probe);
                    std::vector<uint8_t> output(SequoiaWCL::kMaxScanBeaconMessage + 2, 0xa5);
                    const auto size = SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data() + 1, output.size() - 2);
                    assert(size == sizeof(SequoiaWCL::BeaconMetadata) + frame.size() - 36 &&
                           output.front() == 0xa5 && output.back() == 0xa5);
                    const auto *record = output.data() + 1;
                    SequoiaWCL::BeaconMetadata metadata;
                    memcpy(&metadata, record, sizeof(metadata));
                    assert(metadata.ieLength == frame.size() - 36 && metadata.ssidLength == ssidSize);
                    assert(metadata.rssi == -42 && metadata.capability == 0x11 &&
                           metadata.beaconInterval == 100 && metadata.channelSpec ==
                           (channel <= 14 ? static_cast<uint16_t>(0x1000 | channel) :
                                            static_cast<uint16_t>(0xd000 | channel)) &&
                           metadata.flags == 0x4002);
                    assert(!memcmp(metadata.ssid, frame.data() + 38, ssidSize));
                    assert(!memcmp(metadata.bssid, frame.data() + 16, 6));
                    assert(!memcmp(record + sizeof(metadata), frame.data() + 36, frame.size() - 36));

                    ItlScan::IwmPassiveTrial trial;
                    const uint16_t channels[] = {static_cast<uint16_t>(channel)};
                    assert(trial.begin(channels, 1));
                    trial.rxPhy(phyBytes, phySize);
                    std::vector<uint8_t> mpdu(frame.size() + 8, 0);
                    mpdu[0] = frame.size(); mpdu[1] = frame.size() >> 8;
                    memcpy(mpdu.data() + 4, frame.data(), frame.size()); mpdu[4 + frame.size()] = 3;
                    trial.rxMpdu(mpdu.data(), mpdu.size());
                    assert(trial.evidence.phySeen == 1 && trial.evidence.phyLastBytes == phySize && trial.evidence.phy == 1);
                    assert(!trial.evidence.malformed && trial.evidence.beacons == !probe && trial.evidence.probeResponses == probe);
                    trial.hardwareStopped();
                    trial.rxPhy(phyBytes, phySize); trial.rxMpdu(mpdu.data(), mpdu.size());
                    assert(trial.evidence.phySeen == 1 && trial.evidence.mpdu == 1);
                }
}

static void rejection()
{
    uint8_t raw[88] = {}; raw[22] = 1;
    ItlScan::LegacyPhy phy; phy.channel = 36; phy.rssi = -20;
    for (size_t size = 0; size < 68; ++size) {
        assert(ItlScan::decodeLegacyPhy(raw, size, phy) == ItlScan::PhyDecode::BadSize);
        assert(phy.channel == 36 && phy.rssi == -20);
    }
    assert(ItlScan::decodeLegacyPhy(nullptr, 68, phy) == ItlScan::PhyDecode::BadSize);
    assert(ItlScan::decodeLegacyPhy(raw, 4097, phy) == ItlScan::PhyDecode::BadSize);
    raw[1] = 21;
    assert(ItlScan::decodeLegacyPhy(raw, 88, phy) == ItlScan::PhyDecode::BadConfig);
    raw[1] = 20; raw[22] = 197;
    assert(ItlScan::decodeLegacyPhy(raw, 88, phy) == ItlScan::PhyDecode::BadChannel);
    raw[22] = 1;
    assert(ItlScan::decodeLegacyPhy(raw, 88, phy) == ItlScan::PhyDecode::Accepted && phy.rssi == -256);
    auto good = beacon(false, 1);
    std::vector<uint8_t> output(SequoiaWCL::kMaxScanBeaconMessage, 0xa5);
    const auto untouched = output;
    assert(!SequoiaWCL::encodeIwmBeacon(phy, good.data(), good.size(), output.data(), output.size()));
    phy.rssi = -50;
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto bad = good;
        if (fault == 0) bad[0] = 0x08;
        if (fault == 1) bad[1] = 0x40;
        if (fault == 2) bad[22] = 1;
        if (fault == 3) bad[16] = 1;
        if (fault == 4) memset(bad.data() + 16, 0, 6);
        if (fault == 5) bad[37] = 33;
        if (fault == 6) bad.resize(36);
        if (fault == 7) { bad.push_back(0); bad.push_back(0); }
        if (fault == 8) { bad.push_back(255); bad.push_back(0); }
        if (fault == 9) bad.push_back(1);
        assert(!SequoiaWCL::encodeIwmBeacon(phy, bad.data(), bad.size(), output.data(), output.size()));
        assert(output == untouched);
    }
    for (size_t capacity = 0; capacity < sizeof(SequoiaWCL::BeaconMetadata) + 3; ++capacity) {
        assert(!SequoiaWCL::encodeIwmBeacon(phy, good.data(), good.size(), output.data(), capacity));
        assert(output == untouched);
    }
    ItlScan::IwmPassiveTrial trial;
    const uint16_t channels[] = {1}; assert(trial.begin(channels, 1));
    trial.rxPhy(raw, 67);
    raw[1] = 21; trial.rxPhy(raw, 88);
    raw[1] = 0; raw[22] = 36; trial.rxPhy(raw, 88);
    assert(trial.evidence.phyBadSize == 1 && trial.evidence.phyBadConfig == 1 && trial.evidence.phyBadChannel == 1);
    assert(trial.evidence.phySeen == 3 && trial.evidence.malformed == 3);
}

static void randomized()
{
    uint32_t seed = 0x9ad07831;
    for (unsigned run = 0; run < 20000; ++run) {
        seed = seed * 1664525u + 1013904223u;
        std::vector<uint8_t> frame(seed % 2200);
        for (auto &byte : frame) { seed = seed * 1664525u + 1013904223u; byte = seed >> 24; }
        ItlScan::BeaconView result;
        std::vector<uint8_t> output(SequoiaWCL::kMaxScanBeaconMessage, 0xa5);
        const bool valid = ItlScan::decodeScanBeacon(frame.data(), frame.size(), result);
        ItlScan::LegacyPhy phy; phy.channel = 36; phy.rssi = -42;
        const auto size = SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data(), output.size());
        assert(bool(size) == valid);
        if (!valid) for (auto byte : output) assert(byte == 0xa5);
    }
}

static void ordering()
{
    const auto frame = beacon(false, 1);
    std::vector<uint8_t> mpdu(frame.size() + 8, 0);
    mpdu[0] = frame.size();
    memcpy(mpdu.data() + 4, frame.data(), frame.size());
    mpdu[4 + frame.size()] = 3;
    uint8_t phy[88] = {}; phy[22] = 1;
    const uint16_t channels[] = {1};
    for (unsigned fault = 0; fault < 5; ++fault) {
        ItlScan::IwmPassiveTrial trial;
        const auto token = trial.begin(channels, 1);
        assert(token && trial.submitted(token, ItlScan::Submission::Accepted));
        trial.rxMpdu(mpdu.data(), mpdu.size());
        assert(trial.evidence.missingPhy == 1 && !trial.evidence.beacons);
        trial.rxPhy(phy, sizeof(phy));
        trial.rxMpdu(mpdu.data(), mpdu.size());
        assert(trial.evidence.beacons == 1);
        uint8_t bad[88] = {}; bad[22] = 1;
        if (fault == 0) trial.rxPhy(nullptr, 88);
        if (fault == 1) trial.rxPhy(bad, 67);
        if (fault == 2) { bad[1] = 21; trial.rxPhy(bad, 88); }
        if (fault == 3) { bad[22] = 36; trial.rxPhy(bad, 88); }
        if (fault == 4) { bad[22] = 144; trial.rxPhy(bad, 88); }
        trial.rxMpdu(mpdu.data(), mpdu.size());
        assert(trial.evidence.missingPhy == 2 && trial.evidence.beacons == 1);
        trial.rxPhy(phy, sizeof(phy));
        mpdu[4 + frame.size()] = 0;
        trial.rxMpdu(mpdu.data(), mpdu.size());
        assert(trial.evidence.badCrc == 1 && trial.evidence.beacons == 1);
        mpdu[4 + frame.size()] = 3;
        trial.rxMpdu(mpdu.data(), mpdu.size());
        assert(trial.evidence.beacons == 2);
        assert(trial.timeout(token));
        trial.hardwareStopped();
        trial.rxMpdu(mpdu.data(), mpdu.size());
        trial.rxPhy(phy, sizeof(phy));
        assert(trial.evidence.beacons == 2 && trial.evidence.phySeen == 3);
        assert(!trial.begin(channels, 1));
    }
}

static void mutatedFrames()
{
    uint32_t seed = 0x18095173;
    unsigned accepted = 0, rejected = 0;
    ItlScan::LegacyPhy phy; phy.channel = 36; phy.rssi = -42;
    for (unsigned run = 0; run < 20000; ++run) {
        auto frame = beacon(run & 1, run % 33);
        seed = seed * 1664525u + 1013904223u;
        if (run % 3 == 0) frame.resize(seed % (frame.size() + 1));
        else frame[seed % frame.size()] ^= (seed >> 16) | 1;
        ItlScan::BeaconView view;
        const bool valid = ItlScan::decodeScanBeacon(frame.data(), frame.size(), view);
        std::vector<uint8_t> output(SequoiaWCL::kMaxScanBeaconMessage, 0xa5);
        const auto size = SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data(), output.size());
        assert(bool(size) == valid);
        if (valid) {
            ++accepted;
            assert(size == sizeof(SequoiaWCL::BeaconMetadata) + frame.size() - 36);
            assert(!memcmp(output.data() + sizeof(SequoiaWCL::BeaconMetadata), view.ies, view.ieLength));
        } else {
            ++rejected;
            for (auto byte : output) assert(byte == 0xa5);
        }
    }
    assert(accepted > 1000 && rejected > 1000);
}

static void maximumInformationElements()
{
    auto frame = beacon(false, 0);
    while (frame.size() < 36 + SequoiaWCL::kMaxInformationElements) {
        const size_t remaining = 36 + SequoiaWCL::kMaxInformationElements - frame.size();
        assert(remaining >= 2);
        const size_t length = remaining > 257 ? 255 : remaining - 2;
        frame.push_back(221); frame.push_back(length);
        frame.insert(frame.end(), length, 0xa5);
    }
    ItlScan::LegacyPhy phy; phy.channel = 36; phy.rssi = -42;
    std::vector<uint8_t> output(SequoiaWCL::kMaxScanBeaconMessage + 2, 0x3c);
    const auto untouched = output;
    assert(!SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data() + 1,
                                       SequoiaWCL::kMaxScanBeaconMessage - 1));
    assert(output == untouched);
    assert(SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data() + 1,
                                      SequoiaWCL::kMaxScanBeaconMessage) == SequoiaWCL::kMaxScanBeaconMessage);
    assert(output.front() == 0x3c && output.back() == 0x3c);
    assert(!memcmp(output.data() + 1 + sizeof(SequoiaWCL::BeaconMetadata),
                   frame.data() + 36, SequoiaWCL::kMaxInformationElements));
    output = untouched;
    frame.push_back(1); frame.push_back(0);
    assert(!SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data() + 1, output.size() - 2));
    assert(output == untouched);
    frame.resize(frame.size() - 3);
    assert(!SequoiaWCL::encodeIwmBeacon(phy, frame.data(), frame.size(), output.data() + 1, output.size() - 2));
    assert(output == untouched);
}

int main()
{
    pipeline(); rejection(); ordering(); randomized(); mutatedFrames(); maximumInformationElements();
    puts("PHY -> Beacon/Probe -> WCL: 120 combinations, 5 RX order cases, bounds checks and 40,000 randomized/mutated frames passed");
}
