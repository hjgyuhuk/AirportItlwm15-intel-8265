#ifndef SEQUOIA_SCAN_FRAME_H
#define SEQUOIA_SCAN_FRAME_H

#include <Airport/SequoiaWCL.h>
#include <HAL/IwmScanFrame.h>

namespace SequoiaWCL {

// Offline-tested conversion only. It neither publishes nor retains a result.
inline size_t encodeIwmBeacon(const ItlScan::LegacyPhy &phy, const void *frame, size_t size,
                              void *output, size_t capacity)
{
    ItlScan::BeaconView view;
    if (!phy.channel || phy.channel > 196 || phy.rssi < -255 || phy.rssi > 0 ||
        !ItlScan::decodeScanBeacon(frame, size, view)) return 0;
    BeaconInput input = {};
    input.ssid = view.ssid; input.ssidLength = view.ssidLength;
    input.bssid = view.bssid; input.ies = view.ies; input.ieLength = view.ieLength;
    input.channel = phy.channel;
    input.band = phy.channel <= 14 ? Band::GHz2 : Band::GHz5;
    input.rssi = phy.rssi;
    input.capability = view.capability; input.beaconInterval = view.interval;
    // The direct controller post path starts with BeaconMetadata; WCL passes
    // its first 0x40 bytes as metadata and the following bytes as IEs.
    return encodeScanBeacon(input, output, capacity);
}

} // namespace SequoiaWCL
#endif
