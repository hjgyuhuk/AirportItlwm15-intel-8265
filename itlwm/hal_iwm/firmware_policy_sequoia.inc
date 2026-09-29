bool ItlIwm::iwm_firmware_trial_command_allowed(iwm_softc *sc, const iwm_host_cmd *command)
{
    const auto stage = nativeFirmwareDiagnostics.stage;
    if (stage == IwmFirmwareStage::Scan)
        return iwm_scan_trial_command_allowed(sc, command);
    const bool configuration = stage == IwmFirmwareStage::Configuration;
    if (!command || !command->data[0] || (command->flags & IWM_CMD_ASYNC) ||
        (configuration ? (sc->sc_uc_current != IWM_UCODE_TYPE_REGULAR || nativeCalibrationActive ||
                          nativeFirmwareDiagnostics.calibrationResult || nativeFirmwareDiagnostics.regularResult) :
                         (sc->sc_uc_current != IWM_UCODE_TYPE_INIT ||
                          (stage != IwmFirmwareStage::Nvm && stage != IwmFirmwareStage::Calibration))))
        return false;
    if (command->id == IWM_PHY_DB_CMD) {
        if (!configuration || command->flags || command->len[0] != sizeof(iwm_phy_db_cmd) ||
            !command->data[1] || !command->len[1] ||
            command->len[1] > IWM_MAX_CMD_PAYLOAD_SIZE - sizeof(iwm_phy_db_cmd)) return false;
        const auto *request = static_cast<const iwm_phy_db_cmd *>(command->data[0]);
        const auto type = le16toh(request->type);
        if (le16toh(request->length) != command->len[1]) return false;
        uint16_t group = 0;
        if (type == IWM_PHY_DB_CALIB_CHG_PAPD || type == IWM_PHY_DB_CALIB_CHG_TXP) {
            const size_t minimum = type == IWM_PHY_DB_CALIB_CHG_TXP ? sizeof(iwm_phy_db_chg_txp) : sizeof(uint16_t);
            if (command->len[1] < minimum) return false;
            memcpy(&group, command->data[1], sizeof(group));
            group = le16toh(group);
        } else if (type != IWM_PHY_DB_CFG && type != IWM_PHY_DB_CALIB_NCH) return false;
        const auto *entry = iwm_phy_db_get_section(sc, type, group);
        return entry && entry->size == command->len[1] && entry->data == command->data[1] &&
            nativeFirmwareDiagnostics.replayLastType == type && nativeFirmwareDiagnostics.replayLastGroup == group;
    }
    if (command->len[1]) return false;
    if (command->id == IWM_NVM_ACCESS_CMD) {
        if (stage != IwmFirmwareStage::Nvm || command->len[0] != sizeof(iwm_nvm_access_cmd) ||
            command->flags != (IWM_CMD_WANT_RESP | IWM_CMD_SEND_IN_RFKILL)) return false;
        const auto *request = static_cast<const iwm_nvm_access_cmd *>(command->data[0]);
        return request->op_code == IWM_NVM_READ_OPCODE && request->target == 0 &&
            le16toh(request->length) && le16toh(request->length) <= IWM_NVM_DEFAULT_CHUNK_SIZE &&
            uint32_t(le16toh(request->offset)) + le16toh(request->length) <= UINT16_MAX;
    }
    if (command->flags) return false;
    if (command->id == IWM_BT_CONFIG && command->len[0] == sizeof(iwm_bt_coex_cmd)) {
        const auto *request = static_cast<const iwm_bt_coex_cmd *>(command->data[0]);
        const uint32_t base = IWM_BT_COEX_HIGH_BAND_RET | IWM_BT_COEX_SYNC2SCO_ENABLED;
        return request->mode == htole32(IWM_BT_COEX_NW) &&
            (request->enabled_modules == htole32(base) ||
             request->enabled_modules == htole32(base | IWM_BT_COEX_MPLUT_ENABLED));
    }
    if (stage != IwmFirmwareStage::Calibration && !configuration) return false;
    if (command->id == IWM_REPLY_SF_CFG_CMD && command->len[0] == sizeof(iwm_sf_cfg_cmd))
        return !configuration && static_cast<const iwm_sf_cfg_cmd *>(command->data[0])->state == htole32(IWM_SF_INIT_OFF);
    if (command->id == IWM_TX_ANT_CONFIGURATION_CMD && command->len[0] == sizeof(iwm_tx_ant_cfg_cmd))
        return iwm_fw_valid_tx_ant(sc) &&
            static_cast<const iwm_tx_ant_cfg_cmd *>(command->data[0])->valid == htole32(iwm_fw_valid_tx_ant(sc));
    if (command->id == IWM_PHY_CONFIGURATION_CMD && command->len[0] == sizeof(iwm_phy_cfg_cmd)) {
        const auto *request = static_cast<const iwm_phy_cfg_cmd *>(command->data[0]);
        const auto &expected = sc->sc_default_calib[configuration ? IWM_UCODE_TYPE_REGULAR : IWM_UCODE_TYPE_INIT];
        return request->phy_cfg == htole32(sc->sc_fw_phy_config) &&
            request->calib_control.event_trigger == expected.event_trigger &&
            request->calib_control.flow_trigger == expected.flow_trigger;
    }
    return false;
}
