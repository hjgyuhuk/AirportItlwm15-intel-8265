int ItlIwm::iwm_replay_phy_db(struct iwm_softc *sc)
{
    auto &result = nativeFirmwareDiagnostics;
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread() ||
        !nativeFirmwareTrial || nativeCalibrationActive ||
        result.stage != IwmFirmwareStage::Configuration || result.replayLastType != UINT32_MAX ||
        sc->sc_uc_current != IWM_UCODE_TYPE_REGULAR || result.calibrationResult || result.regularResult)
        return EPERM;

    const auto current = iwm_phy_db_snapshot(sc);
    const auto &saved = result.phy;
    if (!current.complete() || current.cfgBytes != saved.cfgBytes || current.nchBytes != saved.nchBytes ||
        current.bytes != saved.bytes || current.papdMask != saved.papdMask || current.txpMask != saved.txpMask ||
        current.channels != saved.channels || current.coveredChannels != saved.coveredChannels)
        return ENODATA;

    struct Section { uint16_t type, group; iwm_phy_db_entry *entry; } sections[2 + IWM_NUM_PAPD_CH_GROUPS + IWM_NUM_TXP_CH_GROUPS];
    unsigned count = 0;
    // Check the entire frozen database before the first DMA submission. The
    // transport copies both command fragments into its own ACK-owned buffer.
    const uint16_t types[] = {IWM_PHY_DB_CFG, IWM_PHY_DB_CALIB_NCH, IWM_PHY_DB_CALIB_CHG_PAPD, IWM_PHY_DB_CALIB_CHG_TXP};
    for (uint16_t type : types) {
        const unsigned groups = type == IWM_PHY_DB_CALIB_CHG_PAPD ? IWM_NUM_PAPD_CH_GROUPS :
            type == IWM_PHY_DB_CALIB_CHG_TXP ? IWM_NUM_TXP_CH_GROUPS : 1;
        for (unsigned group = 0; group < groups; ++group) {
            auto *entry = iwm_phy_db_get_section(sc, type, group);
            if (!entry || bool(entry->size) != bool(entry->data)) return EINVAL;
            if (!entry->size) {
                if (groups == 1) return ENODATA;
                continue;
            }
            if (entry->size > IWM_MAX_CMD_PAYLOAD_SIZE - sizeof(iwm_phy_db_cmd)) return E2BIG;
            if (groups != 1) {
                const size_t minimum = type == IWM_PHY_DB_CALIB_CHG_TXP ? sizeof(iwm_phy_db_chg_txp) : sizeof(uint16_t);
                if (entry->size < minimum) return EINVAL;
                uint16_t recorded;
                memcpy(&recorded, entry->data, sizeof(recorded));
                if (le16toh(recorded) != group) return EINVAL;
            }
            sections[count++] = {type, static_cast<uint16_t>(group), entry};
        }
    }
    for (unsigned index = 0; index < count; ++index) {
        const auto &section = sections[index];
        iwm_phy_db_cmd header = {};
        header.type = htole16(section.type);
        header.length = htole16(section.entry->size);
        iwm_host_cmd command = {};
        command.id = IWM_PHY_DB_CMD;
        command.data[0] = &header;
        command.len[0] = sizeof(header);
        command.data[1] = section.entry->data;
        command.len[1] = section.entry->size;
        result.replayLastType = section.type;
        result.replayLastGroup = section.group;
        const int error = iwm_send_cmd(sc, &command);
        if (error) return error;
        ++result.replayCommands;
        result.replayBytes += section.entry->size;
        if (section.type == IWM_PHY_DB_CALIB_CHG_PAPD) result.replayPapdMask |= 1U << section.group;
        if (section.type == IWM_PHY_DB_CALIB_CHG_TXP) result.replayTxpMask |= 1U << section.group;
    }
    return 0;
}
