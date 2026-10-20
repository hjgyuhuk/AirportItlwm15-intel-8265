// Called only by the external WCL join owner after validating request controls
// and excluding scans/PM. Success means AUTH was queued, not associated/online.
// The controller endpoint is not connected until event/lifetime wiring exists.
int ItlIwm::iwm_native_join_start(const SequoiaWCL::JoinRequestSnapshot &snapshot, size_t index)
{
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread()) return EWOULDBLOCK;
    FirmwareLease operation(this);
    if (!operation) return EBUSY;
    auto *sc = &com;
    auto *ic = &sc->sc_ic;
    if (nativeStateError) return nativeStateError;
    if (nativeJoinProgress.active()) return EBUSY;
    // Probe build: the 7000-family restriction is lifted for the 8000 family.
    // The AUTH frame this path queues was derived from firmware 17's station
    // ABI; the 8265 runs firmware 36, so the exchange may still be invalid
    // there. Every other precondition below is unchanged.
    if (!commandTransportReady || !(ic->ic_if.if_flags & IFF_RUNNING) ||
        sc->ns_nstate != IEEE80211_S_INIT ||
        sc->sc_device_family == IWM_DEVICE_FAMILY_9000 ||
        sc->sc_mqrx_supported || sc->sc_uc_current != IWM_UCODE_TYPE_REGULAR ||
        (sc->sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR | IWM_FLAG_RFKILL))) return ENXIO;
    ieee80211_node *candidate = nullptr;
    int error = iwm_native_join_candidate(snapshot, index, &candidate);
    if (error) return error;
    const auto *request = snapshot.get();
    // Match in AUTO mode: chan2mode otherwise assumes that every candidate
    // belongs to the previously selected legacy band, even after a WCL scan.
    nativeCandidateDiagnostic.stage = 2;
    error = ieee80211_setmode(ic, IEEE80211_MODE_AUTO);
    if (!error) {
        nativeCandidateDiagnostic.stage = 3;
        error = iwm_native_join_security(snapshot);
    }
    if (!error) {
        ic->ic_des_esslen = request->ssidLength;
        bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
        memcpy(ic->ic_des_essid, request->ssid, request->ssidLength);
        IEEE80211_ADDR_COPY(ic->ic_des_bssid, candidate->ni_bssid);
        ic->ic_des_chan = candidate->ni_chan;
        ic->ic_flags = (ic->ic_flags & ~(IEEE80211_F_AUTO_JOIN | IEEE80211_F_BGSCAN)) |
                       IEEE80211_F_DESBSSID;
        // The standard BSD matcher/negotiator remains authoritative for
        // rates, privacy, AKM and cipher compatibility. It may modify rates.
        nativeCandidateDiagnostic.stage = 4;
        if (ieee80211_match_bss(ic, candidate, 0)) error = EINVAL;
        else {
            nativeCandidateDiagnostic.stage = 5;
            if (!nativeJoinProgress.begin(candidate->ni_bssid, sc->sc_generation)) error = EBUSY;
            else {
                nativeCandidateDiagnostic.stage = 6;
                error = ieee80211_node_join_bss(ic, candidate, 0);
                if (!error) nativeCandidateDiagnostic.stage = 7;
            }
        }
        if (error) {
            nativeJoinProgress.clear();
            // No asynchronous AUTH was accepted. Do not leave the failed
            // attempt's PMK or desired network configured for a later caller.
            ieee80211_disable_rsn(ic);
            SequoiaWCL::clearJoinSecret(ic->ic_psk, sizeof(ic->ic_psk));
            ic->ic_flags &= ~(IEEE80211_F_PSK | IEEE80211_F_DESBSSID);
            ic->ic_des_esslen = 0;
            bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
            bzero(ic->ic_des_bssid, sizeof(ic->ic_des_bssid));
            ic->ic_des_chan = IEEE80211_CHAN_ANYC;
            ieee80211_node_newstate(candidate, IEEE80211_STA_CACHE);
        }
    }
    ieee80211_unref_node(&candidate);
    return error;
}
