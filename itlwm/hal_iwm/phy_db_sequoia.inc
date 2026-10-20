int ItlIwm::iwm_phy_db_set_section(struct iwm_softc *sc, const uint8_t *payload, size_t length)
{
    if (!sc || !payload || length < sizeof(iwm_calib_res_notif_phy_db))
        return EINVAL;
    const auto *notification = reinterpret_cast<const iwm_calib_res_notif_phy_db *>(payload);
    const uint16_t type = le16toh(notification->type);
    const uint16_t size = le16toh(notification->length);
    if (!size || size > length - sizeof(*notification))
        return EINVAL;

    uint16_t group = 0;
    if (type == IWM_PHY_DB_CALIB_CHG_PAPD || type == IWM_PHY_DB_CALIB_CHG_TXP) {
        const size_t minimum = type == IWM_PHY_DB_CALIB_CHG_TXP ? sizeof(iwm_phy_db_chg_txp) : sizeof(uint16_t);
        if (size < minimum)
            return EINVAL;
        // Firmware fields may be unaligned inside the RX buffer.
        memcpy(&group, notification->data, sizeof(group));
        group = le16toh(group);
    }
    auto *entry = iwm_phy_db_get_section(sc, type, group);
    if (!entry)
        return EINVAL;
    auto *replacement = static_cast<uint8_t *>(malloc(size, M_DEVBUF, M_NOWAIT));
    if (!replacement)
        return ENOMEM;
    memcpy(replacement, notification->data, size);
    ::free(entry->data);
    entry->data = replacement;
    entry->size = size;
    return 0;
}

void ItlIwm::iwm_rx_phy_db(struct iwm_softc *sc, const uint8_t *payload, size_t length)
{
    // Only the INIT calibration owner may populate the database. Keep the
    // first failure sticky so a later valid notification cannot hide it.
    if (!nativeCalibrationActive || sc->sc_uc_current != IWM_UCODE_TYPE_INIT ||
        (sc->sc_init_complete & IWM_INIT_COMPLETE) || nativeCalibrationError)
        return;
    const int error = iwm_phy_db_set_section(sc, payload, length);
    if (error)
        nativeCalibrationError = error;
    else
        sc->sc_init_complete |= IWM_CALIB_COMPLETE;
    wakeupOn(&sc->sc_init_complete);
}

void ItlIwm::iwm_phy_db_clear(struct iwm_softc *sc)
{
    const auto clear = [](iwm_phy_db_entry &entry) {
        ::free(entry.data);
        entry.data = nullptr;
        entry.size = 0;
    };
    clear(sc->sc_phy_db.cfg);
    clear(sc->sc_phy_db.calib_nch);
    for (auto &entry : sc->sc_phy_db.calib_ch_group_papd) clear(entry);
    for (auto &entry : sc->sc_phy_db.calib_ch_group_txp) clear(entry);
}

IwmPhyDbSnapshot ItlIwm::iwm_phy_db_snapshot(struct iwm_softc *sc)
{
    const auto &db = sc->sc_phy_db;
    IwmPhyDbSnapshot result;
    result.cfgBytes = db.cfg.data ? db.cfg.size : 0;
    result.nchBytes = db.calib_nch.data ? db.calib_nch.size : 0;
    result.bytes = result.cfgBytes + result.nchBytes;
    for (unsigned i = 0; i < IWM_NUM_PAPD_CH_GROUPS; ++i) {
        const auto &entry = db.calib_ch_group_papd[i];
        if (entry.data && entry.size >= sizeof(uint16_t)) {
            result.papdMask |= 1U << i;
            result.bytes += entry.size;
        }
    }
    for (unsigned i = 0; i < IWM_NUM_TXP_CH_GROUPS; ++i) {
        const auto &entry = db.calib_ch_group_txp[i];
        if (entry.data && entry.size >= sizeof(iwm_phy_db_chg_txp)) {
            result.txpMask |= 1U << i;
            result.bytes += entry.size;
        }
    }
    for (unsigned channel = 0; channel < nitems(sc->sc_ic.ic_channels); ++channel) {
        const auto &entry = sc->sc_ic.ic_channels[channel];
        if (!entry.ic_freq || !entry.ic_flags) continue;
        ++result.channels;
        const unsigned papd = iwm_channel_id_to_papd(channel);
        const unsigned txp = iwm_channel_id_to_txp(sc, channel);
        const bool covered = papd < IWM_NUM_PAPD_CH_GROUPS && txp < IWM_NUM_TXP_CH_GROUPS &&
            (result.papdMask & (1U << papd)) && (result.txpMask & (1U << txp));
        if (covered)
            ++result.coveredChannels;
        else if (result.firstUncoveredChannel == UINT32_MAX)
            result.firstUncoveredChannel = channel;
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
        // Probe build: the group the driver picks for every channel, so the
        // group boundaries the firmware reports can be checked against it.
        if (isHardwareTrial()) {
            const size_t used = strlen(nativeNvmDiagnostics.channelGroups);
            if (used + 16 < sizeof(nativeNvmDiagnostics.channelGroups))
                snprintf(nativeNvmDiagnostics.channelGroups + used,
                    sizeof(nativeNvmDiagnostics.channelGroups) - used, "%u.%u.%u.%c ",
                    channel, papd, txp, covered ? '1' : '0');
        }
#endif
    }
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    // Probe build: the maximum channel index each TXP group reports, which
    // is where the firmware says the group ends.
    if (isHardwareTrial()) {
        for (unsigned i = 0; i < IWM_NUM_TXP_CH_GROUPS; ++i) {
            const auto &entry = db.calib_ch_group_txp[i];
            if (!entry.data || entry.size < sizeof(iwm_phy_db_chg_txp)) continue;
            const auto *txp = reinterpret_cast<const iwm_phy_db_chg_txp *>(entry.data);
            const size_t used = strlen(nativeNvmDiagnostics.txpGroupMaxIndex);
            if (used + 8 < sizeof(nativeNvmDiagnostics.txpGroupMaxIndex))
                snprintf(nativeNvmDiagnostics.txpGroupMaxIndex + used,
                    sizeof(nativeNvmDiagnostics.txpGroupMaxIndex) - used, "%u:%u ",
                    i, le16toh(txp->max_channel_idx));
        }
    }
#endif
    return result;
}
