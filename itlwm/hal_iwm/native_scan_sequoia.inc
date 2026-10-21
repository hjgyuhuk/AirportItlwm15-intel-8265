int ItlIwm::iwm_native_scan(const SequoiaWCL::ScanPlan &plan, const ItlScan::NativeSink &sink)
{
    auto *sc = &com;
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread()) return EWOULDBLOCK;
    FirmwareLease operation(this);
    if (!operation) return EBUSY;
    // r225: the associated plan runs bgscan-style on the live connection. The
    // unassociated plan still requires IEEE80211_S_INIT; the Airport layer
    // admits S_SCAN requests but the worker only ever saw INIT on the r224
    // boot, and that pairing is unchanged here.
    const bool associated = plan.associatedScan();
    // Probe build: the 7000-family restriction is lifted. The 8265 is an
    // 8000-family part on firmware 36, and the LMAC scan request this path
    // builds was reverse engineered against firmware 17. Every other
    // precondition below is unchanged.
    if (!plan.valid() || !sink.valid() || nativeScanSink.valid() || isHardwareTrial() ||
        !nativeNet80211Attached || !commandTransportReady ||
        sc->sc_device_family == IWM_DEVICE_FAMILY_9000 ||
        sc->sc_mqrx_supported || sc->sc_uc_current != IWM_UCODE_TYPE_REGULAR ||
        sc->cmdqid != IWM_DQA_CMD_QUEUE ||
        !(sc->sc_ic.ic_ac.ac_if.if_flags & IFF_RUNNING) ||
        (!associated && sc->sc_ic.ic_state != IEEE80211_S_INIT) ||
        (associated && (sc->sc_ic.ic_state != IEEE80211_S_RUN || !sc->sc_ic.ic_bss)) ||
        (sc->sc_flags & (IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN | IWM_FLAG_RFKILL)))
        return EOPNOTSUPP;
    // A channel the radio reports is not automatically usable: one without a
    // calibration group cannot be scanned, and the request builder refuses
    // such a channel outright. Drop those channels here instead of failing the
    // whole scan over one of them.
    nativeScanUsablePlan = plan;
    {
        size_t kept = 0;
        for (size_t i = 0; i < plan.channelCount; ++i) {
            const auto channel = plan.channels[i];
            if (!iwm_channel_is_valid(sc, channel)) continue;
            nativeScanUsablePlan.channels[kept] = channel;
            nativeScanUsablePlan.probes[kept] = plan.probes[i];
            ++kept;
        }
        nativeScanUsablePlan.channelCount = kept;
    }
    const auto &usable = nativeScanUsablePlan;
    nativeScanUsableCount = 0;
    for (size_t i = 0; i < usable.channelCount && i < 64; ++i)
        nativeScanUsableList[nativeScanUsableCount++] = usable.channels[i];
    IwmTrial::PassivePlan wire;
    wire.count = usable.channelCount;
    // Only the 7000 family has to leave channel 144 out; its inherited PHY
    // DB index table cannot calibrate it.
    wire.skipChannel144 = sc->sc_device_family == IWM_DEVICE_FAMILY_7000;
    wire.capacity = sc->sc_capa_n_scan_channels;
    wire.txAnt = iwm_fw_valid_tx_ant(sc);
    wire.rxAnt = iwm_fw_valid_rx_ant(sc);
    // Probe build: record what was asked for and where the request gave up.
    nativeScanRawTerminal = 0;
    nativeScanRawIteration = 0;
    nativeScanRawStatus = 0;
    nativeScanFailStage = 0;
    nativeScanFailChannel = 0;
    nativeScanChannelCount = 0;
    for (size_t i = 0; i < wire.count && i < 64; ++i)
        nativeScanChannelList[nativeScanChannelCount++] = plan.channels[i];
    if (wire.count > nitems(wire.channels)) { nativeScanFailStage = 1; return EINVAL; }
    for (size_t i = 0; i < wire.count; ++i) {
        const auto channel = usable.channels[i];
        if (channel >= nitems(sc->sc_ic.ic_channels) || !iwm_channel_is_valid(sc, channel) ||
            !sc->sc_ic.ic_channels[channel].ic_freq || !isset(sc->sc_ic.ic_chan_avail, channel)) {
            nativeScanFailStage = 2;
            nativeScanFailChannel = channel;
            return EINVAL;
        }
        if (usable.probes[i] && (sc->sc_ic.ic_channels[channel].ic_flags &
            (IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_DFS))) { nativeScanFailStage = 3; return EOPNOTSUPP; }
        wire.channels[i] = channel;
    }
    memcpy(wire.source, usable.source, sizeof(wire.source));
    // Probe build: the values every size check looks at.
    nativeScanWireCount = wire.count;
    nativeScanWireCapacity = wire.capacity;
    nativeScanTxAnt = wire.txAnt;
    nativeScanRxAnt = wire.rxAnt;
    nativeScanSourceValid = ItlScan::validUnicast(wire.source) ? 1 : 0;
    // The firmware says which scan request it wants: it reports the UMAC
    // scan capability, and the driver then sends a UMAC request. The request
    // this tree builds was written for the 7000 family, and the 8265's
    // firmware 36 resets the radio when it sees it, so the native path
    // follows the firmware's choice.
    const bool umac = isset(sc->sc_enabled_capa, IWM_UCODE_TLV_CAPA_UMAC_SCAN);
    nativeScanUmac = umac ? 1 : 0;
    const size_t size = umac ? iwm_native_umac_scan_size(wire.capacity) :
                               IwmTrial::passiveRequestSize(wire);
    if (!size || size > IWM_MAX_CMD_PAYLOAD_SIZE) { nativeScanFailStage = 4; return EINVAL; }
    void *request = malloc(size, M_DEVBUF, M_NOWAIT);
    if (!request) { nativeScanFailStage = 5; return ENOMEM; }
    const bool fragmented = associated &&
        isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_FRAGMENTED_SCAN);
    nativeCacheDiagnostic.assocScanFragmented = fragmented ? 1 : 0;
    const bool built = umac ?
        iwm_native_umac_scan_request(usable, usable.channels, usable.channelCount,
                                      wire.capacity, request, size) == 0 :
        IwmTrial::buildNativeScanRequest(usable, wire.capacity, wire.txAnt, wire.rxAnt,
                                         fragmented, request, size);
    if (!built) {
        ::free(request);
        nativeScanFailStage = 6;
        return EINVAL;
    }
    nativeScanSink = sink;
    nativeScanPlan = &nativeScanUsablePlan;
    nativeScanCachePhy = {};
    nativeScanDone = nativeScanCancel = 0;
    // The associated bgscan must never set IWM_FLAG_SCANNING: the legacy
    // rx.cpp iteration-complete case treats that flag as its own scan and
    // runs iwm_endscan, which would call ieee80211_end_scan on a live BSS.
    // BGSCAN is the semantically correct flag and no native-path code
    // inspects either flag while the scan is running.
    sc->sc_flags |= associated ? IWM_FLAG_BGSCAN : IWM_FLAG_SCANNING;
    nativeScanPassthrough = associated;
    iwm_host_cmd command = {};
    command.id = umac ? iwm_cmd_id(IWM_SCAN_REQ_UMAC, IWM_LONG_GROUP, 0) :
                        IWM_SCAN_OFFLOAD_REQUEST_CMD;
    command.len[0] = static_cast<uint16_t>(size);
    command.data[0] = request;
    nativeScanSubmitReject = 0;
    int error = iwm_send_cmd(sc, &command);
    if (error) nativeScanSubmitReject = nativeNvmDiagnostics.commandReject;
    ::free(request);
    sink.submitted(sink.context, error ? ItlScan::Submission::Indeterminate : ItlScan::Submission::Accepted);
    if (!error) {
        // Budget both active and passive dwell (40 * 2 * 255 ms), plus
        // firmware scheduling overhead. Cancel wakes this deadline promptly.
        error = iwm_wait_status(&nativeScanDone, 1, &nativeScanDone, 30, &nativeScanCancel);
        if (error && !nativeDetached) {
            sink.failed(sink.context, error == ECANCELED ? ItlScan::Outcome::Aborted :
                        error == ETIMEDOUT ? ItlScan::Outcome::TimedOut : ItlScan::Outcome::TransportError);
            if (commandTransportReady && !nativeScanDone) {
                // ACK is not a drain barrier. Even a failed abort ends in reset below.
                if (umac) iwm_native_umac_scan_abort();
                else iwm_lmac_scan_abort(sc);
                iwm_wait_status(&nativeScanDone, 1, &nativeScanDone, 3);
            }
        }
    }
    // Reset every unassociated native scan, including successful terminal RX,
    // before allowing reuse of LMAC's notification stream (no wire token).  An
    // associated bgscan leaves the device and the connection running: no
    // reset, and IFF_RUNNING stays set so the data path continues afterwards.
    //
    // The UMAC request does not need the reset. Its end is a notification the
    // driver recognises, so the stream is known to be in sync when the scan
    // returns; the reset existed to resynchronize a stream whose end could
    // not be recognised. Resetting there would cost the join a full restart
    // of the firmware, and the interface would look down in between, which
    // is what made turning the radio on slow and left the menu bar showing
    // no radio.
    const bool keepRadio = umac && nativeScanKeepRadioEnabled;
    nativeScanKeepRadio = keepRadio ? 1 : 0;
    if (!associated && !nativeDetached && !keepRadio) {
        iwm_stop_device(sc);
        sc->sc_ic.ic_ac.ac_if.if_flags &= ~IFF_RUNNING;
    }
    sc->sc_flags &= ~(IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN);
    nativeScanPassthrough = false;
    nativeScanSink = {};
    nativeScanPlan = nullptr;
    nativeScanCachePhy = {};
    nativeCacheDiagnostic.nodesAtScanEnd = sc->sc_ic.ic_nnodes;
    return error;
}

void ItlIwm::iwm_native_scan_cancel()
{
    if (!getMainWorkLoop()->inGate() || !nativeScanSink.valid()) return;
    nativeScanCancel = ECANCELED;
    getMainCommandGate()->commandWakeup(&nativeScanDone, false);
}

// The layer above understands one shape of scan notification. The AX HAL
// turns its own family's notifications into that shape, and so does this one:
// the UMAC completion becomes the LMAC-shaped offload completion and the
// UMAC iteration becomes the LMAC-shaped iteration. The two families lay
// their notifications out the same way, so the conversion is a copy.
uint16_t ItlIwm::iwm_native_scan_normalize(uint16_t code, const void *payload, size_t size,
                                            const void **body, size_t *bodySize)
{
    *body = payload;
    *bodySize = size;
    if (code == IWM_SCAN_COMPLETE_UMAC) {
        if (size < sizeof(struct iwm_umac_scan_complete)) return 0;
        const auto *bytes = static_cast<const uint8_t *>(payload);
        // The status is where the LMAC-shaped completion keeps it.
        const uint8_t status = bytes[offsetof(struct iwm_umac_scan_complete, status)];
        memset(nativeScanNotifScratch, 0, ScanNotificationHeader);
        nativeScanNotifScratch[2] = status;
        nativeScanRawTerminal = code;
        nativeScanRawStatus = status;
        *body = nativeScanNotifScratch;
        *bodySize = ScanNotificationHeader;
        return IWM_SCAN_OFFLOAD_COMPLETE;
    }
    if (code == IWM_SCAN_ITERATION_COMPLETE_UMAC) {
        const size_t header = offsetof(struct iwm_umac_scan_iter_complete_notif, results);
        const size_t channel = sizeof(struct iwm_scan_results_notif);
        if (size < header) return 0;
        const auto *bytes = static_cast<const uint8_t *>(payload);
        const size_t count = bytes[offsetof(struct iwm_umac_scan_iter_complete_notif, scanned_channels)];
        if (count > SequoiaWCL::ScanPlan::MaxChannels) return 0;
        // The firmware's count is not to be trusted: both the notification it
        // sent and the buffer the conversion writes into have to hold it.
        if (size < header + count * channel) return 0;
        if (ScanNotificationHeader + count * channel > sizeof(nativeScanNotifScratch)) return 0;
        memset(nativeScanNotifScratch, 0, ScanNotificationHeader + count * channel);
        nativeScanNotifScratch[0] = static_cast<uint8_t>(count);
        nativeScanNotifScratch[1] = bytes[offsetof(struct iwm_umac_scan_iter_complete_notif, status)];
        memcpy(nativeScanNotifScratch + ScanNotificationHeader, bytes + header, count * channel);
        nativeScanRawIteration = code;
        *body = nativeScanNotifScratch;
        *bodySize = ScanNotificationHeader + count * channel;
        return IWM_SCAN_ITERATION_COMPLETE;
    }
    return code;
}

bool ItlIwm::iwm_native_scan_rx(uint16_t code, const void *payload, size_t size)
{
    if (!nativeScanSink.valid()) return false;
    if (code != IWM_REPLY_RX_PHY_CMD && code != IWM_REPLY_RX_MPDU_CMD &&
        code != IWM_SCAN_ITERATION_COMPLETE && code != IWM_SCAN_OFFLOAD_COMPLETE &&
        code != IWM_SCAN_ITERATION_COMPLETE_UMAC && code != IWM_SCAN_COMPLETE_UMAC) return false;
    const void *body = payload;
    size_t bodySize = size;
    const uint16_t normalized = iwm_native_scan_normalize(code, payload, size, &body, &bodySize);
    if (!normalized) return false;
    nativeScanSink.notification(nativeScanSink.context, normalized, body, bodySize);
    iwm_native_scan_cache_rx(normalized, body, bodySize);
    ItlScan::ScanNotification notification;
    const bool terminal = normalized == IWM_SCAN_OFFLOAD_COMPLETE &&
        ItlScan::decodeIwmNotification(ItlScan::IwmNotification::LmacComplete, body, bodySize, notification);
    if (terminal) {
        nativeScanDone = 1;
        getMainCommandGate()->commandWakeup(&nativeScanDone, false);
    }
    // Associated bgscan passthrough: RX_PHY/RX_MPDU fall through to the legacy
    // rx.cpp switch so the data path keeps flowing between the firmware's
    // service-channel dwells.  Iteration/offload complete are still consumed
    // here - the legacy IWM_SCAN_ITERATION_COMPLETE case runs iwm_endscan,
    // which would clear the scan flags and run ieee80211_end_scan on a live
    // association.
    if (nativeScanPassthrough &&
        (code == IWM_REPLY_RX_PHY_CMD || code == IWM_REPLY_RX_MPDU_CMD)) {
        ItlScan::CacheDiagnostic::bump(nativeCacheDiagnostic.passthroughFrames);
        return false;
    }
    return true;
}
