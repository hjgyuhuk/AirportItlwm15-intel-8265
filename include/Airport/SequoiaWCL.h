#ifndef AIRPORT_ITLWM_SEQUOIA_WCL_H
#define AIRPORT_ITLWM_SEQUOIA_WCL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Experimental wire layout recovered from 24G830, not an Apple public API.
namespace SequoiaWCL {

constexpr size_t kMaxInformationElements = 0x800;
// 24G830 gSCAN_MANAGERCallBackArray routes 0xc9 to ScanResultEventHandler
// (the scan FSM) and 0xce to scanupdateHandler (unsolicited cache update).
// BOTH consume a single parsed BeaconMetadata + IEs, never wl_bss_info.
// The native Broadcom scan adapter observed on 24G830 posts parsed BSS
// records as 0xce. Use that cache-update entry point for the Intel producer;
// completion remains 0xed.
//
// That mapping is now settled against the family itself, and it is the reverse
// of what this file said. The scan manager is an FSM whose transitions are
// logged, and on the r159 boot its log shows what the driver's messages actually
// do: SCAN_MANAGER_EVENT_SCAN_COMPLETE arrives (so 0xed works) but
// SCAN_MANAGER_EVENT_SCAN_RESULT never does, while the same boot counted 2438
// results delivered here. 0xce therefore feeds the unsolicited cache-update
// entry point (scanupdateHandler) and never reaches the scan FSM as a result;
// 0xc9 is the explicit scan-result message that does. r149 is the only candidate
// that ever sent 0xc9, and it did so with flags that intentionally omitted the
// inline SSID bit, so the two were never combined: this keeps r150's flags - the
// inline-SSID branch that 24G830's setBeaconDataFromMsg follows when bit 1 is
// set - and changes only the message id.
// 0xc9 is APPLE80211_M_WCL_SCAN_RESULT, the scan FSM's explicit result event, and
// 0xce is APPLE80211_M_WCL_SCAN_UPDATE, the unsolicited cache update that
// WCLScanManager::scanupdateHandler serves. Both ids are correct - they come from the
// family's own gAppleMIndexToString table - but they are not interchangeable, and the
// r162/r163 boots show which one this driver actually needs: with 0xc9 the FSM logged
// SCAN_MANAGER_EVENT_SCAN_COMPLETE 21 times and SCAN_MANAGER_EVENT_SCAN_RESULT not
// once, no WCL result handler ever ran, and only the client that asked for a scan
// received results (ControlCenter saw 81..91 networks) while WiFiAgent's own queries
// kept coming back empty and the menu showed no list. Results that reach only the
// requester and are never published to the scan cache are what an unconsumed result
// event looks like, so the ordinary scan results now go to the cache-update entry that
// scanupdateHandler serves.
//
// This is the combination that has never been tried: 0xce was used from r149 to r159,
// before the driver announced itself, when every delivery was discarded because the
// family still reported isDriverAvailable=<0>. r162 fixed that and 0xc9 has been used
// since. The completion stays 0xed either way - the FSM consumes it as
// SCAN_MANAGER_EVENT_SCAN_COMPLETE, which is what releases the busy state.
constexpr uint32_t kBeaconMessage = 0xc9;
constexpr uint32_t kScanResultMessage = 0xce;
constexpr uint32_t kScanCompleteMessage = 0xed;
// Firmware-event ids Apple's BCM driver posts through IO80211Controller::postMessage
// to drive the scan manager -> scan FSM.  The WCL FSM only accepts SCAN_RESULT
// (0xc9/0xce) while it is in the IN_PROGRESS state, which is entered by the scan
// manager after it receives INFRA_SCAN_START (0x7d).  Our native producer posts
// 0xc9/0xce/0xed directly to the FSM, so no 0x7d is ever seen and the FSM drops
// every result while it records the trailing 0xed as a no-op transition.  Posting
// 0x7d (and 0x7e) around the existing result stream is the next hypothesis to
// validate on real hardware; it is gated behind itlwm_scan_seq and off by default
// so a single signed trial can compare the two producers without a rebuild.
constexpr uint32_t kInfraScanStart = 0x7d;
constexpr uint32_t kInfraScanComplete = 0x7e;
constexpr uint32_t kInfraScanAdd = 0xc6;
// WCLBulletinBoardMsgType of the scan-result family. The family's msgIndex encodes
// (msgId << 16) | msgType: WCLBulletinBoard::sendMessage reads the low 16 bits as
// msgType and the high 16 as msgId, and callMsgCallBack matches the full index against
// its handler table (0xc90002 for 0xc9). Posting the bare id left msgType==0, so every
// result resolved to msgId 0 / msgType 0 and was dropped before reaching the manager.
constexpr uint32_t kScanResultMsgType = 2;
// 24G830's setBeaconDataFromMsg forwards the inline metadata SSID to
// setBeaconDataEx when bit 1 is set.  Bit 2 suppresses that path and was used
// only by an earlier diagnostic candidate.  Keep bit 1 for the native
// producer, with RSSI validity independent in bit 14.
constexpr uint32_t kBeaconSSIDPresent = 0x2;
constexpr uint32_t kBeaconSSIDValid = kBeaconSSIDPresent;
constexpr uint32_t kBeaconRSSIValid = 0x4000;

struct BeaconMetadata {
    uint32_t ieLength;
    uint16_t channelSpec;
    uint8_t ssid[32];
    uint8_t ssidLength;
    uint8_t primaryChannel;
    uint8_t reserved28;
    uint8_t bssid[6];
    uint8_t reserved2f;
    int32_t rssi;
    int16_t noise;
    uint16_t snr;
    // 24G830 setBeaconDataFromMsg: +0x38 -> ivars+0x294 (period),
    // +0x3a -> ivars+0x29a (capabilities, including the Privacy bit).
    uint16_t beaconInterval;
    uint16_t capability;
    uint32_t flags;
};

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "WCL codec requires little endian");
static_assert(sizeof(BeaconMetadata) == 0x40, "WCL beacon metadata size");
static_assert(offsetof(BeaconMetadata, channelSpec) == 0x04, "WCL channel offset");
static_assert(offsetof(BeaconMetadata, ssid) == 0x06, "WCL SSID offset");
static_assert(offsetof(BeaconMetadata, ssidLength) == 0x26, "WCL SSID length offset");
static_assert(offsetof(BeaconMetadata, primaryChannel) == 0x27, "WCL primary channel offset");
static_assert(offsetof(BeaconMetadata, bssid) == 0x29, "WCL BSSID offset");
static_assert(offsetof(BeaconMetadata, rssi) == 0x30, "WCL RSSI offset");
static_assert(offsetof(BeaconMetadata, noise) == 0x34, "WCL noise offset");
static_assert(offsetof(BeaconMetadata, snr) == 0x36, "WCL SNR offset");
static_assert(offsetof(BeaconMetadata, beaconInterval) == 0x38, "WCL interval offset");
static_assert(offsetof(BeaconMetadata, capability) == 0x3a, "WCL capability offset");
static_assert(offsetof(BeaconMetadata, flags) == 0x3c, "WCL flags offset");

constexpr size_t kWLBSSFixedSize = 0x80;
constexpr size_t kMaxWLBSSMessage = kWLBSSFixedSize + kMaxInformationElements;
// WCLScanManager::handleScanResult and scanupdateHandler pass the bulletin
// data pointer itself as BeaconMetaData& and message+0x40 as the IE pointer.
// The first 0x40 bytes are therefore the parsed metadata, not an opaque
// prefix. The current-BSS query (slot 520) uses the same direct layout.
constexpr size_t kMaxBeaconMessage = sizeof(BeaconMetadata) + kMaxInformationElements;
constexpr size_t kMaxScanBeaconMessage = kMaxBeaconMessage;

enum class Band { GHz2, GHz5 };

constexpr size_t kMaxSupportedChannels = 400;
enum ChannelFlags : uint32_t {
    Width20MHz = 0x2,
    Band2GHz = 0x8,
    Band5GHz = 0x10,
    ActiveScan = 0x80,
    DFS = 0x100,
};

struct Channel {
    uint32_t version;
    uint32_t number;
    uint32_t flags;
};

struct SupportedChannels {
    uint32_t version;
    uint32_t count;
    Channel channels[kMaxSupportedChannels];
    uint32_t includeAnyFlags;
    uint32_t excludeAnyFlags;
    uint8_t hardwareQuery;
    uint8_t countryOverride;
    uint8_t countryAndReserved[6]; // Country starts here; preserve the opaque tail.
};

static_assert(sizeof(Channel) == 12, "WCL channel entry size");
static_assert(offsetof(Channel, number) == 4, "WCL channel number offset");
static_assert(offsetof(Channel, flags) == 8, "WCL channel flags offset");
static_assert(sizeof(SupportedChannels) == 0x12d8, "WCL supported channels size");
static_assert(offsetof(SupportedChannels, channels) == 8, "WCL channel list offset");
static_assert(offsetof(SupportedChannels, includeAnyFlags) == 0x12c8, "WCL include filter offset");
static_assert(offsetof(SupportedChannels, excludeAnyFlags) == 0x12cc, "WCL exclude filter offset");
static_assert(offsetof(SupportedChannels, hardwareQuery) == 0x12d0, "WCL hardware query offset");
static_assert(offsetof(SupportedChannels, countryOverride) == 0x12d1, "WCL country override offset");
static_assert(offsetof(SupportedChannels, countryAndReserved) == 0x12d2, "WCL country offset");

constexpr size_t kMaxScanSSIDs = 10;
struct ScanSSID {
    uint32_t version; // 1, or 0 for Apple's all-zero terminal wildcard entry.
    uint32_t length;
    uint8_t bytes[32];
    uint8_t opaque[16];
};

struct ScanRequest {
    uint32_t version;
    uint8_t privateMAC;
    uint8_t reserved05[3];
    uint32_t homeAwayTime;
    uint32_t control0c;
    uint32_t bssType;
    uint8_t bssid[6];
    uint8_t reserved1a[2];
    uint32_t ssidLength;
    uint8_t ssid[32];
    uint32_t scanType;
    uint16_t scanFlags;
    uint8_t reserved46[2];
    uint32_t activeDwell;
    uint32_t passiveDwell;
    uint32_t restTime;
    uint32_t channelCount;
    Channel channels[kMaxSupportedChannels];
    uint32_t ssidCount;
    ScanSSID ssids[kMaxScanSSIDs];
    uint32_t shortSSID;
};

static_assert(sizeof(ScanSSID) == 0x38, "WCL scan SSID stride");
static_assert(offsetof(ScanSSID, length) == 4 && offsetof(ScanSSID, bytes) == 8, "WCL scan SSID fields");
static_assert(sizeof(ScanRequest) == 0x1550, "WCL scan request size");
static_assert(offsetof(ScanRequest, privateMAC) == 4, "WCL scan private MAC offset");
static_assert(offsetof(ScanRequest, homeAwayTime) == 8, "WCL scan home-away offset");
static_assert(offsetof(ScanRequest, control0c) == 0xc, "WCL scan control offset");
static_assert(offsetof(ScanRequest, bssType) == 0x10, "WCL scan BSS type offset");
static_assert(offsetof(ScanRequest, bssid) == 0x14, "WCL scan BSSID offset");
static_assert(offsetof(ScanRequest, ssidLength) == 0x1c && offsetof(ScanRequest, ssid) == 0x20, "WCL scan SSID offsets");
static_assert(offsetof(ScanRequest, scanType) == 0x40, "WCL scan type offset");
static_assert(offsetof(ScanRequest, scanFlags) == 0x44, "WCL scan flags offset");
static_assert(offsetof(ScanRequest, activeDwell) == 0x48, "WCL scan active dwell offset");
static_assert(offsetof(ScanRequest, passiveDwell) == 0x4c, "WCL scan passive dwell offset");
static_assert(offsetof(ScanRequest, restTime) == 0x50, "WCL scan rest time offset");
static_assert(offsetof(ScanRequest, channelCount) == 0x54, "WCL scan channel count offset");
static_assert(offsetof(ScanRequest, channels) == 0x58, "WCL scan channel list offset");
static_assert(offsetof(ScanRequest, ssidCount) == 0x1318, "WCL scan SSID count offset");
static_assert(offsetof(ScanRequest, ssids) == 0x131c, "WCL scan SSID list offset");
static_assert(offsetof(ScanRequest, shortSSID) == 0x154c, "WCL scan short SSID offset");

// Structural bounds only. Success does NOT mean the HAL supports the requested
// modes, random MAC, channel flags, timing, or the opaque per-SSID controls.
// The caller must keep the input stable during validation and subsequent use.
inline bool validScanRequestBounds(const void *request, size_t size)
{
    if (!request || size < sizeof(ScanRequest))
        return false;
    const auto *bytes = static_cast<const uint8_t *>(request);
    uint32_t length, channelCount, ssidCount;
    memcpy(&length, bytes + offsetof(ScanRequest, ssidLength), sizeof(length));
    memcpy(&channelCount, bytes + offsetof(ScanRequest, channelCount), sizeof(channelCount));
    memcpy(&ssidCount, bytes + offsetof(ScanRequest, ssidCount), sizeof(ssidCount));
    if (length > 32 || channelCount > kMaxSupportedChannels || ssidCount > kMaxScanSSIDs)
        return false;
    for (size_t i = 0; i < ssidCount; ++i) {
        memcpy(&length, bytes + offsetof(ScanRequest, ssids) + i * sizeof(ScanSSID) +
               offsetof(ScanSSID, length), sizeof(length));
        if (length > 32)
            return false;
    }
    return true;
}

enum class ChannelQueryStatus { Success, BadArgument, Unsupported, NoSpace };

// Input is an already validated, NVM-restricted channel inventory (at most 400
// entries). Never broaden it for a hardware query. Input must not alias request.
// An error leaves the request unchanged; unaligned request buffers are allowed.
inline ChannelQueryStatus encodeSupportedChannels(void *request, size_t requestSize,
                                                   const Channel *channels, size_t count,
                                                   bool hardware, const uint8_t *currentCountry = nullptr)
{
    if (!request || requestSize < sizeof(SupportedChannels) || (count && !channels))
        return ChannelQueryStatus::BadArgument;
    if (count > kMaxSupportedChannels)
        return ChannelQueryStatus::NoSpace;

    auto *bytes = static_cast<uint8_t *>(request);
    uint32_t includeFlags, excludeFlags;
    memcpy(&includeFlags, bytes + offsetof(SupportedChannels, includeAnyFlags), sizeof(includeFlags));
    memcpy(&excludeFlags, bytes + offsetof(SupportedChannels, excludeAnyFlags), sizeof(excludeFlags));
    // CoreWiFi's default scan obtains countryCode, then calls
    // supportedChannelsWithCountryCode:error: with that same country. This is
    // a read of the current NVM inventory, not regulatory reconfiguration.
    // Only admit an exact, valid current-country query. A different country
    // still needs regulatory support; never broaden the inventory for it.
    // The stock hardware entry point explicitly clears the country override.
    if (!hardware && bytes[offsetof(SupportedChannels, countryOverride)]) {
        const auto *country = bytes + offsetof(SupportedChannels, countryAndReserved);
        if (bytes[offsetof(SupportedChannels, countryOverride)] != 1 || !currentCountry ||
            currentCountry[0] < 'A' || currentCountry[0] > 'Z' ||
            currentCountry[1] < 'A' || currentCountry[1] > 'Z' || currentCountry[2] ||
            memcmp(country, currentCountry, 3))
            return ChannelQueryStatus::Unsupported;
    }
    if (hardware || (!includeFlags && !excludeFlags))
        includeFlags = UINT32_MAX;

    memset(bytes, 0, offsetof(SupportedChannels, includeAnyFlags));
    const uint32_t version = 1;
    memcpy(bytes, &version, sizeof(version));
    memcpy(bytes + offsetof(SupportedChannels, includeAnyFlags), &includeFlags, sizeof(includeFlags));
    if (hardware) {
        bytes[offsetof(SupportedChannels, hardwareQuery)] = 1;
        bytes[offsetof(SupportedChannels, countryOverride)] = 0;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!(channels[i].flags & includeFlags) || (channels[i].flags & excludeFlags))
            continue;
        memcpy(bytes + offsetof(SupportedChannels, channels) + written * sizeof(Channel),
               channels + i, sizeof(Channel));
        ++written;
    }
    memcpy(bytes + offsetof(SupportedChannels, count), &written, sizeof(written));
    return ChannelQueryStatus::Success;
}

struct BeaconInput {
    const uint8_t *ssid;
    size_t ssidLength;
    const uint8_t *bssid; // Six bytes owned by the caller.
    const uint8_t *ies;
    size_t ieLength;
    unsigned channel;
    Band band;
    int32_t rssi;
    uint16_t capability;
    uint16_t beaconInterval;
};

inline bool validInformationElements(const uint8_t *bytes, size_t length)
{
    if (length > kMaxInformationElements || (length && !bytes))
        return false;
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 2)
            return false;
        size_t elementLength = bytes[offset + 1];
        if (elementLength > length - offset - 2)
            return false;
        if (bytes[offset] == 255 && elementLength == 0)
            return false; // Extended IEs must contain an extension ID.
        offset += 2 + elementLength;
    }
    return true;
}

// The 24G830 WCL BSS decoder applies stricter checks to a handful of
// information elements than the generic TLV bounds check above.  Keep this
// inspection read-only and bounded: it is used only to explain an Apple-side
// cache rejection after the producer has already decoded a beacon.
struct InformationElementShape {
    uint32_t total = 0;
    uint32_t firstId = UINT32_MAX;
    uint32_t ssid = 0;
    uint32_t ssidLength = 0;
    uint32_t ds = 0;
    uint32_t dsLength = 0;
    uint32_t dsBadLength = 0;
    uint32_t ibss = 0;
    uint32_t ibssLength = 0;
    uint32_t ibssBadLength = 0;
    uint32_t qbss = 0;
    uint32_t qbssLength = 0;
    uint32_t qbssBadLength = 0;
    uint32_t vht = 0;
    uint32_t vhtLength = 0;
    uint32_t vhtBadLength = 0;
    uint32_t malformed = 0;
};

inline InformationElementShape inspectInformationElements(const uint8_t *bytes, size_t length)
{
    InformationElementShape result;
    if (length && !bytes) {
        result.malformed = 1;
        return result;
    }
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 2) {
            result.malformed = 1;
            break;
        }
        const uint8_t id = bytes[offset];
        const size_t elementLength = bytes[offset + 1];
        if (result.total == 0) result.firstId = id;
        if (elementLength > length - offset - 2) {
            result.malformed = 1;
            break;
        }
        ++result.total;
        switch (id) {
            case 0:
                ++result.ssid;
                result.ssidLength = static_cast<uint32_t>(elementLength);
                break;
            case 3:
                ++result.ds;
                result.dsLength = static_cast<uint32_t>(elementLength);
                if (elementLength != 1) ++result.dsBadLength;
                break;
            case 6:
                ++result.ibss;
                result.ibssLength = static_cast<uint32_t>(elementLength);
                // Apple rejects a one-byte IBSS Parameter Set; zero and the
                // complete 6-byte form are retained as distinct diagnostics.
                if (elementLength == 1) ++result.ibssBadLength;
                break;
            case 11:
                ++result.qbss;
                result.qbssLength = static_cast<uint32_t>(elementLength);
                // QBSS Load is normally 5 bytes. Record any other shape;
                // this is diagnostic only and does not alter the payload.
                if (elementLength != 5) ++result.qbssBadLength;
                break;
            case 191:
                ++result.vht;
                result.vhtLength = static_cast<uint32_t>(elementLength);
                // The audited setBeaconDataEx expects VHT Capabilities' 12
                // byte body (14 bytes including the TLV header).
                if (elementLength != 12) ++result.vhtBadLength;
                break;
            default:
                break;
        }
        offset += 2 + elementLength;
    }
    return result;
}

inline uint16_t channelSpec20MHz(unsigned channel, Band band)
{
    if (!channel || channel > 255)
        return 0;
    if (band == Band::GHz2 && channel <= 14)
        return static_cast<uint16_t>(0x1000 | channel);
    if (band == Band::GHz5 && channel > 14)
        return static_cast<uint16_t>(0xd000 | channel);
    return 0;
}

inline bool channel20MHz(unsigned number, Band band, bool passive, bool dfs, Channel &output)
{
    if (!channelSpec20MHz(number, band))
        return false;
    output = {1, number, Width20MHz | (band == Band::GHz2 ? Band2GHz : Band5GHz) |
                         (passive ? 0u : ActiveScan) | (dfs ? DFS : 0u)};
    return true;
}

// The legacy CHANNELS_INFO ioctl is still queried by airportd on Sequoia.
// Keep its per-channel mapping portable so the kernel adapter and offline
// fixtures exercise the same chanspec/flag rules.  The restriction and width
// values are supplied by the HAL inventory; no capability is inferred here.
struct LegacyChannelInfoEntry {
    uint16_t channelSpec;
    uint8_t channel;
    uint8_t indoorRestriction;
    uint8_t radarDFS;
    uint8_t passive;
    uint8_t support40MHz;
    uint8_t support80MHz;
};

inline bool legacyChannelInfo(unsigned number, Band band, bool passive, bool dfs,
                              bool support40MHz, bool support80MHz,
                              LegacyChannelInfoEntry &output)
{
    const uint16_t spec = channelSpec20MHz(number, band);
    if (!spec)
        return false;
    output = {spec, static_cast<uint8_t>(number), 0, static_cast<uint8_t>(dfs),
              static_cast<uint8_t>(passive), static_cast<uint8_t>(support40MHz),
              static_cast<uint8_t>(support80MHz)};
    return true;
}

// Returns zero without changing output on invalid input. Input and output must
// not overlap; the caller owns all input spans for the duration of this call.
inline size_t encodeBeacon(const BeaconInput &input, void *output, size_t capacity)
{
    const uint16_t wlChannelSpec = channelSpec20MHz(input.channel, input.band);
    if (!output || input.ssidLength > 32 || (input.ssidLength && !input.ssid) ||
        !input.bssid || !wlChannelSpec || input.ieLength > kMaxInformationElements ||
        capacity < sizeof(BeaconMetadata) + input.ieLength ||
        !validInformationElements(input.ies, input.ieLength))
        return 0;
    uint8_t addressBits = 0;
    for (size_t i = 0; i < 6; ++i)
        addressBits |= input.bssid[i];
    if (!addressBits || (input.bssid[0] & 1))
        return 0;

    BeaconMetadata metadata = {};
    metadata.ieLength = static_cast<uint32_t>(input.ieLength);
    // WCL consumes AppleChannelSpec_t in both the scan and current-BSS
    // metadata paths. Its fillLegacy20MHzChanSpec uses these band/width bits;
    // no Broadcom hardware or firmware-API-version assumption is needed.
    metadata.channelSpec = wlChannelSpec;
    if (input.ssidLength)
        memcpy(metadata.ssid, input.ssid, input.ssidLength);
    metadata.ssidLength = static_cast<uint8_t>(input.ssidLength);
    metadata.primaryChannel = static_cast<uint8_t>(input.channel);
    memcpy(metadata.bssid, input.bssid, sizeof(metadata.bssid));
    metadata.rssi = input.rssi;
    metadata.capability = input.capability;
    metadata.beaconInterval = input.beaconInterval;
    // setBeaconDataFromMsg tests bit 14 BEFORE storing RSSI and its receive
    // timestamp. A numeric RSSI without this flag is discarded. Bits 12/13
    // claim measured noise/SNR, which this HAL does not supply; leave clear.
    // d89 leaves bits 1/2 clear to exercise the other audited
    // setBeaconDataFromMsg branch.  RSSI validity remains independent.
    metadata.flags = kBeaconSSIDValid | kBeaconRSSIValid;
    memcpy(output, &metadata, sizeof(metadata));
    if (input.ieLength)
        memcpy(static_cast<uint8_t *>(output) + sizeof(metadata), input.ies, input.ieLength);
    return sizeof(metadata) + input.ieLength;
}

// Encode the payload passed directly to IO80211Controller::postMessage for
// the parsed scan-result message (currently 0xce). The receiver's call site
// passes the message start as BeaconMetaData& and message+0x40 as the IE
// pointer, so this is exactly BeaconMetadata followed by its declared IE
// bytes. There is no extra prefix before the metadata.
inline size_t encodeScanBeacon(const BeaconInput &input, void *output, size_t capacity)
{
    return encodeBeacon(input, output, capacity);
}

// Encode the Broadcom wl_bss_info record that 24G830's
// AppleBCMWLANScanAdapter::processScanResults consumes.  This is deliberately
// a byte-layout codec: the private structure is not available in the SDK and
// the Apple parser reads the fields at the fixed offsets below. This codec is
// for offline producer comparison only; WCL messages use BeaconMetadata.
inline size_t encodeWLBSSInfo(const BeaconInput &input, void *output, size_t capacity)
{
    const uint16_t channelSpec = channelSpec20MHz(input.channel, input.band);
    if (!output || input.ssidLength > 32 || (input.ssidLength && !input.ssid) ||
        !input.bssid || !channelSpec || input.ieLength > kMaxInformationElements ||
        input.ieLength > SIZE_MAX - kWLBSSFixedSize ||
        capacity < kWLBSSFixedSize + input.ieLength ||
        !validInformationElements(input.ies, input.ieLength))
        return 0;
    uint8_t addressBits = 0;
    for (size_t i = 0; i < 6; ++i)
        addressBits |= input.bssid[i];
    if (!addressBits || (input.bssid[0] & 1) || input.rssi < INT16_MIN || input.rssi > INT16_MAX)
        return 0;

    uint8_t header[kWLBSSFixedSize] = {};
    auto put16 = [&](size_t offset, uint16_t value) { memcpy(header + offset, &value, sizeof(value)); };
    auto put32 = [&](size_t offset, uint32_t value) { memcpy(header + offset, &value, sizeof(value)); };
    // WL_BSS_INFO_VERSION 109 is the version Apple special-cases for the
    // 802.11n/HT capability flags.  The parser otherwise only needs the
    // stable fields below and the IE span.
    put32(0x00, 109);
    put32(0x04, static_cast<uint32_t>(kWLBSSFixedSize + input.ieLength));
    memcpy(header + 0x08, input.bssid, 6);
    put16(0x0e, input.capability);
    put16(0x10, input.beaconInterval);
    header[0x12] = static_cast<uint8_t>(input.ssidLength);
    if (input.ssidLength) memcpy(header + 0x13, input.ssid, input.ssidLength);
    put16(0x48, channelSpec);
    put16(0x4e, static_cast<uint16_t>(static_cast<int16_t>(input.rssi)));
    // 0x51 == n_cap.  Leaving it clear makes Apple derive the primary channel
    // from the supplied chanspec, while these optional capability bytes stay
    // conservative for Intel frames.
    header[0x51] = 0;
    header[0x58] = static_cast<uint8_t>(input.channel);
    put32(0x74, static_cast<uint32_t>(kWLBSSFixedSize));
    put32(0x78, static_cast<uint32_t>(input.ieLength));
    memcpy(output, header, sizeof(header));
    if (input.ieLength)
        memcpy(static_cast<uint8_t *>(output) + sizeof(header), input.ies, input.ieLength);
    return sizeof(header) + input.ieLength;
}

} // namespace SequoiaWCL

#endif
