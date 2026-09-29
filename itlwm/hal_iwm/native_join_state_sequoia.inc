void ItlIwm::iwm_native_join_event(const ItlJoin::Event &event)
{
    if (!getMainWorkLoop()->inGate() || nativeDetached ||
        (com.sc_flags & IWM_FLAG_SHUTDOWN) ||
        !nativeJoinProgress.record(event, com.sc_generation)) return;
    if (nativeJoinProgress.failed() && !nativeStateError)
        nativeStateError = event.stage == ItlJoin::Stage::Deauth || event.stage == ItlJoin::Stage::Disassoc
            ? ENETDOWN : ECONNREFUSED;
    getMainCommandGate()->commandWakeup(&nativeStateCompleted, false);
}

// taskq executes this on an external worker under the shared workloop gate.
// Firmware commands can sleep: never commit a transition after reset, detach
// or a newer request. Recovery belongs to the WCL owner, not automatic scan.
void ItlIwm::iwm_native_newstate_task()
{
    auto *loop = getMainWorkLoop();
    if (!loop->inGate() || loop->onThread() || nativeDetached ||
        (com.sc_flags & IWM_FLAG_SHUTDOWN) || nativeStateError) return;
    auto *sc = &com;
    auto *ic = &sc->sc_ic;
    const auto serial = nativeStateSerial;
    const int generation = sc->sc_generation;
    const auto requested = sc->ns_nstate;
    const auto previous = ic->ic_state;
    const int argument = sc->ns_arg;
    FirmwareLease operation(this);
    int error = operation ? 0 : EBUSY;
    auto current = [&]() {
        return !nativeDetached && generation == sc->sc_generation &&
            !(sc->sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR | IWM_FLAG_RFKILL)) &&
            serial == nativeStateSerial && commandTransportReady && !nativeStateError;
    };
    if (!error && (!current() || !(ic->ic_if.if_flags & IFF_RUNNING) ||
        !ic->ic_bss || ic->ic_opmode != IEEE80211_M_STA || isHardwareTrial())) error = ENXIO;
    if (!error && (requested < IEEE80211_S_INIT || requested > IEEE80211_S_RUN)) error = EINVAL;
    // BSD watchdog/retry can request SCAN, but WCL owns candidate selection.
    if (!error && requested == IEEE80211_S_SCAN) error = EOPNOTSUPP;
    if (!error && (previous == IEEE80211_S_SCAN ||
        (requested == IEEE80211_S_ASSOC && previous != IEEE80211_S_AUTH) ||
        (requested == IEEE80211_S_RUN && previous != IEEE80211_S_ASSOC))) error = EINVAL;
    if (!error && previous == IEEE80211_S_RUN) {
        timeout_del(&sc->sc_calib_to);
        iwm_del_task(sc, systq, &sc->ba_task);
        iwm_del_task(sc, systq, &sc->mac_ctxt_task);
        iwm_del_task(sc, systq, &sc->chan_ctxt_task);
        if (!current()) error = ECANCELED;
    }
    if (!error && requested <= previous) {
        if (previous == IEEE80211_S_RUN) error = iwm_run_stop(sc);
        if (!error && !current()) error = ECANCELED;
        if (!error && previous >= IEEE80211_S_AUTH && requested <= IEEE80211_S_AUTH)
            error = iwm_deauth(sc);
    }
    if (!error && !current()) error = ECANCELED;
    if (!error) {
        switch (requested) {
            case IEEE80211_S_AUTH: error = iwm_auth(sc); break;
            case IEEE80211_S_RUN: error = iwm_run(sc); break;
            default: break;
        }
    }
    if (!error && !current()) error = ECANCELED;
    if (!error) error = sc->sc_newstate(ic, requested, argument);
    // stop/detach owns its own reset result and joins us before reuse/free.
    if (nativeDetached || generation != sc->sc_generation ||
        (sc->sc_flags & IWM_FLAG_SHUTDOWN)) return;
    if (error && !nativeStateError) nativeStateError = error;
    nativeStateCompleted = serial;
    getMainCommandGate()->commandWakeup(&nativeStateCompleted, false);
}
