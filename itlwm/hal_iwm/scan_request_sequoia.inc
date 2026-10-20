// Uses the existing BSD/Intel wire structures. No net80211 attachment, node,
// rate table, frame allocation or interface MAC mutation is needed.
#include <Airport/SequoiaScanPlan.h>

namespace IwmTrial {
struct PassivePlan {
    uint16_t channels[ItlScan::IwmPassiveTrial::MaxChannels] = {};
    size_t count = 0, capacity = 0;
    uint8_t source[6] = {}, txAnt = 0, rxAnt = 0;
};

static size_t passiveRequestSize(const PassivePlan &plan)
{
    if (!plan.count || plan.count > plan.capacity || plan.capacity > ItlScan::IwmPassiveTrial::MaxChannels ||
        !ItlScan::validUnicast(plan.source) || !plan.txAnt || (plan.txAnt & ~7) ||
        !plan.rxAnt || (plan.rxAnt & ~7)) return 0;
    for (size_t i = 0; i < plan.count; ++i) {
        const auto channel = plan.channels[i];
        if (!channel || channel > 196 || channel == 144) return 0;
        for (size_t j = 0; j < i; ++j)
            if (channel == plan.channels[j]) return 0;
    }
    const auto bytes = sizeof(iwm_scan_req_lmac) + plan.capacity * sizeof(iwm_scan_channel_cfg_lmac) +
        sizeof(iwm_scan_probe_req_v1);
    return bytes <= IWM_MAX_CMD_PAYLOAD_SIZE ? bytes : 0;
}

static bool buildPassiveRequest(const PassivePlan &plan, void *output, size_t bytes)
{
    const auto expected = passiveRequestSize(plan);
    if (!output || !expected || bytes != expected) return false;
    memset(output, 0, bytes);
    auto *request = static_cast<iwm_scan_req_lmac *>(output);
    request->n_channels = static_cast<uint8_t>(plan.count);
    request->active_dwell = 10;
    request->passive_dwell = 110;
    request->fragmented_dwell = 44;
    request->extended_dwell = 90;
    request->rx_chain_select = htole16((plan.rxAnt << IWM_PHY_RX_CHAIN_VALID_POS) |
        (plan.rxAnt << IWM_PHY_RX_CHAIN_FORCE_MIMO_SEL_POS) |
        (plan.rxAnt << IWM_PHY_RX_CHAIN_FORCE_SEL_POS) | (1 << IWM_PHY_RX_CHAIN_DRIVER_FORCE_POS));
    request->scan_flags = htole32(IWM_LMAC_SCAN_FLAG_PASS_ALL | IWM_LMAC_SCAN_FLAG_PASSIVE |
        IWM_LMAC_SCAN_FLAG_ITER_COMPLETE | IWM_LMAC_SCAN_FLAG_EXTENDED_DWELL);
    request->filter_flags = htole32(IWM_MAC_FILTER_ACCEPT_GRP | IWM_MAC_FILTER_IN_BEACON);
    // Lowest valid TX chain, OFDM 6M (PLCP 0x0d), matching no_cck=1 in
    // iwm_scan_rate_n_flags. Firmware still requires a valid passive template.
    const uint8_t tx = plan.txAnt & (~plan.txAnt + 1);
    for (auto &command : request->tx_cmd) {
        command.tx_flags = htole32(IWM_TX_CMD_FLG_SEQ_CTL | IWM_TX_CMD_FLG_BT_DIS);
        command.rate_n_flags = htole32(0x0d | (uint32_t(tx) << 14));
        command.sta_id = IWM_AUX_STA_ID;
    }
    request->scan_prio = htole32(IWM_SCAN_PRIORITY_HIGH);
    request->iter_num = htole32(1);
    request->schedule[0].iterations = 1;
    request->schedule[0].full_scan_mul = 1;
    request->channel_opt[0].non_ebs_ratio = htole16(1);
    request->channel_opt[1].non_ebs_ratio = htole16(1);
    auto *channels = reinterpret_cast<iwm_scan_channel_cfg_lmac *>(request->data);
    bool has24 = false, has5 = false;
    for (size_t i = 0; i < plan.count; ++i) {
        channels[i].flags = htole32(IWM_UNIFIED_SCAN_CHANNEL_PARTIAL);
        channels[i].channel_num = htole16(plan.channels[i]);
        channels[i].iter_count = htole16(1);
        if (plan.channels[i] <= 14) has24 = true;
        else has5 = true;
    }
    request->flags = htole32((has24 ? IWM_PHY_BAND_24 : 0) |
        (has5 ? IWM_PHY_BAND_5 : 0));
    auto *probe = reinterpret_cast<iwm_scan_probe_req_v1 *>(request->data +
        plan.capacity * sizeof(*channels));
    // The probe request is laid out the way the firmware expects: a MAC header
    // of 26 bytes, then an empty SSID element the hardware fills in, then the
    // per band rate sets, then the data both bands share. The firmware places
    // the SSID element itself, so it has to be told where the header ends.
    probe->buf[0] = 0x40;
    probe->buf[1] = 0x00;
    memset(probe->buf + 4, 0xff, 6);
    memcpy(probe->buf + 10, plan.source, 6);
    memset(probe->buf + 16, 0xff, 6);
    probe->mac_header.offset = 0;
    probe->mac_header.len = htole16(26); // Includes a zero-length SSID IE.
    size_t offset = 26;
    probe->buf[offset++] = 0;             // IEEE80211_ELEMID_SSID
    probe->buf[offset++] = 0;             // Empty; the hardware inserts it.
    // Standard legacy rates, independent of the unattached ic_sup_rates.
    const uint8_t rates24[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 12, 18, 24, 36, 50, 4, 48, 72, 96, 108};
    const uint8_t rates5[] = {1, 8, 0x8c, 18, 0x98, 36, 0xb0, 72, 96, 108};
    for (unsigned band = 0; band < 2; ++band) {
        const bool included = band ? has5 : has24;
        if (!included) continue;
        probe->band_data[band].offset = htole16(offset);
        const auto length = band ? sizeof(rates5) : sizeof(rates24);
        memcpy(probe->buf + offset, band ? rates5 : rates24, length);
        probe->band_data[band].len = htole16(length);
        offset += length;
    }
    probe->common_data.offset = htole16(offset);
    probe->common_data.len = htole16(0);
    return true;
}

static inline bool buildNativeScanRequest(const SequoiaWCL::ScanPlan &native,
                                     size_t capacity, uint8_t txAnt, uint8_t rxAnt,
                                     bool fragmented, void *output, size_t bytes)
{
    if (!native.valid() || !native.channelCount || native.channelCount > SequoiaWCL::ScanPlan::MaxChannels)
        return false;
    PassivePlan plan;
    plan.count = native.channelCount;
    plan.capacity = capacity;
    plan.txAnt = txAnt;
    plan.rxAnt = rxAnt;
    memcpy(plan.channels, native.channels, plan.count * sizeof(plan.channels[0]));
    memcpy(plan.source, native.source, sizeof(plan.source));
    if (!buildPassiveRequest(plan, output, bytes)) return false;
    auto *request = static_cast<iwm_scan_req_lmac *>(output);
    // Bound the dwell so a scan finishes inside the freshness window airportd's
    // auto-join demands. Its own log shows what a slower scan costs:
    //
    //     AUTO-JOIN: Scanning (dwellTime=110ms, cacheOnly=0, maxCacheAge=2111ms)
    //     AUTO-JOIN: Scan Results (0) -->          (415 times)
    //     trigger=retry 111, trigger=known_network_added 109
    //
    // With the caller's dwells, 37 channels at 110 ms take about four seconds, so every
    // result the auto-join was offered was older than its 2111 ms cache age, it accepted
    // nothing, and it retried forever - which is what keeps ControlCenter animating and
    // inactive instead of listing networks, and why WiFiAgent holds 220 networks that
    // never reach the menu. r163 clamped the plan instead, which put a scan at 2.1 s -
    // still on the wrong side of that boundary - and cost results (81..91 per scan fell
    // to 62..72). Bounding the dwells on the wire leaves the plan's recorded request
    // values, and the plan tests, untouched.
    constexpr uint32_t MaxPassiveDwell = 45;
    constexpr uint32_t MaxActiveDwell = 20;
    request->passive_dwell = native.passiveDwell < MaxPassiveDwell ? native.passiveDwell : MaxPassiveDwell;
    request->active_dwell = native.activeDwell < MaxActiveDwell ? native.activeDwell : MaxActiveDwell;
    if (fragmented && native.associatedScan()) {
        // r225 associated bgscan wire.  FRAGMENTED makes the firmware
        // alternate off-channel passive dwells with suspend periods on the
        // serving channel (iwlwifi bgscan defaults: 120 TU suspend, 600 TU
        // max off-channel, fragmented dwell 95), so the association's beacons
        // and data keep flowing while the neighbours are collected.  The
        // caller passes fragmented only after checking
        // IWM_UCODE_TLV_API_FRAGMENTED_SCAN; without the API bit the plan
        // still scans, just in one uninterrupted off-channel stretch.
        request->fragmented_dwell = 95;
        request->max_out_time = htole32(600);
        request->suspend_time = htole32(120);
        request->scan_flags |= htole32(IWM_LMAC_SCAN_FLAG_FRAGMENTED);
    }
    if (!native.active()) return true;
    const size_t count = native.probeSSIDCount();
    if (!count || count > sizeof(request->direct_scan) / sizeof(request->direct_scan[0]) || count > 20)
        return false;
    // LMAC selects SSID entries with bits 1..20, unlike UMAC's 0..19.
    const uint32_t bitmap = ((1u << count) - 1) << 1;
    uint32_t flags = IWM_LMAC_SCAN_FLAG_PASS_ALL | IWM_LMAC_SCAN_FLAG_ITER_COMPLETE;
    if (count == 1 && native.probeSSIDLength(0)) flags |= IWM_LMAC_SCAN_FLAG_PRE_CONNECTION;
    request->scan_flags = htole32(flags);
    for (size_t i = 0; i < count; ++i) {
        auto &ssid = request->direct_scan[i];
        const size_t length = native.probeSSIDLength(i);
        if (length > sizeof(ssid.ssid) || (length && !native.probeSSID(i))) return false;
        ssid.len = static_cast<uint8_t>(length);
        if (length) memcpy(ssid.ssid, native.probeSSID(i), length);
    }
    auto *channels = reinterpret_cast<iwm_scan_channel_cfg_lmac *>(request->data);
    for (size_t i = 0; i < plan.count; ++i)
        channels[i].flags = htole32(IWM_UNIFIED_SCAN_CHANNEL_PARTIAL | (native.probes[i] ? bitmap : 0));
    auto *probe = reinterpret_cast<iwm_scan_probe_req_v1 *>(request->data + plan.capacity * sizeof(*channels));
    if (native.probeBSSID()) memcpy(probe->buf + 16, native.probeBSSID(), 6);
    return true;
}
} // namespace IwmTrial
