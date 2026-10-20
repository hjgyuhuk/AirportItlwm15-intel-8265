int ItlIwm::iwm_run_firmware_trial(struct iwm_softc *sc)
{
    // Probe build: the upstream trial was gated to the 7000 family, which runs
    // firmware 17 (iwm-7265-17). The 8265 is an 8000-family part running
    // firmware 36, so the firmware dialogue below has to be exercised against
    // it to learn where the two generations actually diverge. The 9000 family
    // (MQ RX parts) keeps its own station ABI and stays excluded.
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread() ||
        !nativeFirmwareTrial || nativeNvmTrial || nativeNet80211Attached ||
        nativeFirmwareDiagnostics.stage != IwmFirmwareStage::Resources ||
        sc->sc_device_family == IWM_DEVICE_FAMILY_9000)
        return EPERM;
    FirmwareLease operation(this);
    if (!operation) return EBUSY;
    auto &result = nativeFirmwareDiagnostics;
    auto &detail = nativeNvmDiagnostics;
    int error;
    nativeHardwareStarted = true;

    result.stage = IwmFirmwareStage::Nvm;
    detail.phase = "start-hardware";
    error = iwm_start_hw(sc);
    if (!error) error = iwm_run_init_mvm_ucode(sc, 1);
    result.nvmResult = error;
    result.nvmAliveBytes = detail.aliveBytes;
    result.nvmAliveStatus = detail.aliveStatus;
    if (error) goto out;
    iwm_stop_device(sc);

    result.stage = IwmFirmwareStage::Calibration;
    // The legacy BSD/Intel PHY DB map has no channel 144 mapping. Remove
    // only that known unsupported channel from this unattached trial's usable
    // inventory; retain every other channel so missing calibration still fails.
    for (unsigned channel = 0; channel < nitems(sc->sc_ic.ic_channels); ++channel) {
        auto &entry = sc->sc_ic.ic_channels[channel];
        if (!entry.ic_freq || !entry.ic_flags) continue;
        ++result.nvmChannels;
        if (channel == 144 && !iwm_is_valid_channel(channel)) {
            memset(&entry, 0, sizeof(entry));
            ++result.excludedChannels;
            result.firstExcludedChannel = channel;
        }
    }
    detail.phase = "start-hardware";
    detail.recordAlive(nullptr, 0);
    error = iwm_start_hw(sc);
    if (!error) error = iwm_run_init_mvm_ucode(sc, 0);
    result.calibrationAliveBytes = detail.aliveBytes;
    result.calibrationAliveStatus = detail.aliveStatus;
    result.calibrationFlags = sc->sc_init_complete;
    result.phy = iwm_phy_db_snapshot(sc);
    if (!error) {
        detail.phase = "validate-phy-db";
        if (!result.phy.complete()) error = ENODATA;
    }
    result.calibrationResult = error;
    if (error) goto out;
    iwm_stop_device(sc);

    // Load regular firmware before the separately bounded setup/scan phases.
    result.stage = IwmFirmwareStage::Regular;
    detail.phase = "start-hardware";
    detail.recordAlive(nullptr, 0);
    error = iwm_start_hw(sc);
    if (!error && (sc->sc_flags & IWM_FLAG_RFKILL)) error = EPERM;
    if (!error) error = iwm_load_ucode_wait_alive(sc, IWM_UCODE_TYPE_REGULAR);
    result.regularResult = error;
    result.regularAliveBytes = detail.aliveBytes;
    result.regularAliveStatus = detail.aliveStatus;
    if (error) goto out;

    result.stage = IwmFirmwareStage::Configuration;
    detail.phase = "regular-antennas";
    error = iwm_send_tx_ant_cfg(sc, iwm_fw_valid_tx_ant(sc));
    result.antennaResult = error;
    if (!error) {
        detail.phase = "replay-phy-db";
        error = iwm_replay_phy_db(sc);
        result.replayResult = error;
    }
    if (!error) {
        detail.phase = "regular-phy-config";
        error = iwm_send_phy_cfg_cmd(sc);
        result.phyConfigResult = error;
    }
    if (!error) {
        detail.phase = "regular-bt-config";
        error = iwm_send_bt_init_conf(sc);
        result.btResult = error;
    }
    result.configurationResult = error;
    if (!error) {
        result.stage = IwmFirmwareStage::Scan;
        error = iwm_passive_scan_trial(sc);
        result.scanResult = error;
    }
    if (!error) {
        result.stage = IwmFirmwareStage::Complete;
        detail.phase = "complete";
    }
out:
    iwm_stop_device(sc);
    nativePassiveScan.hardwareStopped();
    return error;
}
