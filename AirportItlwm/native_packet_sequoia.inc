// All packet callbacks execute without sleeping under the HAL workqueue gate.
// Admission closes before teardown drains that gate or releases registration.
bool AirportItlwm::initNativePacket()
{
    if (nativePacketTimer) return false;
    auto *ifp = &fHalService->get80211Controller()->ic_if;
    if (!ifp->netStat) ifp->netStat = &nativeNetworkStats;
    nativePacketTimer = IOTimerEventSource::timerEventSource(this, nativePacketTimerAction);
    if (!nativePacketTimer || _fWorkloop->addEventSource(nativePacketTimer) != kIOReturnSuccess) return false;
    nativePacketTimerAdded = true;
    return true;
}

void AirportItlwm::stopNativePacket()
{
    if (!nativePacketTimer) return;
    if (nativePacketTimerAdded) {
        _fWorkloop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<AirportItlwm *>(owner);
            self->setNativeDataPath(false);
            self->nativeTxRetry = false;
            self->nativePacketTimer->cancelTimeout();
            self->nativePacketTimer->disable();
            return kIOReturnSuccess;
        }, this);
        _fWorkloop->removeEventSource(nativePacketTimer);
        nativePacketTimerAdded = false;
    }
    OSSafeReleaseNULL(nativePacketTimer);
}

void AirportItlwm::retryNativeTransmit(UInt delay)
{
    if (!_fWorkloop->inGate() || !nativeAvailable() || !nativePacketTimerAdded || nativePacketError)
        return;
    nativeTxRetry = true;
    nativePacketError = nativePacketTimer->setTimeoutMS(delay);
}

void AirportItlwm::nativePacketTimerAction(OSObject *owner, IOTimerEventSource *timer)
{
    auto *self = static_cast<AirportItlwm *>(owner);
    if (!self->getWorkQueue()->inGate() || timer != self->nativePacketTimer ||
        !self->nativeAvailable() || !self->nativeTxRetry || self->nativePacketError) return;
    if (!self->nativeTrafficAvailable()) { self->retryNativeTransmit(100); return; }
    self->nativeTxRetry = false;
    auto *queue = self->nativeRegistration.txQueue();
    if (queue && queue->packetSubmission(false) != kIOReturnSuccess)
        self->retryNativeTransmit(10);
}

bool AirportItlwm::nativeTrafficAvailable()
{
    return nativeRegistration.packetQueuesEnabled() && nativeJoin.connected && nativeJoin.completionPosted &&
        !nativeJoin.cancel && !nativeJoin.error && nativePortAvailable();
}

bool AirportItlwm::nativePortAvailable()
{
    if (!nativeRadioAvailable() || !nativeRegistration.prepared() || !nativeHalAttached || nativePacketError)
        return false;
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    auto *hal = fHalService ? fHalService->getNativeHal() : nullptr;
    if (!hal || !hal->nativePortReady() || hal->nativeDetachedState() || hal->nativeStateErrorCode() ||
        !hal->nativeCommandTransportReady() || !hal->nativeJoinProgressState().active() ||
        hal->nativeJoinProgressState().failed()) return false;
    auto *ic = fHalService->get80211Controller();
#else
    // Host tests use the small IWM model directly instead of ItlHalService.
    // Keep the same admission predicates without making that model emulate
    // the kernel-only native HAL wrapper.
    if (!fHalService || !fHalService->iwm_native_port_ready() || fHalService->nativeDetached ||
        fHalService->nativeStateError || !fHalService->commandTransportReady ||
        fHalService->nativeStateSerial != fHalService->nativeStateCompleted ||
        fHalService->nativeFirmwareOwner.busy() ||
        !fHalService->nativeJoinProgress.active() || fHalService->nativeJoinProgress.failed()) return false;
    auto *ic = fHalService->get80211Controller();
#endif
    return ic && ic->ic_bss && ic->ic_state == IEEE80211_S_RUN &&
        (ic->ic_if.if_flags & IFF_RUNNING) && ic->ic_if.if_link_state == LINK_STATE_UP;
}

UInt AirportItlwm::nativeTransmitPackets(IOSkywalkTxSubmissionQueue *queue,
                                        IOSkywalkPacket *const *packets, UInt count)
{
    if (!_fWorkloop->inGate() || queue != nativeRegistration.txQueue() ||
        (count && !packets)) return 0;
    if (!nativeTrafficAvailable()) { retryNativeTransmit(100); return 0; }
    auto *ifp = &fHalService->get80211Controller()->ic_if;
    if (!ifp->if_snd.queue || !ifp->if_start) return 0;
    bool queued = false;
    const UInt consumed = SequoiaPacketIO::transmit(nativeRegistration.pool(), packets, count,
        [&](const uint8_t *bytes, size_t length) {
            mbuf_t copy = nullptr;
            if (mbuf_allocpacket(MBUF_DONTWAIT, length, nullptr, &copy) || !copy)
                return SequoiaPacketIO::Submission::Retry;
            if (mbuf_copyback(copy, 0, length, bytes, MBUF_DONTWAIT) ||
                !ifp->if_snd.queue->lockEnqueue(copy)) {
                mbuf_freem(copy);
                return SequoiaPacketIO::Submission::Retry;
            }
            nativeTrafficCounters.transmitted();
            queued = true;
            return SequoiaPacketIO::Submission::Accepted;
        });
    // The BSD/Intel TX path owns only independent mbufs, never Skywalk memory.
    if (queued) (*ifp->if_start)(ifp);
    // The system stops calling us after a zero-consumption result. The old
    // BSD signalOutputThread only wakes the Ethernet pull queue, not Skywalk.
    // Retry on this workloop so transient allocation/queue pressure can clear.
    if (consumed < count) retryNativeTransmit(10);
    return consumed;
}

UInt32 AirportItlwm::nativeReceivePacket(mbuf_t packet)
{
    if (!packet) return 0;
    IOReturn result = kIOReturnNotReady;
    if (nativeTrafficAvailable() && (mbuf_flags(packet) & MBUF_PKTHDR)) {
        const auto priorFailure = nativeRxDiagnostics.failureResult;
        const auto priorStage = nativeRxDiagnostics.failureStage;
        result = SequoiaPacketIO::receive(nativeRegistration.pool(), nativeRegistration.rxQueue(),
            mbuf_pkthdr_len(packet), [&](uint8_t *bytes, size_t length) {
                return mbuf_copydata(packet, 0, length, bytes) == 0;
            }, &nativeRxDiagnostics);
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
        // Bounded sampling; retain the last failure even after a good packet.
        // No packet bytes or network credentials enter registry diagnostics.
        const auto &d = nativeRxDiagnostics;
        if (d.attempts <= 4 || !(d.attempts & (d.attempts - 1)) ||
            priorFailure != d.failureResult || priorStage != d.failureStage) {
            auto *provider = __atomic_load_n(&gItlwmStageProvider, __ATOMIC_RELAXED);
            if (provider) {
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxAttempts", d.attempts, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxDelivered", d.delivered, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxFailed", d.failed, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxStage", d.stage, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxResult", d.result, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxFailureStage", d.failureStage, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxFailureResult", d.failureResult, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxBuffers", d.buffers, 32);
                ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRxCapacity", d.capacity, 32);
            }
        }
#else
        (void)priorFailure; (void)priorStage;
#endif
    }
    // The Skywalk path copies the bytes into its own packet; the mbuf is only a
    // source. On failure return ownership to the caller so it can deliver the same
    // mbuf through the legacy IONetworkInterface input path instead of dropping it.
    if (result != kIOReturnSuccess) return 0;
    nativeTrafficCounters.received();
    mbuf_freem(packet);
    return 1;
}

// The compatibility RX list also serves Skywalk, which has no IOEthernetInterface
// child. The HAL already lends this controller pointer until RX is drained by
// detach; use it under the same command gate. A zero return retains the mbuf.
UInt32 itlwmNativeInputPacket(IOEthernetController *controller, mbuf_t packet)
{
    auto *owner = OSDynamicCast(AirportItlwm, controller);
    return owner ? owner->nativeReceivePacket(packet) : 0;
}
