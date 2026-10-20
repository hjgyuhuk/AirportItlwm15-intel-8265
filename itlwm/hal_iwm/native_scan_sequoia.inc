// Whether a scan leaves the radio running. A firmware family that answers
// the request in a form carrying a scan id does not need the radio reset the
// other one needs, because its notification stream stays in sync.
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
    // The UMAC request does not need it: it carries a scan id and its
    // notifications carry the same one, so the stream stays in sync.  Stopping
    // the radio there would cost the join a full restart of the firmware, and
    // the interface would look down in between, which is what made turning
    // the radio on slow and left the menu bar showing no radio.
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

bool ItlIwm::iwm_native_scan_rx(uint16_t code, const void *payload, size_t size)
{
    if (!nativeScanSink.valid()) return false;
    if (code != IWM_REPLY_RX_PHY_CMD && code != IWM_REPLY_RX_MPDU_CMD &&
        code != IWM_SCAN_ITERATION_COMPLETE && code != IWM_SCAN_OFFLOAD_COMPLETE &&
        code != IWM_SCAN_ITERATION_COMPLETE_UMAC && code != IWM_SCAN_COMPLETE_UMAC) return false;
    // The layer above understands one shape of scan notification. The AX HAL
    // turns its own family's notifications into that shape, and so does this
    // one: the UMAC completion becomes the LMAC-shaped offload completion and
    // the UMAC iteration becomes the LMAC-shaped iteration. The two families
    // lay their notifications out the same way, so the conversion is a copy.
    uint16_t normalized = code;
    const void *body = payload;
    size_t bodySize = size;
    if (code == IWM_SCAN_COMPLETE_UMAC) {
        if (size < 12) return false;
        memset(nativeScanNotifScratch, 0, 12);
        nativeScanNotifScratch[2] = static_cast<const uint8_t *>(payload)[6];
        nativeScanRawTerminal = code;
        nativeScanRawStatus = nativeScanNotifScratch[2];
        normalized = IWM_SCAN_OFFLOAD_COMPLETE;
        body = nativeScanNotifScratch;
        bodySize = 12;
    } else if (code == IWM_SCAN_ITERATION_COMPLETE_UMAC) {
        if (size < 16) return false;
        const auto *bytes = static_cast<const uint8_t *>(payload);
        const size_t count = bytes[4];
        if (count > 40 || 12 + count * 8 > sizeof(nativeScanNotifScratch)) return false;
        memset(nativeScanNotifScratch, 0, 12 + count * 8);
        nativeScanNotifScratch[0] = static_cast<uint8_t>(count);
        nativeScanNotifScratch[1] = bytes[5];
        memcpy(nativeScanNotifScratch + 12, bytes + 16, count * 8);
        nativeScanRawIteration = code;
        normalized = IWM_SCAN_ITERATION_COMPLETE;
        body = nativeScanNotifScratch;
        bodySize = 12 + count * 8;
    }
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
