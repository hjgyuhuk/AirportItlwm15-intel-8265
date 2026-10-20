bool ItlIwm::iwm_scan_trial_command_allowed(iwm_softc *sc, const iwm_host_cmd *command)
{
    const auto &result = nativeFirmwareDiagnostics;
    if (!nativeFirmwareTrial || nativeNet80211Attached || result.stage != IwmFirmwareStage::Scan ||
        result.configurationResult || result.scanPreflightResult || nativeCalibrationActive ||
        sc->sc_uc_current != IWM_UCODE_TYPE_REGULAR || !command || command->len[1] ||
        !command->data[0]) return false;
    const auto *phase = nativeNvmDiagnostics.phase;
    if (!strcmp(phase, "scan-dqa") && command->id == iwm_cmd_id(IWM_DQA_ENABLE_CMD, IWM_DATA_PATH_GROUP, 0)) {
        const iwm_dqa_enable_cmd expected = {htole32(IWM_DQA_CMD_QUEUE)};
        return !command->flags && command->len[0] == sizeof(expected) &&
            !memcmp(command->data[0], &expected, sizeof(expected));
    }
    if (!strcmp(phase, "scan-aux") && result.scanDqaResult == 0) {
        if (command->id == IWM_SCD_QUEUE_CFG) {
            iwm_scd_txq_cfg_cmd expected = {};
            expected.sta_id = IWM_AUX_STA_ID;
            expected.tid = IWM_MAX_TID_COUNT;
            expected.scd_queue = IWM_DQA_AUX_QUEUE;
            expected.enable = IWM_SCD_CFG_ENABLE_QUEUE;
            expected.tx_fifo = IWM_TX_FIFO_MCAST;
            expected.window = IWM_FRAME_LIMIT;
            return !command->flags && command->len[0] == sizeof(expected) &&
                !memcmp(command->data[0], &expected, sizeof(expected));
        }
        if (command->id == IWM_ADD_STA) {
            // The station type capability decides the layout of the command.
            // The 8000 family reports it and takes the larger command, the
            // 7000 family does not and takes the v7 one. The driver already
            // picks the same way, so the expectation follows it.
            if (isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_STA_TYPE)) {
                struct iwm_add_sta_cmd expected = {};
                expected.tid_disable_tx = htole16(0xffff);
                expected.mac_id_n_color =
                    htole32(IWM_FW_CMD_ID_AND_COLOR(IWM_MAC_INDEX_AUX, 0));
                expected.sta_id = IWM_AUX_STA_ID;
                expected.tfd_queue_msk = htole32(1 << IWM_DQA_AUX_QUEUE);
                expected.station_type = IWM_STA_AUX_ACTIVITY;
                return command->flags == IWM_CMD_WANT_RESP &&
                    command->len[0] == sizeof(expected) &&
                    !memcmp(command->data[0], &expected, sizeof(expected));
            }
            iwm_add_sta_cmd_v7 expected = {};
            expected.tid_disable_tx = htole16(0xffff);
            expected.mac_id_n_color =
                htole32(IWM_FW_CMD_ID_AND_COLOR(IWM_MAC_INDEX_AUX, 0));
            expected.sta_id = IWM_AUX_STA_ID;
            expected.tfd_queue_msk = htole32(1 << IWM_DQA_AUX_QUEUE);
            return command->flags == IWM_CMD_WANT_RESP &&
                command->len[0] == sizeof(expected) &&
                !memcmp(command->data[0], &expected, sizeof(expected));
        }
    }
    if (strcmp(phase, "scan-submit") || result.scanAuxResult != 0 || command->flags)
        return false;
    if (nativeUpstreamUmacScan)
        return command->id == iwm_cmd_id(IWM_SCAN_REQ_UMAC, IWM_LONG_GROUP, 0) &&
            command->data[0] != nullptr && command->len[0] != 0;
    if (nativeUpstreamScan)
        return command->id == IWM_SCAN_OFFLOAD_REQUEST_CMD &&
            command->data[0] != nullptr && command->len[0] != 0;
    if (command->id != IWM_SCAN_OFFLOAD_REQUEST_CMD)
        return false;
    return nativePassiveRequest && command->data[0] == nativePassiveRequest &&
        command->len[0] == nativePassiveRequestBytes;
}

int ItlIwm::iwm_passive_scan_trial(iwm_softc *sc)
{
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread() || !nativeFirmwareTrial ||
        nativeNet80211Attached || nativeFirmwareDiagnostics.stage != IwmFirmwareStage::Scan ||
        nativeFirmwareDiagnostics.configurationResult ||
        nativeFirmwareDiagnostics.scanPreflightResult != UINT32_MAX) return EPERM;
    auto &result = nativeFirmwareDiagnostics;
    auto &phase = nativeNvmDiagnostics.phase;
    IwmTrial::PassivePlan plan;
    phase = "scan-preflight";
    // This first radio trial was written for firmware 17's LMAC + DQA + v7
    // station ABI. The 8000 family runs firmware 36 and reports two more
    // capabilities than the 7000 family does: station type assignment and
    // UMAC scan. Both are extra capabilities rather than a different
    // station format, so neither blocks the trial. Never send it to an MQ RX
    // part.
    const bool familyOk = sc->sc_device_family != IWM_DEVICE_FAMILY_9000;
    const bool mqrxOk = !sc->sc_mqrx_supported;
    const bool dqaOk = isset(sc->sc_enabled_capa, IWM_UCODE_TLV_CAPA_DQA_SUPPORT);
    const bool umacOk = true;
    const bool staTypeOk = true;
    const bool cmdqOk = sc->cmdqid == IWM_DQA_CMD_QUEUE;
    const bool phyOk = result.phy.complete();
    const bool transportOk = commandTransportReady;
    const bool rfkillOk = !(sc->sc_flags & IWM_FLAG_RFKILL);
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (isHardwareTrial()) {
        auto &diagnostics = nativeNvmDiagnostics;
        diagnostics.scanPreflightMask = (familyOk ? 0u : 1u) | (mqrxOk ? 0u : 2u) |
            (dqaOk ? 0u : 4u) | (umacOk ? 0u : 8u) | (staTypeOk ? 0u : 16u) |
            (cmdqOk ? 0u : 32u) | (phyOk ? 0u : 64u) | (transportOk ? 0u : 128u) |
            (rfkillOk ? 0u : 256u);
        for (size_t i = 0; i < nitems(sc->sc_ucode_api); ++i)
            snprintf(diagnostics.ucodeApiBits + i * 2, 3, "%02x", sc->sc_ucode_api[i]);
        for (size_t i = 0; i < nitems(sc->sc_enabled_capa); ++i)
            snprintf(diagnostics.enabledCapaBits + i * 2, 3, "%02x", sc->sc_enabled_capa[i]);
    }
#endif
    if (!familyOk || !mqrxOk || !dqaOk || !umacOk || !staTypeOk || !cmdqOk || !phyOk ||
        !transportOk || !rfkillOk) {
        result.scanPreflightResult = ENOTSUP;
        return ENOTSUP;
    }
    plan.capacity = sc->sc_capa_n_scan_channels;
    plan.txAnt = iwm_fw_valid_tx_ant(sc);
    plan.rxAnt = iwm_fw_valid_rx_ant(sc);
    memcpy(plan.source, sc->sc_nvm.hw_addr, sizeof(plan.source));
    for (size_t i = 1; i < nitems(sc->sc_ic.ic_channels); ++i) {
        const auto &channel = sc->sc_ic.ic_channels[i];
        if (!channel.ic_freq || !channel.ic_flags) continue;
        if (plan.count == nitems(plan.channels) || !iwm_channel_is_valid(sc, i)) {
            result.scanPreflightResult = EINVAL;
            return EINVAL;
        }
        // Only the 7000 family has to leave channel 144 out; its inherited
        // PHY DB index table cannot calibrate it.
        if (sc->sc_device_family == IWM_DEVICE_FAMILY_7000 && i == 144) continue;
        plan.channels[plan.count++] = static_cast<uint16_t>(i);
    }
    const size_t bytes = IwmTrial::passiveRequestSize(plan);
    // The plan may only use channels the radio's regulatory section offers.
    // How many of them carry a PAPD and TXP calibration group is a property
    // of the firmware's grouping, so it is reported but not required here.
    if (!bytes || !plan.count) {
        result.scanPreflightResult = EINVAL;
        return EINVAL;
    }
    auto *request = malloc(bytes, M_DEVBUF, M_NOWAIT);
    if (!request) { result.scanPreflightResult = ENOMEM; return ENOMEM; }
    if (!IwmTrial::buildPassiveRequest(plan, request, bytes)) {
        ::free(request);
        result.scanPreflightResult = EINVAL;
        return EINVAL;
    }
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (isHardwareTrial()) {
        const auto *built = static_cast<const iwm_scan_req_lmac *>(request);
        nativeNvmDiagnostics.scanRequestFlags = built->flags;
        nativeNvmDiagnostics.scanRequestChannels = built->n_channels;
        nativeNvmDiagnostics.scanCapaChannels = plan.capacity;
    }
#endif
    result.scanPreflightResult = 0;
    phase = "scan-dqa";
    int error = iwm_send_dqa_cmd(sc);
    result.scanDqaResult = error;
    if (!error) {
        phase = "scan-aux";
        const uint64_t epoch = commandEpoch;
        if (!iwm_nic_lock(sc)) error = EBUSY;
        else {
            error = iwm_add_aux_sta(sc);
            // A reset clears the NIC lock count while the command sleeps.
            if (commandEpoch == epoch) iwm_nic_unlock(sc);
            else error = ENXIO;
        }
        result.scanAuxResult = error;
    }
    if (!error) {
        phase = "scan-submit";
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
        if (nativeUpstreamScan) {
            // Probe build: let the driver build the scan request itself, the
            // way it does for the regular path, so the firmware sees a
            // request it already accepts on this card. The trial does not
            // attach to net80211, so the rate sets and the station address
            // are supplied here.
            auto *ic = &sc->sc_ic;
            static const uint8_t rates24[] = {0x02, 0x04, 0x0b, 0x0c, 0x12,
                0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
            static const uint8_t rates5[] = {0x0c, 0x12, 0x18, 0x24, 0x30,
                0x42, 0x48, 0x60, 0x6c};
            memcpy(ic->ic_sup_rates[IEEE80211_MODE_11G].rs_rates, rates24,
                sizeof(rates24));
            ic->ic_sup_rates[IEEE80211_MODE_11G].rs_nrates = sizeof(rates24);
            memcpy(ic->ic_sup_rates[IEEE80211_MODE_11A].rs_rates, rates5,
                sizeof(rates5));
            ic->ic_sup_rates[IEEE80211_MODE_11A].rs_nrates = sizeof(rates5);
            IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->sc_nvm.hw_addr);
            error = nativeUpstreamUmacScan ? iwm_umac_scan(sc, 0, nullptr)
                                           : iwm_lmac_scan(sc, 0, nullptr);
            result.scanSubmitResult = error;
            result.scanWaitResult = error;
            if (!error) {
                phase = "scan-wait";
                error = iwm_wait_status(&nativePassiveDone, 1, &nativePassiveDone,
                    ItlScan::IwmPassiveTrial::WaitSeconds);
                result.scanWaitResult = error;
            }
            goto scan_done;
        }
#endif
        const auto token = nativePassiveScan.begin(plan.channels, plan.count);
        if (!token) error = EBUSY;
        else {
            iwm_host_cmd command = {};
            command.id = IWM_SCAN_OFFLOAD_REQUEST_CMD;
            command.len[0] = static_cast<uint16_t>(bytes);
            command.data[0] = request;
            nativePassiveRequest = request;
            nativePassiveRequestBytes = bytes;
            error = iwm_send_cmd(sc, &command);
            nativePassiveRequest = nullptr;
            nativePassiveRequestBytes = 0;
            nativePassiveScan.submitted(token, error ? ItlScan::Submission::Indeterminate : ItlScan::Submission::Accepted);
        }
        result.scanSubmitResult = error;
        if (!error) {
            phase = "scan-wait";
            error = iwm_wait_status(&nativePassiveDone, 1, &nativePassiveDone, ItlScan::IwmPassiveTrial::WaitSeconds);
            if (error == ETIMEDOUT) nativePassiveScan.timeout(token);
            ItlScan::Completion completion;
            if (!error && (!nativePassiveScan.completion(completion) || !completion.accepted ||
                completion.outcome != ItlScan::Outcome::Success || !nativePassiveScan.completeCoverage())) error = EIO;
            result.scanWaitResult = error;
        }
    }
scan_done:
    ::free(request);
    // The owning orchestrator always resets hardware, even on timeout. An
    // abort ACK alone cannot release this scan's DMA/RX ownership.
    return error;
}

void ItlIwm::iwm_scan_trial_rx(uint16_t code, const void *payload, size_t size)
{
    if (nativeFirmwareDiagnostics.stage != IwmFirmwareStage::Scan) return;
    if (code == IWM_REPLY_RX_PHY_CMD) nativePassiveScan.rxPhy(payload, size);
    else if (code == IWM_REPLY_RX_MPDU_CMD) nativePassiveScan.rxMpdu(payload, size);
    else if (code == IWM_SCAN_OFFLOAD_COMPLETE || code == IWM_SCAN_ITERATION_COMPLETE) {
        if (nativePassiveScan.notification(code, payload, size)) {
            nativePassiveDone = 1;
            getMainCommandGate()->commandWakeup(&nativePassiveDone, false);
        }
    }
}
