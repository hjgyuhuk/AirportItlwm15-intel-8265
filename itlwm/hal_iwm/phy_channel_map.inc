// Legacy BSD PHY DB mapping shared by the HAL and its production-code tests.
int ItlIwm::iwm_is_valid_channel(uint16_t ch_id)
{
    if (ch_id <= 14 ||
        (36 <= ch_id && ch_id <= 64 && ch_id % 4 == 0) ||
        (100 <= ch_id && ch_id <= 140 && ch_id % 4 == 0) ||
        (145 <= ch_id && ch_id <= 165 && ch_id % 4 == 1))
        return 1;
    return 0;
}

uint8_t ItlIwm::iwm_ch_id_to_ch_index(uint16_t ch_id)
{
    if (!iwm_is_valid_channel(ch_id))
        return 0xff;
    if (ch_id <= 14)
        return ch_id - 1;
    if (ch_id <= 64)
        return (ch_id + 20) / 4;
    if (ch_id <= 140)
        return (ch_id - 12) / 4;
    return (ch_id - 13) / 4;
}

uint16_t ItlIwm::iwm_channel_id_to_papd(uint16_t ch_id)
{
    if (!iwm_is_valid_channel(ch_id))
        return 0xff;
    if (1 <= ch_id && ch_id <= 14)
        return 0;
    if (36 <= ch_id && ch_id <= 64)
        return 1;
    if (100 <= ch_id && ch_id <= 140)
        return 2;
    return 3;
}

uint16_t ItlIwm::iwm_channel_id_to_txp(struct iwm_softc *sc, uint16_t ch_id)
{
    struct iwm_phy_db *phy_db = &sc->sc_phy_db;
    struct iwm_phy_db_chg_txp *txp_chg;
    int i;
    uint8_t ch_index = iwm_ch_id_to_ch_index(ch_id);
    if (ch_index == 0xff)
        return 0xff;
    for (i = 0; i < IWM_NUM_TXP_CH_GROUPS; i++) {
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
        if (phy_db->calib_ch_group_txp[i].size < sizeof(*txp_chg))
            return 0xff;
#endif
        txp_chg = (struct iwm_phy_db_chg_txp *)phy_db->calib_ch_group_txp[i].data;
        if (!txp_chg)
            return 0xff;
        /* First group whose maximum channel index covers the requested channel. */
        if (le16toh(txp_chg->max_channel_idx) >= ch_index)
            return i;
    }
    return 0xff;
}
