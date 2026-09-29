// Configure the existing BSD supplicant, without starting association or
// reporting success to WCL. The join owner still must validate candidates,
// radio controls, select a fresh beacon, and deliver real connection events.
int ItlIwm::iwm_native_join_security(const SequoiaWCL::JoinRequestSnapshot &snapshot)
{
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread()) return EWOULDBLOCK;
    FirmwareLease operation(this);
    if (!operation) return EBUSY;
    auto *ic = &com.sc_ic;
    const auto *request = snapshot.get();
    if (!request) return EINVAL;
    if (isHardwareTrial() || !nativeNet80211Attached || nativeScanSink.valid() ||
        ic->ic_state != IEEE80211_S_INIT || ic->ic_opmode != IEEE80211_M_STA ||
        (ic->ic_flags & IEEE80211_F_WEPON)) return EBUSY;
    // Changing installed keys is a different transaction, not a silent reset
    // of live firmware or per-peer replay counters from a configuration helper.
    for (size_t i = 0; i < nitems(ic->ic_nw_keys); ++i)
        if (ic->ic_nw_keys[i].k_cipher != IEEE80211_CIPHER_NONE) return EBUSY;
    if (!ic->ic_bss || ic->ic_bss->ni_pairwise_key.k_cipher != IEEE80211_CIPHER_NONE)
        return EBUSY;
    if (request->authLower != APPLE80211_AUTHTYPE_OPEN || request->authUpperExtended ||
        !SequoiaWCL::supportedJoinAllowAuths(*request))
        return EOPNOTSUPP;
    const auto &key = request->key;
    if (key.version > 1 || key.flags || key.index || key.rscLength || key.kckLength ||
        key.kekLength || request->extraKeyLength) return EOPNOTSUPP;
    ieee80211_wpaparams wpa = {};
    const bool secured = request->authUpper == APPLE80211_AUTHTYPE_WPA2_PSK;
    if (!request->authUpper) {
        if (key.length || key.cipher != APPLE80211_CIPHER_NONE || request->rsnLength)
            return EINVAL;
    } else if (secured) {
        if (!(ic->ic_caps & IEEE80211_C_RSN)) return ENODEV;
        if (key.cipher != APPLE80211_CIPHER_PMK) return EOPNOTSUPP;
        if (key.length != sizeof(ic->ic_psk)) return EINVAL;
        // A single explicit CCMP + PSK RSN selection, optionally with caps.
        // This bounds the BSD parser and excludes partially present optional
        // fields, PMKSA caching, MFP and an appended IE it would not consume.
        if ((request->rsnLength != 20 && request->rsnLength != 22) ||
            request->rsn[0] != IEEE80211_ELEMID_RSN ||
            static_cast<unsigned>(request->rsn[1]) + 2 != request->rsnLength)
            return EINVAL;
        ieee80211_rsnparams rsn = {};
        if (ieee80211_parse_rsn(ic, request->rsn, &rsn) != IEEE80211_STATUS_SUCCESS)
            return EINVAL;
        if (rsn.rsn_nciphers != 1 || rsn.rsn_ciphers != IEEE80211_CIPHER_CCMP ||
            rsn.rsn_groupcipher != IEEE80211_CIPHER_CCMP ||
            rsn.rsn_nakms != 1 || rsn.rsn_akms != IEEE80211_AKM_PSK ||
            rsn.rsn_npmkids || (rsn.rsn_caps & ~(IEEE80211_RSNCAP_PTKSA_RCNT_MASK |
                                               IEEE80211_RSNCAP_GTKSA_RCNT_MASK)))
            return EOPNOTSUPP;
        // The shared BSD parser also recognizes WPA vendor selectors. RSN
        // requests must use the IEEE OUI, even when the suite number matches.
        const unsigned selectors[] = {4, 10, 16};
        for (const unsigned offset : selectors)
            if (memcmp(request->rsn + offset, IEEE80211_OUI, 3)) return EOPNOTSUPP;
        wpa.i_enabled = 1;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK;
        wpa.i_groupcipher = wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP;
    } else return EOPNOTSUPP;

    // No operation below sleeps. All fallible validation precedes mutation;
    // setwpaparms returns ENETRESET to mean configuration changed, not failure.
    if (secured) {
        const int error = ieee80211_ioctl_setwpaparms(ic, &wpa);
        if (error && error != ENETRESET) return error;
    } else ieee80211_disable_rsn(ic);
    SequoiaWCL::clearJoinSecret(ic->ic_psk, sizeof(ic->ic_psk));
    ic->ic_flags &= ~IEEE80211_F_PSK;
    if (secured) {
        memcpy(ic->ic_psk, key.bytes, sizeof(ic->ic_psk));
        ic->ic_flags |= IEEE80211_F_PSK;
    }
    return 0;
}
