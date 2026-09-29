#include <Airport/SequoiaWCL.h>
#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace SequoiaWCL;

static const uint8_t kSSID[] = {'a', 0, 'b'};
static const uint8_t kBSSID[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kIEs[] = {0, 3, 'a', 0, 'b', 1, 1, 0x82};

static BeaconInput input()
{
    return {kSSID, sizeof(kSSID), kBSSID, kIEs, sizeof(kIEs),
            6, Band::GHz2, -42, 0x431, 100};
}

static void expectRejected(const BeaconInput &value, size_t capacity = kMaxBeaconMessage)
{
    std::vector<uint8_t> bytes(capacity, 0xa5);
    assert(encodeBeacon(value, bytes.data(), capacity) == 0);
    for (uint8_t byte : bytes)
        assert(byte == 0xa5);
}

static void goldenLayout()
{
    uint8_t bytes[0x49];
    memset(bytes, 0xa5, sizeof(bytes));
    // Unaligned output is deliberate; the codec must not write through a struct pointer.
    assert(encodeBeacon(input(), bytes + 1, 0x48) == 0x48);
    assert(bytes[0] == 0xa5);
    const uint8_t *result = bytes + 1;
    assert(result[0] == 8 && result[1] == 0 && result[2] == 0 && result[3] == 0);
    assert(result[4] == 6 && result[5] == 0x10);
    assert(memcmp(result + 6, kSSID, sizeof(kSSID)) == 0);
    for (size_t i = 9; i < 0x26; ++i)
        assert(result[i] == 0);
    assert(result[0x26] == 3 && result[0x27] == 6 && result[0x28] == 0);
    assert(memcmp(result + 0x29, kBSSID, sizeof(kBSSID)) == 0);
    assert(result[0x2f] == 0);
    assert(result[0x30] == 0xd6 && result[0x31] == 0xff &&
           result[0x32] == 0xff && result[0x33] == 0xff);
    for (size_t i = 0x34; i < 0x38; ++i)
        assert(result[i] == 0);
    assert(result[0x38] == 100 && result[0x39] == 0);
    assert(result[0x3a] == 0x31 && result[0x3b] == 0x04);
    // Independently emulate the audited Apple consumer, not our struct.
    // A 100-TU secured AP must retain Privacy; swapping these words would
    // classify it as open and give it a nonsensical 1073-TU beacon period.
    const unsigned applePeriod = result[0x38] | (result[0x39] << 8);
    const unsigned appleCapabilities = result[0x3a] | (result[0x3b] << 8);
    assert(applePeriod == 100);
    assert((appleCapabilities & 0x10) != 0);
    // d93 supplies the inline SSID bit while bit 14 independently enables
    // RSSI/timestamp storage; bit 2 remains clear so the receiver takes the
    // audited inline setter path.
    assert(result[0x3c] == 0x02 && result[0x3d] == 0x40 && result[0x3e] == 0 && result[0x3f] == 0);
    uint32_t flags = 0;
    memcpy(&flags, result + 0x3c, sizeof(flags));
    assert((flags & kBeaconSSIDValid) == kBeaconSSIDValid);
    assert((flags & 0x4) == 0);
    assert((flags & kBeaconRSSIValid) == kBeaconRSSIValid);
    assert(memcmp(result + 0x40, kIEs, sizeof(kIEs)) == 0);
}

static void limitsAndInvalidInputs()
{
    uint8_t bytes[kMaxBeaconMessage];
    auto value = input();
    expectRejected(value, 0x47);
    assert(encodeBeacon(value, nullptr, sizeof(bytes)) == 0);
    value.ssidLength = 33;
    expectRejected(value);
    value = input();
    value.ssid = nullptr;
    expectRejected(value);
    value.ssidLength = 0;
    assert(encodeBeacon(value, bytes, sizeof(bytes)) == 0x48);
    assert(bytes[0x26] == 0);
    value = input();
    uint8_t fullSSID[32];
    memset(fullSSID, 0x7f, sizeof(fullSSID));
    value.ssid = fullSSID;
    value.ssidLength = sizeof(fullSSID);
    assert(encodeBeacon(value, bytes, sizeof(bytes)) == 0x48);
    assert(bytes[0x26] == 32 && memcmp(bytes + 6, fullSSID, sizeof(fullSSID)) == 0);

    value = input();
    value.bssid = nullptr;
    expectRejected(value);
    const uint8_t zero[6] = {};
    value.bssid = zero;
    expectRejected(value);
    const uint8_t multicast[6] = {1, 2, 3, 4, 5, 6};
    value.bssid = multicast;
    expectRejected(value);

    value = input();
    value.ies = nullptr;
    expectRejected(value);
    value.ieLength = 0;
    assert(encodeBeacon(value, bytes, sizeof(bytes)) == sizeof(BeaconMetadata));
    uint8_t maxIEs[kMaxInformationElements] = {}; // Complete zero-length IEs.
    value.ies = maxIEs;
    value.ieLength = sizeof(maxIEs);
    assert(encodeBeacon(value, bytes, sizeof(bytes)) == sizeof(BeaconMetadata) + kMaxInformationElements);
    value.ieLength = sizeof(maxIEs) + 1;
    expectRejected(value);
    value.ieLength = SIZE_MAX;
    expectRejected(value);

    const uint8_t truncated[] = {0, 5, 1};
    assert(!validInformationElements(truncated, sizeof(truncated)));
    const uint8_t trailing[] = {0, 0, 1};
    assert(!validInformationElements(trailing, sizeof(trailing)));
    const uint8_t emptyExtension[] = {255, 0};
    assert(!validInformationElements(emptyExtension, sizeof(emptyExtension)));
    const uint8_t validExtension[] = {255, 1, 35};
    assert(validInformationElements(validExtension, sizeof(validExtension)));
    assert(validInformationElements(nullptr, 0));
    assert(!validInformationElements(nullptr, 1));
    value = input();
    value.ies = truncated;
    value.ieLength = sizeof(truncated);
    expectRejected(value);
}

static void legacyChannelsInfoLayout()
{
    // The 24G830 inventory observed on the host contains 13 2.4 GHz and
    // 24 5 GHz primary channels.  Exercise the complete fixed-size mapping
    // without depending on a live IOKit object.
    const unsigned channels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
        116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165,
    };
    assert(sizeof(channels) / sizeof(channels[0]) == 37);
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i) {
        const bool band2 = channels[i] <= 14;
        LegacyChannelInfoEntry entry = {};
        assert(legacyChannelInfo(channels[i], band2 ? Band::GHz2 : Band::GHz5,
                                 channels[i] == 52, channels[i] == 52,
                                 channels[i] >= 36, channels[i] >= 36, entry));
        assert(entry.channel == channels[i]);
        assert(entry.channelSpec == channelSpec20MHz(
            channels[i], band2 ? Band::GHz2 : Band::GHz5));
        assert(entry.indoorRestriction == 0);
        assert(entry.radarDFS == (channels[i] == 52));
        assert(entry.passive == (channels[i] == 52));
        assert(entry.support40MHz == (channels[i] >= 36));
        assert(entry.support80MHz == (channels[i] >= 36));
    }
    LegacyChannelInfoEntry unchanged = {0x55aa, 1, 2, 3, 4, 5, 6};
    assert(!legacyChannelInfo(0, Band::GHz2, false, false, false, false, unchanged));
    assert(unchanged.channelSpec == 0x55aa && unchanged.channel == 1);
    assert(!legacyChannelInfo(36, Band::GHz2, false, false, false, false, unchanged));
}

static void informationElementShape()
{
    const uint8_t ies[] = {
        0, 3, 'a', 0, 'b',
        3, 1, 36,
        6, 6, 0, 1, 2, 3, 4, 5,
        11, 5, 1, 2, 3, 4, 5,
        191, 12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        255, 1, 35
    };
    const auto shape = inspectInformationElements(ies, sizeof(ies));
    assert(shape.total == 6 && shape.firstId == 0);
    assert(shape.ssid == 1 && shape.ssidLength == 3);
    assert(shape.ds == 1 && shape.dsLength == 1 && shape.dsBadLength == 0);
    assert(shape.ibss == 1 && shape.ibssLength == 6 && shape.ibssBadLength == 0);
    assert(shape.qbss == 1 && shape.qbssLength == 5 && shape.qbssBadLength == 0);
    assert(shape.vht == 1 && shape.vhtLength == 12 && shape.vhtBadLength == 0);
    const uint8_t bad[] = {3, 2, 1, 2, 6, 1, 0, 191, 1, 0};
    const auto badShape = inspectInformationElements(bad, sizeof(bad));
    assert(badShape.ds == 1 && badShape.dsBadLength == 1);
    assert(badShape.ibss == 1 && badShape.ibssBadLength == 1);
    assert(badShape.vht == 1 && badShape.vhtBadLength == 1);
    assert(!badShape.malformed);
    const uint8_t truncated[] = {0, 4, 1};
    assert(inspectInformationElements(truncated, sizeof(truncated)).malformed == 1);
    assert(inspectInformationElements(nullptr, 1).malformed == 1);
}

static void wlBssInputLayout()
{
    std::vector<uint8_t> bytes(kMaxWLBSSMessage + 2, 0xa5);
    const auto written = encodeWLBSSInfo(input(), bytes.data() + 1, bytes.size() - 2);
    assert(written == kWLBSSFixedSize + sizeof(kIEs));
    const auto *record = bytes.data() + 1;
    uint32_t length = 0, ieOffset = 0, ieLength = 0;
    memcpy(&length, record + 4, sizeof(length));
    memcpy(&ieOffset, record + 0x74, sizeof(ieOffset));
    memcpy(&ieLength, record + 0x78, sizeof(ieLength));
    assert(length == written && ieOffset == kWLBSSFixedSize && ieLength == sizeof(kIEs));
    assert(record[0] == 109 && record[0x12] == sizeof(kSSID));
    assert(!memcmp(record + 0x08, kBSSID, sizeof(kBSSID)));
    assert(!memcmp(record + 0x13, kSSID, sizeof(kSSID)));
    assert(!memcmp(record + kWLBSSFixedSize, kIEs, sizeof(kIEs)));
    const auto untouched = bytes;
    assert(!encodeWLBSSInfo(input(), bytes.data() + 1, kWLBSSFixedSize + sizeof(kIEs) - 1));
    assert(bytes == untouched);
}

static void channels()
{
    assert(channelSpec20MHz(1, Band::GHz2) == 0x1001);
    assert(channelSpec20MHz(14, Band::GHz2) == 0x100e);
    assert(channelSpec20MHz(36, Band::GHz5) == 0xd024);
    assert(channelSpec20MHz(165, Band::GHz5) == 0xd0a5);
    assert(!channelSpec20MHz(0, Band::GHz2));
    assert(!channelSpec20MHz(36, Band::GHz2));
    assert(!channelSpec20MHz(6, Band::GHz5));
    assert(!channelSpec20MHz(256, Band::GHz5));
    assert(!channelSpec20MHz(36, static_cast<Band>(99)));
}

static void supportedChannels()
{
    Channel inventory[4];
    assert(channel20MHz(1, Band::GHz2, false, false, inventory[0]));
    assert(channel20MHz(14, Band::GHz2, true, false, inventory[1]));
    assert(channel20MHz(36, Band::GHz5, false, false, inventory[2]));
    assert(channel20MHz(52, Band::GHz5, true, true, inventory[3]));
    assert(inventory[0].flags == 0x8a && inventory[1].flags == 0x0a);
    assert(inventory[2].flags == 0x92 && inventory[3].flags == 0x112);
    Channel saved = inventory[0];
    assert(!channel20MHz(36, Band::GHz2, false, false, inventory[0]));
    assert(memcmp(&saved, &inventory[0], sizeof(saved)) == 0);

    SupportedChannels request = {};
    memset(request.countryAndReserved, 0x7b, sizeof(request.countryAndReserved));
    request.count = UINT32_MAX; // This is an output count, not caller capacity.
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Success);
    assert(request.version == 1 && request.count == 4 && request.includeAnyFlags == UINT32_MAX);
    assert(memcmp(request.channels, inventory, sizeof(inventory)) == 0);
    for (uint8_t byte : request.countryAndReserved)
        assert(byte == 0x7b);
    for (size_t i = 4; i < kMaxSupportedChannels; ++i)
        assert(request.channels[i].version == 0 && request.channels[i].number == 0 && request.channels[i].flags == 0);

    request.includeAnyFlags = Band5GHz;
    request.excludeAnyFlags = DFS;
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Success);
    assert(request.count == 1 && request.channels[0].number == 36);
    request.includeAnyFlags = ActiveScan;
    request.excludeAnyFlags = 0;
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Success);
    assert(request.count == 2 && request.channels[0].number == 1 && request.channels[1].number == 36);
    request.includeAnyFlags = Band2GHz | DFS; // Any included bit, not all bits.
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Success);
    assert(request.count == 3 && request.channels[2].number == 52);
    request.includeAnyFlags = 0;
    request.excludeAnyFlags = DFS; // Stock defaults to all only if BOTH masks are zero.
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Success);
    assert(request.count == 0 && request.includeAnyFlags == 0);
    request.includeAnyFlags = 0x800; // Bonded 160 MHz entries are not advertised yet.
    request.excludeAnyFlags = 0;
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Success);
    assert(request.count == 0);

    request.countryOverride = 1;
    SupportedChannels unchanged = request;
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, false) == ChannelQueryStatus::Unsupported);
    assert(memcmp(&request, &unchanged, sizeof(request)) == 0);
    // 24H23 CWFApple80211 forwards its current country in ioctl 27. It is
    // a query, not a request to change the NVM regulatory domain.
    const uint8_t currentCountry[3] = {'U', 'S', 0};
    for (unsigned alignment = 0; alignment < 8; ++alignment) {
        for (unsigned scenario = 0; scenario < 10; ++scenario) {
            SupportedChannels query = {};
            query.version = 1;
            query.includeAnyFlags = UINT32_MAX;
            query.countryOverride = 1;
            memcpy(query.countryAndReserved, currentCountry, 3);
            memset(query.countryAndReserved + 3, 0x7b, 3);
            uint8_t reported[3];
            memcpy(reported, currentCountry, 3);
            if (scenario == 1) query.countryAndReserved[1] = 'K';
            if (scenario == 2) query.countryAndReserved[2] = 'X';
            if (scenario == 3) query.countryOverride = 2;
            if (scenario == 4) reported[0] = 0;
            if (scenario == 5) reported[1] = 's';
            if (scenario == 6) reported[2] = 'X';
            if (scenario == 7) query.excludeAnyFlags = DFS;
            if (scenario == 8) query.includeAnyFlags = Band2GHz;
            std::vector<uint8_t> storage(sizeof(query) + 16, 0xa5);
            memcpy(storage.data() + alignment, &query, sizeof(query));
            const auto before = storage;
            const bool success = scenario == 0 || scenario == 7 || scenario == 8;
            assert(encodeSupportedChannels(storage.data() + alignment, sizeof(query), inventory, 4,
                       false, scenario == 9 ? nullptr : reported) ==
                   (success ? ChannelQueryStatus::Success : ChannelQueryStatus::Unsupported));
            if (!success) { assert(storage == before); continue; }
            SupportedChannels result;
            memcpy(&result, storage.data() + alignment, sizeof(result));
            assert(result.count == (scenario == 7 ? 3 : scenario == 8 ? 2 : 4));
            assert(result.countryOverride == 1);
            assert(!memcmp(result.countryAndReserved, query.countryAndReserved, 6));
            for (unsigned i = 0; i < alignment; ++i) assert(storage[i] == 0xa5);
            for (unsigned i = alignment + sizeof(query); i < storage.size(); ++i) assert(storage[i] == 0xa5);
            // Passive and DFS flags come from the original NVM inventory.
            for (unsigned i = 0; i < result.count; ++i) {
                const Channel *original = nullptr;
                for (const auto &entry : inventory)
                    if (entry.number == result.channels[i].number) original = &entry;
                assert(original && !memcmp(original, &result.channels[i], sizeof(Channel)));
            }
        }
    }
    request.excludeAnyFlags = DFS;
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, 4, true) == ChannelQueryStatus::Success);
    assert(request.hardwareQuery == 1 && request.countryOverride == 0);
    assert(request.includeAnyFlags == UINT32_MAX && request.excludeAnyFlags == DFS && request.count == 3);
    assert(request.channels[1].flags == 0x0a); // Hardware query does not enable passive channels.
    assert(memcmp(request.countryAndReserved, unchanged.countryAndReserved, sizeof(request.countryAndReserved)) == 0);

    unchanged = request;
    assert(encodeSupportedChannels(nullptr, sizeof(request), inventory, 4, false) == ChannelQueryStatus::BadArgument);
    assert(encodeSupportedChannels(&request, sizeof(request) - 1, inventory, 4, false) == ChannelQueryStatus::BadArgument);
    assert(encodeSupportedChannels(&request, sizeof(request), nullptr, 1, false) == ChannelQueryStatus::BadArgument);
    assert(encodeSupportedChannels(&request, sizeof(request), inventory, SIZE_MAX, false) == ChannelQueryStatus::NoSpace);
    assert(memcmp(&request, &unchanged, sizeof(request)) == 0);

    Channel maxInventory[kMaxSupportedChannels];
    for (auto &channel : maxInventory)
        channel = inventory[0];
    std::vector<uint8_t> bytes(sizeof(request) + 2, 0xa5);
    memcpy(bytes.data() + 1, &request, sizeof(request));
    assert(encodeSupportedChannels(bytes.data() + 1, sizeof(request), maxInventory,
                                   kMaxSupportedChannels, false) == ChannelQueryStatus::Success);
    assert(bytes.front() == 0xa5 && bytes.back() == 0xa5);
    memcpy(&request, bytes.data() + 1, sizeof(request));
    assert(request.count == kMaxSupportedChannels && request.channels[399].number == 1);
    assert(encodeSupportedChannels(&request, sizeof(request), nullptr, 0, false) == ChannelQueryStatus::Success);
    assert(request.count == 0 && request.channels[399].number == 0);
}

static uint32_t nextRandom(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

static void scanRequestBounds()
{
    // Write independently recovered wire offsets, not offsets computed from the
    // struct under test. Byte 0xa5 also exercises opaque fields and binary SSIDs.
    std::vector<uint8_t> bytes(0x1550 + 2, 0xa5);
    auto *request = bytes.data() + 1;
    auto put32 = [request](size_t offset, uint32_t value) {
        memcpy(request + offset, &value, sizeof(value));
    };
    put32(0x1c, 32);
    put32(0x54, 400);
    put32(0x1318, 10);
    for (size_t i = 0; i < 10; ++i)
        put32(0x1320 + i * 0x38, 32);
    const auto unchanged = bytes;
    assert(validScanRequestBounds(request, 0x1550));
    assert(bytes == unchanged);
    ScanRequest decoded;
    memcpy(&decoded, request, sizeof(decoded));
    assert(decoded.ssidLength == 32 && decoded.channelCount == 400 && decoded.ssidCount == 10);
    assert(decoded.ssids[9].length == 32 && decoded.ssids[9].bytes[31] == 0xa5);
    assert(!validScanRequestBounds(nullptr, 0x1550));
    for (size_t size = 0; size < 0x1550; ++size) {
        // Exact-sized allocation ensures ASan catches reads past the supplied span.
        std::vector<uint8_t> truncated(size);
        assert(!validScanRequestBounds(truncated.data(), size));
    }
    put32(0x54, 401);
    assert(!validScanRequestBounds(request, 0x1550));
    put32(0x54, UINT32_MAX);
    assert(!validScanRequestBounds(request, 0x1550));
    put32(0x54, 0);
    put32(0x1c, 33);
    assert(!validScanRequestBounds(request, 0x1550));
    put32(0x1c, UINT32_MAX);
    assert(!validScanRequestBounds(request, 0x1550));
    put32(0x1c, 0);
    put32(0x1318, 11);
    assert(!validScanRequestBounds(request, 0x1550));
    put32(0x1318, UINT32_MAX);
    assert(!validScanRequestBounds(request, 0x1550));
    put32(0x1318, 10);
    for (size_t i = 0; i < 10; ++i) {
        put32(0x1320 + i * 0x38, 33);
        assert(!validScanRequestBounds(request, 0x1550));
        put32(0x1320 + i * 0x38, 0);
    }
    assert(validScanRequestBounds(request, 0x1550));
    put32(0x1318, 0);
    put32(0x1320, UINT32_MAX); // Unused entries are not interpreted.
    assert(validScanRequestBounds(request, 0x1550));
    assert(bytes.front() == 0xa5 && bytes.back() == 0xa5);
}

static void randomizedBounds()
{
    uint32_t seed = 0x24'08'30;
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        auto value = input();
        size_t length = nextRandom(seed) % (kMaxInformationElements + 4);
        std::vector<uint8_t> ies(length);
        for (uint8_t &byte : ies)
            byte = nextRandom(seed) >> 24;
        if (iteration % 4 == 0) {
            length &= ~size_t(1);
            ies.assign(length, 0);
        }
        value.ies = ies.data();
        value.ieLength = length;
        size_t capacity = nextRandom(seed) % (kMaxBeaconMessage + 4);
        std::vector<uint8_t> output(capacity + 2, 0xa5);
        size_t written = encodeBeacon(value, output.data() + 1, capacity);
        assert(output.front() == 0xa5 && output.back() == 0xa5);
        if (!written) {
            for (uint8_t byte : output)
                assert(byte == 0xa5);
        } else {
            assert(written == sizeof(BeaconMetadata) + length && written <= capacity);
            assert(validInformationElements(ies.data(), length));
            assert(memcmp(output.data() + 1 + sizeof(BeaconMetadata), ies.data(), length) == 0);
        }
    }
}

int main()
{
    goldenLayout();
    limitsAndInvalidInputs();
    informationElementShape();
    wlBssInputLayout();
    channels();
    legacyChannelsInfoLayout();
    supportedChannels();
    scanRequestBounds();
    randomizedBounds();
    puts("WCL codec: beacon/channel/scan layouts, filters, boundary cases and 20000 randomized cases passed");
}
