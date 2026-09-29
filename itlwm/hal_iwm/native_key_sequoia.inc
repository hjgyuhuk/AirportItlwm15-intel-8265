// The native WPA2 path uses the existing BSD CCMP implementation. RX cannot
// sleep waiting for a firmware ACK, and async command admission is not proof
// that a hardware key is installed. This path performs no firmware waits;
// context allocation uses the existing BSD compatibility allocator.
int ItlIwm::iwm_native_set_key(struct ieee80211com *ic,
    struct ieee80211_node *ni, struct ieee80211_key *k)
{
    if (!getMainWorkLoop()->inGate()) return EWOULDBLOCK;
    if (ic != &com.sc_ic || !ni || ni != ic->ic_bss || !k) return EINVAL;
    if (isHardwareTrial() || nativeDetached || !nativeNet80211Attached ||
        !commandTransportReady || nativeStateError ||
        nativeStateSerial != nativeStateCompleted || nativeFirmwareOwner.busy() ||
        (com.sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR | IWM_FLAG_RFKILL)) ||
        !(ic->ic_if.if_flags & IFF_RUNNING) || ic->ic_state != IEEE80211_S_RUN ||
        ic->ic_opmode != IEEE80211_M_STA ||
        !nativeJoinProgress.ownsPeer(ni->ni_bssid, com.sc_generation) ||
        !nativeJoinProgress.assoc() || nativeJoinProgress.assoc()->status) return ENETDOWN;
    if (!(ic->ic_flags & IEEE80211_F_RSNON) || !(ic->ic_flags & IEEE80211_F_PSK) ||
        ni->ni_rsnprotos != IEEE80211_PROTO_RSN || ni->ni_rsnakms != IEEE80211_AKM_PSK ||
        ni->ni_rsncipher != IEEE80211_CIPHER_CCMP || ni->ni_rsngroupcipher != IEEE80211_CIPHER_CCMP ||
        k->k_cipher != IEEE80211_CIPHER_CCMP || k->k_len != 16 || k->k_id > 3 ||
        (k->k_flags & ~(IEEE80211_KEY_GROUP | IEEE80211_KEY_TX))) return EOPNOTSUPP;
    if (k->k_priv) return EBUSY;
    if (k->k_flags & IEEE80211_KEY_GROUP) {
        if (k != &ic->ic_nw_keys[k->k_id]) return EINVAL;
    } else if (k != &ni->ni_pairwise_key || k->k_id || k->k_flags) return EINVAL;
    return ieee80211_set_key(ic, ni, k);
}

bool ItlIwm::iwm_native_port_ready()
{
    if (!getMainWorkLoop()->inGate() || isHardwareTrial() || nativeDetached ||
        !nativeNet80211Attached || !commandTransportReady || nativeStateError ||
        nativeStateSerial != nativeStateCompleted || nativeFirmwareOwner.busy() ||
        (com.sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR | IWM_FLAG_RFKILL))) return false;
    const auto *ic = &com.sc_ic;
    const auto *ni = ic->ic_bss;
    if (!ni || ic->ic_state != IEEE80211_S_RUN || ic->ic_opmode != IEEE80211_M_STA ||
        !(ic->ic_if.if_flags & IFF_RUNNING) || ic->ic_if.if_link_state != LINK_STATE_UP ||
        (ic->ic_flags & IEEE80211_F_WEPON) ||
        !nativeJoinProgress.ownsPeer(ni->ni_bssid, com.sc_generation) ||
        !nativeJoinProgress.assoc() || nativeJoinProgress.assoc()->status) return false;
    if (!(ic->ic_flags & IEEE80211_F_RSNON))
        return !(ic->ic_flags & IEEE80211_F_PSK) &&
            ni->ni_pairwise_key.k_cipher == IEEE80211_CIPHER_NONE;
    if (!(ic->ic_flags & IEEE80211_F_PSK) || !ni->ni_port_valid ||
        (ni->ni_flags & IEEE80211_NODE_TXRXPROT) != IEEE80211_NODE_TXRXPROT ||
        ni->ni_rsnprotos != IEEE80211_PROTO_RSN || ni->ni_rsnakms != IEEE80211_AKM_PSK ||
        ni->ni_rsncipher != IEEE80211_CIPHER_CCMP || ni->ni_rsngroupcipher != IEEE80211_CIPHER_CCMP)
        return false;
    const auto &pair = ni->ni_pairwise_key;
    if (pair.k_cipher != IEEE80211_CIPHER_CCMP || pair.k_len != 16 || pair.k_id ||
        pair.k_flags != IEEE80211_KEY_SWCRYPTO || !pair.k_priv) return false;
    for (unsigned id = 0; id < 4; ++id) {
        const auto &group = ic->ic_nw_keys[id];
        if (group.k_cipher == IEEE80211_CIPHER_CCMP && group.k_len == 16 && group.k_id == id &&
            (group.k_flags & ~IEEE80211_KEY_TX) == (IEEE80211_KEY_SWCRYPTO | IEEE80211_KEY_GROUP) &&
            group.k_priv) return true;
    }
    return false;
}

// Only full stop calls this after firmware RX and state workers are drained.
// BSD INIT cleanup does not release the BSS pairwise software context.
void ItlIwm::iwm_native_clear_keys()
{
    auto *ic = &com.sc_ic;
    for (auto &key : ic->ic_nw_keys) ieee80211_reset_key(ic, &key);
    if (ic->ic_bss) {
        auto *ni = ic->ic_bss;
        ieee80211_reset_key(ic, &ni->ni_pairwise_key);
        ni->ni_port_valid = 0;
        ni->ni_rsn_group_ack_pending = 0;
        explicit_bzero(ni->ni_rsn_group_ack_mic, sizeof(ni->ni_rsn_group_ack_mic));
        ni->ni_flags &= ~(IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_PMK);
        explicit_bzero(&ni->ni_ptk, sizeof(ni->ni_ptk));
        explicit_bzero(ni->ni_pmk, sizeof(ni->ni_pmk));
        explicit_bzero(ni->ni_nonce, sizeof(ni->ni_nonce));
    }
    ic->ic_flags &= ~IEEE80211_F_PSK;
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    explicit_bzero(ic->ic_nonce, sizeof(ic->ic_nonce));
}
