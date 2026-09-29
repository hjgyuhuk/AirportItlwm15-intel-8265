// Best-effort over-air notification, followed by the owner's mandatory full
// stop. Empty TX rings mean completion, not proof that the AP received an ACK.
int ItlIwm::iwm_native_leave(bool deauthenticate, uint16_t reason, unsigned waitMs)
{
    if (!getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread()) return EWOULDBLOCK;
    if (!reason || waitMs > 1000) return EINVAL;
    FirmwareLease operation(this);
    if (!operation) return EBUSY;
    auto *ic = &com.sc_ic;
    if (ic->ic_state == IEEE80211_S_INIT) return 0;
    const auto generation = com.sc_generation;
    auto current = [&]() {
        return !nativeDetached && generation == com.sc_generation && commandTransportReady &&
            !nativeStateError && !(com.sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR | IWM_FLAG_RFKILL));
    };
    if (!current() || !(ic->ic_if.if_flags & IFF_RUNNING) || !ic->ic_bss ||
        !nativeJoinProgress.ownsPeer(ic->ic_bss->ni_bssid, generation)) return ENETDOWN;
    if (ic->ic_opmode != IEEE80211_M_STA || ic->ic_state < IEEE80211_S_AUTH) return EINVAL;
    if (!ic->ic_if.netStat) return ENXIO;
    if (mq_full(&ic->ic_mgtq)) return ENOBUFS;
    const auto beforeErrors = ic->ic_if.netStat->outputErrors;
    const int error = ieee80211_send_mgmt(ic, ic->ic_bss,
        deauthenticate ? IEEE80211_FC0_SUBTYPE_DEAUTH : IEEE80211_FC0_SUBTYPE_DISASSOC, reason, 0);
    if (error) return error;
    // WCL's zero means no override. Use a bounded local drain in that case.
    const uint64_t started = airport_up_time();
    const unsigned budget = waitMs ? waitMs : 100;
    for (;;) {
        if (!current()) return ENETDOWN;
        if (ic->ic_if.netStat->outputErrors != beforeErrors) return EIO;
        bool pending = !mq_empty(&ic->ic_mgtq);
        for (unsigned qid = 0; qid < nitems(com.txq); ++qid)
            if (qid != com.cmdqid && com.txq[qid].queued) pending = true;
        if (!pending) return 0;
        const auto now = airport_up_time();
        if (now < started || now - started >= budget) return ETIMEDOUT;
        AbsoluteTime deadline;
        clock_interval_to_deadline(1, kMillisecondScale, &deadline);
        getMainCommandGate()->commandSleep(&com.txq, deadline, THREAD_UNINT);
        // Queue pressure may have postponed the management frame before the
        // previous TX completion. The regular BSD producer owns its mbuf.
        if (current()) iwm_start(&ic->ic_if);
    }
}
