#include <HAL/ItlScanCachePacket.h>

void ItlIwm::iwm_native_scan_cache_rx(uint16_t code, const void *payload, size_t size)
{
    auto &d = nativeCacheDiagnostic;
    using ItlScan::CacheDiagnostic;
    if (code == IWM_REPLY_RX_MPDU_CMD) CacheDiagnostic::bump(d.frames);
    if (!nativeScanPlan || !nativeScanSink.valid() || nativeScanCancel ||
        !getMainWorkLoop()->inGate() || com.sc_ic.ic_state != IEEE80211_S_INIT) {
        if (code == IWM_REPLY_RX_MPDU_CMD) CacheDiagnostic::bump(d.eligibilityRejected);
        return;
    }
    if (code == IWM_REPLY_RX_PHY_CMD) {
        nativeScanCachePhy = {};
        if (ItlScan::decodeLegacyPhy(payload, size, nativeScanCachePhy) != ItlScan::PhyDecode::Accepted)
            nativeScanCachePhy = {};
        return;
    }
    if (code != IWM_REPLY_RX_MPDU_CMD) {
        if (code == IWM_SCAN_OFFLOAD_COMPLETE) nativeScanCachePhy = {};
        return;
    }
    const auto *bytes = static_cast<const uint8_t *>(payload);
    if (!bytes || size < 8 || !nativeScanCachePhy.channel) {
        CacheDiagnostic::bump(d.frameRejected); return;
    }
    const size_t length = ItlScan::scanRead16(bytes);
    d.lastFrameLength = length;
    if (length > size - 8 || length < 36 || (bytes[4 + length] & 3) != 3) {
        CacheDiagnostic::bump(d.frameRejected); return;
    }
    ItlScan::BeaconView beacon;
    if (!ItlScan::decodeScanBeacon(bytes + 4, length, beacon) ||
        !nativeScanPlan->matches(nativeScanCachePhy, beacon) ||
        (beacon.capability & 3) != 1 || memcmp(bytes + 14, beacon.bssid, 6)) {
        CacheDiagnostic::bump(d.filterRejected); return;
    }
    const auto channel = nativeScanCachePhy.channel;
    if (channel >= nitems(com.sc_ic.ic_channels) || !isset(com.sc_ic.ic_chan_avail, channel) ||
        !com.sc_ic.ic_channels[channel].ic_freq) {
        CacheDiagnostic::bump(d.channelRejected); return;
    }
    mbuf_t packet = ItlScan::copyCacheFrame(bytes + 4, length, d);
    if (!packet) return;
    ieee80211_rxinfo info = {};
    info.rxi_chan = channel;
    info.rxi_rssi = nativeScanCachePhy.rssi - IWM_MIN_DBM;
    if (info.rxi_rssi < 0) info.rxi_rssi = 0;
    if (info.rxi_rssi > com.sc_ic.ic_max_rssi) info.rxi_rssi = com.sc_ic.ic_max_rssi;
    CacheDiagnostic::bump(d.parserCalls);
    d.nodeLimit = com.sc_ic.ic_max_nnodes;
    ieee80211_collect_bss(&com.sc_ic, packet, &info,
                         (bytes[4] & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_PROBE_RESP);
    if (ieee80211_find_node(&com.sc_ic, beacon.bssid)) CacheDiagnostic::bump(d.nodeHits);
    mbuf_freem(packet);
}
