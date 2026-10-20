// The 8000 family firmware takes the scan request in the UMAC form. The
// request this tree builds was written for the 7000 family, and the 8265's
// firmware 36 resets when it sees that request. The firmware reports the
// UMAC scan capability, and the driver already sends a UMAC request whenever
// it does, so the native path sends that same request with the native plan's
// channels and SSIDs.

size_t ItlIwm::iwm_native_umac_scan_size(size_t capacity) const
{
    auto *sc = &com;
    const bool adaptiveDwell = isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_ADAPTIVE_DWELL);
    const bool adaptiveDwellV2 = isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_ADAPTIVE_DWELL_V2);
    const bool extendedChan = isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_SCAN_EXT_CHAN_VER);
    const size_t base = adaptiveDwellV2 ? IWM_SCAN_REQ_UMAC_SIZE_V8 :
        adaptiveDwell ? IWM_SCAN_REQ_UMAC_SIZE_V7 : IWM_SCAN_REQ_UMAC_SIZE_V1;
    const size_t tail = extendedChan ? sizeof(struct iwm_scan_req_umac_tail_v2) :
                                      sizeof(struct iwm_scan_req_umac_tail_v1);
    return base + sizeof(struct iwm_scan_channel_cfg_umac) * capacity + tail;
}

int ItlIwm::iwm_native_umac_scan_request(const SequoiaWCL::ScanPlan &plan,
                                          const uint16_t *channels, size_t channelCount,
                                          size_t capacity, void *output, size_t bytes)
{
    auto *sc = &com;
    const bool adaptiveDwell = isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_ADAPTIVE_DWELL);
    const bool adaptiveDwellV2 = isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_ADAPTIVE_DWELL_V2);
    const bool extendedChan = isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_SCAN_EXT_CHAN_VER);
    const size_t base = adaptiveDwellV2 ? IWM_SCAN_REQ_UMAC_SIZE_V8 :
        adaptiveDwell ? IWM_SCAN_REQ_UMAC_SIZE_V7 : IWM_SCAN_REQ_UMAC_SIZE_V1;
    const size_t tail = extendedChan ? sizeof(struct iwm_scan_req_umac_tail_v2) :
                                      sizeof(struct iwm_scan_req_umac_tail_v1);
    const size_t expected = base + sizeof(struct iwm_scan_channel_cfg_umac) * capacity + tail;
    if (!output || bytes != expected || expected > IWM_MAX_CMD_PAYLOAD_SIZE) return EINVAL;
    if (!channels || !channelCount || channelCount > capacity) return EINVAL;
    memset(output, 0, bytes);
    auto *request = static_cast<struct iwm_scan_req_umac *>(output);

    // Bound the dwell so a scan finishes inside the freshness window
    // airportd's auto-join demands. The same bound applies to the LMAC
    // request; see buildNativeScanRequest for the numbers.
    constexpr uint32_t MaxPassiveDwell = 45;
    constexpr uint32_t MaxActiveDwell = 20;
    const auto passiveDwell = static_cast<uint8_t>(
        plan.passiveDwell < MaxPassiveDwell ? plan.passiveDwell : MaxPassiveDwell);
    const auto activeDwell = static_cast<uint8_t>(
        plan.activeDwell < MaxActiveDwell ? plan.activeDwell : MaxActiveDwell);

    if (adaptiveDwellV2) {
        request->v8.active_dwell[IWM_SCAN_LB_LMAC_IDX] = activeDwell;
        request->v8.passive_dwell[IWM_SCAN_LB_LMAC_IDX] = passiveDwell;
        request->v8.adwell_default_n_aps = IWM_SCAN_ADWELL_DEFAULT_LB_N_APS;
        request->v8.adwell_default_n_aps_social = IWM_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL;
        request->v8.adwell_max_budget = htole16(IWM_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN);
        request->v8.max_out_time[IWM_SCAN_LB_LMAC_IDX] = 0;
        request->v8.suspend_time[IWM_SCAN_LB_LMAC_IDX] = 0;
        request->v8.scan_priority = htole32(IWM_SCAN_PRIORITY_HIGH);
        request->v8.general_flags2 = IWM_UMAC_SCAN_GEN_FLAGS2_ALLOW_CHNL_REORDER;
    } else if (adaptiveDwell) {
        request->v7.active_dwell = activeDwell;
        request->v7.passive_dwell = passiveDwell;
        request->v7.fragmented_dwell = 44;
        request->v7.adwell_default_n_aps = IWM_SCAN_ADWELL_DEFAULT_LB_N_APS;
        request->v7.adwell_default_n_aps_social = IWM_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL;
        request->v7.adwell_max_budget = htole16(IWM_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN);
        request->v7.max_out_time[IWM_SCAN_LB_LMAC_IDX] = 0;
        request->v7.suspend_time[IWM_SCAN_LB_LMAC_IDX] = 0;
        request->v7.scan_priority = htole32(IWM_SCAN_PRIORITY_HIGH);
    } else {
        request->v1.active_dwell = activeDwell;
        request->v1.passive_dwell = passiveDwell;
        request->v1.fragmented_dwell = 44;
        request->v1.extended_dwell = 90;
        request->v1.max_out_time = 0;
        request->v1.suspend_time = 0;
        request->v1.scan_priority = htole32(IWM_SCAN_PRIORITY_HIGH);
    }
    request->ooc_priority = htole32(IWM_SCAN_PRIORITY_HIGH);

    const size_t ssidCount = plan.probeSSIDCount();
    if (ssidCount > 20) return EINVAL;
    auto *chanParam = adaptiveDwellV2 ? &request->v8.channel :
        adaptiveDwell ? &request->v7.channel : &request->v1.channel;
    auto *commandData = adaptiveDwellV2 ? &request->v8.data[0] :
        adaptiveDwell ? &request->v7.data[0] : &request->v1.data[0];
    auto *chanCfg = reinterpret_cast<struct iwm_scan_channel_cfg_umac *>(commandData);
    // UMAC selects the SSID entries with bits 0 to 19, unlike LMAC's 1 to 20.
    const uint32_t ssidBitmap = ssidCount ? ((1u << ssidCount) - 1) : 0;
    for (size_t i = 0; i < channelCount; ++i) {
        const auto channel = channels[i];
        // The channel number is the one the regulatory inventory reported.
        // The firmware's own conversion from a frequency would turn channel
        // 144 into 191, so the inventory's number is the one that is sent.
        if (!channel || channel > 255) return EINVAL;
        chanCfg[i].channel_num = static_cast<uint8_t>(channel);
        chanCfg[i].iter_count = 1;
        chanCfg[i].iter_interval = htole16(0);
        chanCfg[i].flags = htole32(plan.probes[i] ? ssidBitmap : 0);
    }
    chanParam->count = static_cast<uint8_t>(channelCount);
    chanParam->flags = 0;

    uint32_t generalFlags = IWM_UMAC_SCAN_GEN_FLAGS_PASS_ALL | IWM_UMAC_SCAN_GEN_FLAGS_ITER_COMPLETE;
    if (ssidCount) generalFlags |= IWM_UMAC_SCAN_GEN_FLAGS_PRE_CONNECT;
    else generalFlags |= IWM_UMAC_SCAN_GEN_FLAGS_PASSIVE;
    if (adaptiveDwell) generalFlags |= IWM_UMAC_SCAN_GEN_FLAGS_ADAPTIVE_DWELL;
    else generalFlags |= IWM_UMAC_SCAN_GEN_FLAGS_EXTENDED_DWELL;
    if (isset(sc->sc_enabled_capa, IWM_UCODE_TLV_CAPA_DS_PARAM_SET_IE_SUPPORT) &&
        isset(sc->sc_enabled_capa, IWM_UCODE_TLV_CAPA_WFA_TPC_REP_IE_SUPPORT))
        generalFlags |= IWM_UMAC_SCAN_GEN_FLAGS_RRM_ENABLED;
    request->general_flags = htole32(generalFlags);

    // "tail" is a global in the Intel headers, so the locals are named after
    // the version they point at.
    auto *tailData = commandData + sizeof(struct iwm_scan_channel_cfg_umac) * capacity;
    auto *tailV2 = reinterpret_cast<struct iwm_scan_req_umac_tail_v2 *>(tailData);
    auto *tailV1 = reinterpret_cast<struct iwm_scan_req_umac_tail_v1 *>(tailData);
    // The scan runs one iteration over the whole channel list.
    tailV2->schedule[0].interval = 0;
    tailV2->schedule[0].iter_count = 1;

    ItlScan::ProbeIdentity identity;
    memcpy(identity.source, plan.source, sizeof(identity.source));
    const auto *bssid = plan.probeBSSID();
    if (bssid) memcpy(identity.bssid, bssid, sizeof(identity.bssid));
    else memset(identity.bssid, 0xff, sizeof(identity.bssid));
    int error;
    if (extendedChan) error = iwm_fill_probe_req(sc, &tailV2->preq, &identity);
    else error = iwm_fill_probe_req_v1(sc, &tailV1->preq, &identity);
    if (error) return error;

    for (size_t i = 0; i < ssidCount; ++i) {
        auto &ssid = tailV1->direct_scan[i];
        const size_t length = plan.probeSSIDLength(i);
        if (length > sizeof(ssid.ssid)) return EINVAL;
        ssid.id = IEEE80211_ELEMID_SSID;
        ssid.len = static_cast<uint8_t>(length);
        if (length) memcpy(ssid.ssid, plan.probeSSID(i), length);
    }
    return 0;
}

int ItlIwm::iwm_native_umac_scan_abort()
{
    iwm_host_cmd command = {};
    command.id = iwm_cmd_id(IWM_SCAN_ABORT_UMAC, IWM_LONG_GROUP, 0);
    return iwm_send_cmd(&com, &command);
}
