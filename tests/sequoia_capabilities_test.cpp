#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <Airport/SequoiaWCL.h>
#include "sequoia_capabilities_types.inc"
#define ITLWM_VERSION "test"
#define GIT_COMMIT "-fixture"

using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady,
       kIOReturnUnsupported, kIOReturnNoSpace, kIOReturnError };
struct OSObject {};
static const char *testCountryOverride = nullptr;
bool PE_parse_boot_argn(const char *name, void *value, int length) {
    if (!name || strcmp(name, "itlwm_cc") != 0 || !testCountryOverride || length <= 0) return false;
    size_t count = strlen(testCountryOverride);
    if (count >= static_cast<size_t>(length)) count = static_cast<size_t>(length - 1);
    memcpy(value, testCountryOverride, count);
    static_cast<char *>(value)[count] = '\0';
    return true;
}
struct Loop {
    std::recursive_mutex mutex;
    std::thread::id thread;
    bool inGate() { return thread == std::this_thread::get_id(); }
    IOReturn runAction(IOReturn (*action)(OSObject *, void *, void *, void *, void *), OSObject *owner, void *data) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        const auto previous = thread;
        thread = std::this_thread::get_id();
        const auto result = action(owner, data, nullptr, nullptr, nullptr);
        thread = previous; return result;
    }
};
static void IOLockLock(std::mutex *lock) { lock->lock(); }
static void IOLockUnlock(std::mutex *lock) { lock->unlock(); }
struct MockChannel { unsigned band = 0, ic_freq = 0, ic_flags = 0; };
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define isset(bits, i) (((bits)[(i) / 8] & (1u << ((i) % 8))) != 0)
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->band == 2)
#define IEEE80211_IS_CHAN_5GHZ(c) ((c)->band == 5)
struct Com {
    uint32_t ic_caps = 0, ic_htcaps = 0;
    uint8_t ic_chan_avail[(IEEE80211_CHAN_MAX + 8) / 8] = {};
    MockChannel ic_channels[IEEE80211_CHAN_MAX + 1];
};
struct HAL {
    Com ic, *controller = &ic;
    struct Info {
        const char *firmware = "36.77", *country = "ZZ";
        const char *getFirmwareVersion() { return firmware; }
        const char *getFirmwareCountryCode() { return country; }
    } info, *driverInfo = &info;
    Com *get80211Controller() { return controller; }
    Info *getDriverInfo() { return driverInfo; }
};
struct AirportItlwm : OSObject {
    Loop loop, *_fWorkloop = &loop;
    std::mutex mutex, *nativePMLock = &mutex;
    bool nativeStopping = false, nativeHalAttached = true;
    bool nativeWclCompatR162 = false;
    HAL hal, *fHalService = &hal;
    char geo_location_cc[3] = {};
    IOReturn getCARD_CAPABILITIES(OSObject *, apple80211_capability_data *);
    bool nativeHardwareQueryAvailable();
    IOReturn getNativeVersion(apple80211_version_data *, bool);
    IOReturn getNativeCountry(apple80211_country_code_data *);
};
#include "sequoia_capabilities_method.inc"
struct AirportItlwmSkywalkInterface {
    AirportItlwm *instance;
    HAL *fHalService;
    IOReturn getSequoiaSupportedChannels(void *, bool);
};
#include "sequoia_channels_method.inc"
static void channel(Com &ic, unsigned index, unsigned band) {
    ic.ic_chan_avail[index / 8] |= 1u << (index % 8);
    ic.ic_channels[index].band = band;
    ic.ic_channels[index].ic_freq = band == 2 ? 2412 : 5180;
}
int main() {
    unsigned cases = 0;
    // Actual interface query -> actual current-country getter -> production
    // encoder. Only the workloop, boot args and HAL inventory are fixtures.
    for (unsigned fault = 0; fault < 9; ++fault) {
        AirportItlwm d;
        d.hal.info.country = "US";
        channel(d.hal.ic, 1, 2);
        channel(d.hal.ic, 36, 5);
        channel(d.hal.ic, 52, 5);
        d.hal.ic.ic_channels[52].ic_flags = IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_DFS;
        AirportItlwmSkywalkInterface iface{&d, &d.hal};
        SequoiaWCL::SupportedChannels query = {};
        query.version = 1; query.includeAnyFlags = UINT32_MAX;
        query.countryOverride = 1; memcpy(query.countryAndReserved, "US", 3);
        if (fault == 1) d.nativeStopping = true;
        if (fault == 2) d.hal.info.country = nullptr;
        if (fault == 3) d._fWorkloop = nullptr;
        if (fault == 4) iface.instance = nullptr;
        if (fault == 5) d.hal.controller = nullptr;
        if (fault == 6) iface.fHalService = nullptr;
        if (fault == 7) query.countryAndReserved[1] = 'K';
        if (fault == 8) d.nativeHalAttached = false;
        const auto before = query;
        assert(iface.getSequoiaSupportedChannels(nullptr, false) == kIOReturnBadArgument);
        auto result = iface.getSequoiaSupportedChannels(&query, false);
        assert(result == (fault == 7 ? kIOReturnUnsupported : fault ? kIOReturnNotReady : kIOReturnSuccess));
        if (fault) assert(!memcmp(&query, &before, sizeof(query)));
        else {
            assert(query.count == 3 && query.channels[2].number == 52);
            assert(query.channels[2].flags == (SequoiaWCL::Width20MHz | SequoiaWCL::Band5GHz | SequoiaWCL::DFS));
            // Exercise reentrant query from the same gate, as in WCL dispatch.
            d.loop.thread = std::this_thread::get_id();
            assert(iface.getSequoiaSupportedChannels(&query, false) == kIOReturnSuccess);
            assert(d.loop.inGate()); d.loop.thread = {};
        }
        ++cases;
    }
    // 24H23 Apple80211Scan +0x162 selects the list-request layout using
    // capabilities[9] bit 1. +0x417/+0x44c then select ioctl 10/86.
    // Both stock external handler tables lack 86. The original framework
    // selection block is also executed by reverse_apple80211_scan_routing.py.
    // This consumer model checks the production capability reply, not radio I/O.
    {
        AirportItlwm d;
        apple80211_capability_data reply = {};
        assert(d.getCARD_CAPABILITIES(nullptr, &reply) == kIOReturnSuccess);
        auto opcode = [](const apple80211_capability_data &caps, bool ssids, bool bssids) {
            return (ssids || bssids) && !(caps.capabilities[9] & 2) ? 86 : 10;
        };
        for (unsigned lists = 0; lists < 4; ++lists) {
            assert(opcode(reply, lists & 1, lists & 2) == 10);
            ++cases;
        }
        reply.capabilities[9] &= ~2;
        assert(opcode(reply, false, false) == 10);
        assert(opcode(reply, true, false) == 86);
        assert(opcode(reply, false, true) == 86);
    }
    for (unsigned mask = 0; mask < 128; ++mask) {
        for (unsigned alignment = 0; alignment < 8; ++alignment) {
            AirportItlwm d;
            auto &ic = d.hal.ic;
            if (mask & 1) ic.ic_caps |= IEEE80211_C_RSN;
            if (mask & 2) ic.ic_caps |= IEEE80211_C_SHSLOT;
            if (mask & 4) ic.ic_caps |= IEEE80211_C_SHPREAMBLE;
            if (mask & 8) ic.ic_caps |= IEEE80211_C_QOS;
            if (mask & 16) ic.ic_htcaps |= IEEE80211_HTCAP_SGI20;
            if (mask & 32) channel(ic, 1, 2);
            if (mask & 64) channel(ic, 36, 5);
            // Unsupported chipset features cannot leak into native claims.
            ic.ic_caps |= IEEE80211_C_WEP | IEEE80211_C_PMGT;
            std::vector<uint8_t> bytes(sizeof(apple80211_capability_data) + 16, 0xa5);
            auto *output = reinterpret_cast<apple80211_capability_data *>(bytes.data() + alignment);
            assert(d.getCARD_CAPABILITIES(nullptr, output) == 0);
            apple80211_capability_data result;
            memcpy(&result, output, sizeof(result));
            assert(result.version == APPLE80211_VERSION);
            assert(result.capabilities[0] == ((mask & 1) ? 8 : 0));
            assert(result.capabilities[1] == (((mask & 1) ? 0x40 : 0) | ((mask & 2) ? 2 : 0) | ((mask & 4) ? 4 : 0)));
            assert(result.capabilities[2] == (0x40 | ((mask & 8) ? 2 : 0) | ((mask & 16) ? 8 : 0)));
            assert(result.capabilities[3] == ((mask & 32) && (mask & 64) ? 0x20 : 0));
            for (unsigned i = 4; i < sizeof(result.capabilities); ++i)
                assert(result.capabilities[i] == (i == 5 ? 0x40 : i == 9 ? 2 : i == 10 ? 0x10 : 0));
            for (unsigned i = 0; i < alignment; ++i) assert(bytes[i] == 0xa5);
            for (unsigned i = alignment + sizeof(result); i < bytes.size(); ++i) assert(bytes[i] == 0xa5);
            ++cases;
        }
    }
    // The legacy-cache compatibility experiment must not disable the
    // independent userspace scan-completion wait capability.
    {
        AirportItlwm d;
        d.nativeWclCompatR162 = true;
        apple80211_capability_data reply = {};
        assert(d.getCARD_CAPABILITIES(nullptr, &reply) == kIOReturnSuccess);
        assert(reply.capabilities[5] & 0x40);
        assert(reply.capabilities[2] & 0x40);
        assert(reply.capabilities[9] & 2);
        assert(reply.capabilities[10] == 0);
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        AirportItlwm d; apple80211_capability_data output;
        memset(&output, 0xa5, sizeof(output));
        if (fault == 1) d._fWorkloop = nullptr;
        if (fault == 2) d.nativePMLock = nullptr;
        if (fault == 3) d.nativeStopping = true;
        if (fault == 4) d.nativeHalAttached = false;
        if (fault == 5) d.fHalService = nullptr;
        if (fault == 6) d.hal.controller = nullptr;
        if (fault == 7) d.hal.driverInfo = nullptr;
        assert(d.getCARD_CAPABILITIES(nullptr, nullptr) == kIOReturnBadArgument);
        assert(d.getCARD_CAPABILITIES(nullptr, &output) == (fault ? kIOReturnNotReady : kIOReturnSuccess));
        if (fault) for (auto byte : output.capabilities) assert(byte == 0xa5);
        apple80211_version_data version;
        apple80211_country_code_data country;
        assert(d.getNativeVersion(nullptr, true) == kIOReturnBadArgument);
        assert(d.getNativeCountry(nullptr) == kIOReturnBadArgument);
        assert(d.getNativeVersion(&version, true) == (fault ? kIOReturnNotReady : 0));
        assert(d.getNativeCountry(&country) == (fault ? kIOReturnNotReady : 0));
        ++cases;
    }
    for (unsigned length = 0; length < 400; ++length) {
        for (unsigned alignment = 0; alignment < 8; ++alignment) {
            AirportItlwm d;
            std::vector<char> firmware(length + 1, 'a'); firmware.back() = 0;
            d.hal.info.firmware = firmware.data();
            for (bool driver : {false, true}) {
                std::vector<uint8_t> storage(sizeof(apple80211_version_data) + 16, 0xa5);
                auto *out = reinterpret_cast<apple80211_version_data *>(storage.data() + alignment);
                assert(d.getNativeVersion(out, driver) == 0);
                apple80211_version_data value;
                memcpy(&value, out, sizeof(value));
                assert(value.version == APPLE80211_VERSION && value.string_len == strlen(value.string));
                assert(value.string_len < sizeof(value.string));
                if (!driver) assert(value.string_len == std::min(size_t(length), sizeof(value.string) - 1));
                for (unsigned i = 0; i < alignment; ++i) assert(storage[i] == 0xa5);
                for (unsigned i = alignment + sizeof(value); i < storage.size(); ++i) assert(storage[i] == 0xa5);
                ++cases;
            }
            char twoBytes[] = {'C', 'N'};
            d.hal.info.country = twoBytes;
            std::vector<uint8_t> storage(sizeof(apple80211_country_code_data) + 16, 0xa5);
            auto *out = reinterpret_cast<apple80211_country_code_data *>(storage.data() + alignment);
            assert(d.getNativeCountry(out) == 0);
            apple80211_country_code_data country;
            memcpy(&country, out, sizeof(country));
            assert(country.version == APPLE80211_VERSION && !memcmp(country.cc, "CN", 2));
            for (unsigned i = 2; i < sizeof(country.cc); ++i) assert(!country.cc[i]);
            d.hal.info.country = twoBytes;
            testCountryOverride = "US";
            memset(out, 0xa5, sizeof(*out));
            assert(d.getNativeCountry(out) == 0);
            memcpy(&country, out, sizeof(country));
            assert(!memcmp(country.cc, "US", 2));
            testCountryOverride = "cn";
            memset(out, 0xa5, sizeof(*out));
            assert(d.getNativeCountry(out) == 0);
            memcpy(&country, out, sizeof(country));
            assert(!memcmp(country.cc, "CN", 2));
            testCountryOverride = "C";
            memset(out, 0xa5, sizeof(*out));
            assert(d.getNativeCountry(out) == 0);
            memcpy(&country, out, sizeof(country));
            assert(!memcmp(country.cc, "CN", 2));
            d.hal.info.country = twoBytes;
            testCountryOverride = nullptr;
            d.hal.info.country = nullptr;
            assert(d.getNativeCountry(out) == kIOReturnNotReady);
            d.hal.info.firmware = nullptr;
            apple80211_version_data version;
            assert(d.getNativeVersion(&version, true) == kIOReturnNotReady);
            ++cases;
        }
    }
    {
        AirportItlwm d;
        char firmwareZZ[] = {'Z', 'Z', '\0'};
        d.hal.info.country = firmwareZZ;
        apple80211_country_code_data country;
        testCountryOverride = nullptr;
        assert(d.getNativeCountry(&country) == 0);
        assert(!memcmp(country.cc, "ZZ", 2));
        char geo[] = {'C', 'N', '\0'};
        memcpy(d.geo_location_cc, geo, sizeof(geo));
        assert(d.getNativeCountry(&country) == 0);
        assert(!memcmp(country.cc, "CN", 2));
        testCountryOverride = "JP";
        assert(d.getNativeCountry(&country) == 0);
        assert(!memcmp(country.cc, "JP", 2));
        testCountryOverride = nullptr;
    }
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        AirportItlwm d;
        std::thread reader([&] {
            apple80211_capability_data output;
            for (unsigned i = 0; i < 100; ++i) {
                auto result = d.getCARD_CAPABILITIES(nullptr, &output);
                assert(result == kIOReturnSuccess || result == kIOReturnNotReady);
                apple80211_version_data version;
                result = d.getNativeVersion(&version, true);
                assert(result == kIOReturnSuccess || result == kIOReturnNotReady);
                apple80211_country_code_data country;
                result = d.getNativeCountry(&country);
                assert(result == kIOReturnSuccess || result == kIOReturnNotReady);
            }
        });
        { std::lock_guard<std::mutex> lock(d.mutex); d.nativeStopping = true; }
        { std::lock_guard<std::recursive_mutex> drain(d.loop.mutex); }
        d.nativeHalAttached = false; d.fHalService = nullptr;
        reader.join(); ++cases;
    }
    printf("Native capabilities: %u exact bitmap/unaligned/startup/rejection/shutdown traces passed; HAL and workloop mocked\n", cases);
}
