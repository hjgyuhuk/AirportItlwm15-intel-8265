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
    if (!plan.valid() || !sink.valid() || nativeScanSink.valid() || isHardwareTrial() ||
        !nativeNet80211Attached || !commandTransportReady || sc->sc_device_family != IWM_DEVICE_FAMILY_7000 ||
        sc->sc_mqrx_supported || sc->sc_uc_current != IWM_UCODE_TYPE_REGULAR ||
        sc->cmdqid != IWM_DQA_CMD_QUEUE ||
        !(sc->sc_ic.ic_ac.ac_if.if_flags & IFF_RUNNING) ||
        (!associated && sc->sc_ic.ic_state != IEEE80211_S_INIT) ||
        (associated && (sc->sc_ic.ic_state != IEEE80211_S_RUN || !sc->sc_ic.ic_bss)) ||
        (sc->sc_flags & (IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN | IWM_FLAG_RFKILL)) ||
        isset(sc->sc_enabled_capa, IWM_UCODE_TLV_CAPA_UMAC_SCAN)) return EOPNOTSUPP;
    IwmTrial::PassivePlan wire;
    wire.count = plan.channelCount;
    wire.capacity = sc->sc_capa_n_scan_channels;
    wire.txAnt = iwm_fw_valid_tx_ant(sc);
    wire.rxAnt = iwm_fw_valid_rx_ant(sc);
    if (wire.count > nitems(wire.channels)) return EINVAL;
    for (size_t i = 0; i < wire.count; ++i) {
        const auto channel = plan.channels[i];
        if (channel >= nitems(sc->sc_ic.ic_channels) || !iwm_is_valid_channel(channel) ||
            !sc->sc_ic.ic_channels[channel].ic_freq || !isset(sc->sc_ic.ic_chan_avail, channel)) return EINVAL;
        if (plan.probes[i] && (sc->sc_ic.ic_channels[channel].ic_flags &
            (IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_DFS))) return EOPNOTSUPP;
        wire.channels[i] = channel;
    }
    memcpy(wire.source, plan.source, sizeof(wire.source));
    const size_t size = IwmTrial::passiveRequestSize(wire);
    if (!size) return EINVAL;
    void *request = malloc(size, M_DEVBUF, M_NOWAIT);
    if (!request) return ENOMEM;
    const bool fragmented = associated &&
        isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_FRAGMENTED_SCAN);
    nativeCacheDiagnostic.assocScanFragmented = fragmented ? 1 : 0;
    if (!IwmTrial::buildNativeScanRequest(plan, wire.capacity, wire.txAnt, wire.rxAnt,
                                          fragmented, request, size)) {
        ::free(request);
        return EINVAL;
    }
    nativeScanSink = sink;
    nativeScanPlan = &plan;
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
    command.id = IWM_SCAN_OFFLOAD_REQUEST_CMD;
    command.len[0] = static_cast<uint16_t>(size);
    command.data[0] = request;
    int error = iwm_send_cmd(sc, &command);
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
                iwm_lmac_scan_abort(sc);
                iwm_wait_status(&nativeScanDone, 1, &nativeScanDone, 3);
            }
        }
    }
    // Reset every unassociated native scan, including successful terminal RX,
    // before allowing reuse of LMAC's notification stream (no wire token).  An
    // associated bgscan leaves the device and the connection running: no
    // reset, and IFF_RUNNING stays set so the data path continues afterwards.
    if (!associated && !nativeDetached) {
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
        code != IWM_SCAN_ITERATION_COMPLETE && code != IWM_SCAN_OFFLOAD_COMPLETE) return false;
    nativeScanSink.notification(nativeScanSink.context, code, payload, size);
    iwm_native_scan_cache_rx(code, payload, size);
    ItlScan::ScanNotification notification;
    if (code == IWM_SCAN_OFFLOAD_COMPLETE &&
        ItlScan::decodeIwmNotification(ItlScan::IwmNotification::LmacComplete, payload, size, notification)) {
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
