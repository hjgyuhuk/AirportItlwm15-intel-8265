#ifndef IWM_SCAN_FRAME_H
#define IWM_SCAN_FRAME_H

#include <HAL/ItlScan.h>

namespace ItlScan {

inline uint16_t scanRead16(const uint8_t *bytes)
{
    return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

struct LegacyPhy {
    uint16_t channel = 0;
    int32_t rssi = -256;
};

enum class PhyDecode { Accepted, BadSize, BadConfig, BadChannel };

inline PhyDecode decodeLegacyPhy(const void *payload, size_t size, LegacyPhy &output)
{
    // RX_PHY may contain a suffix. Only the fixed firmware ABI prefix is read.
    // IWX also normalizes its RX descriptor into this format. Validate the
    // channel range here; the caller checks its admitted scan/NVM inventory.
    const auto *bytes = static_cast<const uint8_t *>(payload);
    if (!bytes || size < 68 || size > 4096) return PhyDecode::BadSize;
    if (bytes[1] > 20) return PhyDecode::BadConfig;
    const uint16_t channel = scanRead16(bytes + 22);
    if (!channel || channel > 196) return PhyDecode::BadChannel;
    LegacyPhy result;
    result.channel = channel;
    // The existing IWM signal helper selects the strongest nonzero energy.
    for (size_t i = 28; i < 31; ++i)
        if (bytes[i] && -int32_t(bytes[i]) > result.rssi) result.rssi = -int32_t(bytes[i]);
    output = result;
    return PhyDecode::Accepted;
}

// Borrowed views are valid only while the RX buffer remains owned by the caller.
struct BeaconView {
    const uint8_t *ssid = nullptr, *bssid = nullptr, *ies = nullptr;
    size_t ssidLength = 0, ieLength = 0;
    uint16_t capability = 0, interval = 0;
    bool probeResponse = false;
};

inline bool decodeScanBeacon(const void *payload, size_t size, BeaconView &output)
{
    const auto *frame = static_cast<const uint8_t *>(payload);
    if (!frame || size < 36 || size - 36 > 2048 || (frame[0] != 0x80 && frame[0] != 0x50) ||
        (frame[1] & 0x47) || (frame[22] & 0x0f) || !validUnicast(frame + 16)) return false;
    BeaconView result;
    result.bssid = frame + 16;
    result.ies = frame + 36;
    result.ieLength = size - 36;
    result.interval = scanRead16(frame + 32);
    result.capability = scanRead16(frame + 34);
    result.probeResponse = frame[0] == 0x50;
    bool ssid = false;
    for (size_t offset = 36; offset < size;) {
        if (size - offset < 2 || frame[offset + 1] > size - offset - 2) return false;
        const size_t length = frame[offset + 1];
        if (frame[offset] == 255 && !length) return false;
        if (!frame[offset]) {
            if (ssid || length > 32) return false;
            ssid = true;
            result.ssid = frame + offset + 2;
            result.ssidLength = length;
        }
        offset += 2 + length;
    }
    if (!ssid) return false;
    output = result;
    return true;
}

} // namespace ItlScan
#endif
