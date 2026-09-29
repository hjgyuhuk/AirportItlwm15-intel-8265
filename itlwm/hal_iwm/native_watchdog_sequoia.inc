// Poll only on the native HAL workloop. Never run the legacy watchdog's
// automatic init/scan tasks; the WCL join owner performs joined cleanup.
int ItlIwm::iwm_native_watchdog(uint64_t nowMs)
{
    if (!getMainWorkLoop()->inGate()) return EWOULDBLOCK;
    if (nativeStateError) return nativeStateError;
    auto *ic = &com.sc_ic;
    if (isHardwareTrial() || nativeDetached || !nativeNet80211Attached ||
        !commandTransportReady || !ic->ic_bss ||
        !(ic->ic_if.if_flags & IFF_RUNNING) ||
        (com.sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR | IWM_FLAG_RFKILL)) ||
        !nativeJoinProgress.ownsPeer(ic->ic_bss->ni_bssid, com.sc_generation)) return ENETDOWN;
    if (nativeStateSerial != nativeStateCompleted || nativeFirmwareOwner.busy()) {
        nativeWatchdogActive = false;
        return 0;
    }
    if (!nativeWatchdogActive) {
        nativeWatchdogLastMs = nowMs;
        nativeWatchdogActive = true;
        return 0;
    }
    bool expired = nowMs < nativeWatchdogLastMs;
    if (!expired) {
        const uint64_t elapsedSeconds = (nowMs - nativeWatchdogLastMs) / 1000;
        if (!elapsedSeconds) return 0;
        nativeWatchdogLastMs += elapsedSeconds * 1000;
        for (auto &timer : com.sc_tx_timer) {
            if (timer <= 0) continue;
            if (elapsedSeconds >= static_cast<uint64_t>(timer)) { timer = 0; expired = true; }
            else timer -= elapsedSeconds;
        }
        if (ic->ic_mgt_timer > 0) {
            if (elapsedSeconds >= static_cast<uint64_t>(ic->ic_mgt_timer)) {
                ic->ic_mgt_timer = 0;
                expired = true;
            } else ic->ic_mgt_timer -= elapsedSeconds;
        }
    }
    if (!expired) return 0;
    nativeStateError = ETIMEDOUT;
    ic->ic_bss->ni_port_valid = 0;
    ic->ic_if.if_link_state = LINK_STATE_DOWN;
    return nativeStateError;
}
