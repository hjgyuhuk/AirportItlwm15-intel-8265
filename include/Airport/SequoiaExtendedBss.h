#ifndef AIRPORT_SEQUOIA_EXTENDED_BSS_H
#define AIRPORT_SEQUOIA_EXTENDED_BSS_H

#include <Airport/SequoiaJoinRequest.h>

namespace SequoiaWCL {
// 24G830 setCurrentBSS allocates 0x1e0. BCM fills rate set, HT/VHT/HE
// maps, MLO byte and associated RSN IE at these exact offsets. In particular
// VHT/HE occupy eight bytes, unlike the old packed six-byte VHT ioctl.
struct ExtendedBssInfo {
    struct Rate { uint32_t version, mbps, flags; };
    uint32_t version;
    uint16_t rateCount, reserved06;
    Rate rates[15];
    struct Ht { uint32_t version; uint8_t map[10], reserved[2]; } ht;
    struct Mcs { uint32_t version; uint16_t map, reserved; } vht, he;
    uint8_t mlo;
    uint8_t rsnIE[257];
    uint8_t reserved1de[2];
};
static_assert(sizeof(ExtendedBssInfo) == 0x1e0, "24G830 extended BSS length");
static_assert(offsetof(ExtendedBssInfo, ht) == 0xbc, "24G830 HT map");
static_assert(offsetof(ExtendedBssInfo, vht) == 0xcc, "24G830 VHT map");
static_assert(offsetof(ExtendedBssInfo, he) == 0xd4, "24G830 HE map");
static_assert(offsetof(ExtendedBssInfo, mlo) == 0xdc, "24G830 MLO flag");
static_assert(offsetof(ExtendedBssInfo, rsnIE) == 0xdd, "24G830 associated RSN IE");

inline uint16_t intersectMcsMaps(uint16_t peerRx, uint16_t localTx)
{
    uint16_t result = 0;
    for (unsigned shift = 0; shift < 16; shift += 2) {
        unsigned a = (peerRx >> shift) & 3, b = (localTx >> shift) & 3;
        result |= static_cast<uint16_t>((a == 3 || b == 3 ? 3 : (a < b ? a : b)) << shift);
    }
    return result;
}

inline bool encodeExtendedBss(const uint8_t *rates, size_t count,
    const uint8_t *ht, uint16_t vht, uint16_t he,
    const uint8_t *ies, size_t ieLength, void *data)
{
    if (!data || !rates || !count || count > 15 || !ht || !ies ||
        !ieLength || ieLength > 2048) return false;
    const uint8_t *rsn = nullptr;
    size_t rsnLength = 0;
    for (size_t at = 0; at < ieLength;) {
        if (ieLength - at < 2) return false;
        size_t length = static_cast<size_t>(ies[at + 1]) + 2;
        if (length > ieLength - at) return false;
        if (ies[at] == 48) {
            if (rsn || length < 4 || ies[at + 2] != 1 || ies[at + 3] != 0) return false;
            rsn = ies + at; rsnLength = length;
        }
        at += length;
    }
    // Native admission currently permits only WPA2/PSK. No fabricated IE.
    if (!rsn) return false;
    ExtendedBssInfo result = {};
    result.version = result.ht.version = result.vht.version = result.he.version = 1;
    result.rateCount = static_cast<uint16_t>(count);
    for (size_t i = 0; i < count; ++i) {
        if (!(rates[i] & 0x7f)) return false;
        // Apple's mapBcomRatesetToAppleRateset uses integer Mbps and basic bit.
        result.rates[i] = {1, static_cast<uint32_t>((rates[i] >> 1) & 0x3f),
                             static_cast<uint32_t>(rates[i] >> 7)};
    }
    memcpy(result.ht.map, ht, sizeof(result.ht.map));
    result.vht.map = vht; result.he.map = he;
    memcpy(result.rsnIE, rsn, rsnLength);
    memcpy(data, &result, sizeof(result)); // Caller buffer need not be aligned.
    return true;
}
} // namespace SequoiaWCL
#endif
