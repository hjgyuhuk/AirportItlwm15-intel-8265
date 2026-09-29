// Resolve the ifnet from its selected owner. Skywalk uses Apple's public
// getBSDInterface API; the legacy mode uses our Ethernet child. Synchronize
// running/MAC without changing the user's administrative IFF_UP flag.
IOReturn AirportItlwm::setNativeDataPath(bool enabled)
{
    if (!_fWorkloop || !_fWorkloop->inGate()) return kIOReturnCannotLock;
    auto ifnet = nativeSkywalkBSDOwner ? (fNetIf ? fNetIf->getBSDInterface() : nullptr) :
        (bsdInterface ? bsdInterface->getIfnet() : nullptr);
    auto finish = [&](IOReturn result) {
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
        if (auto *provider = getProvider()) {
            provider->setProperty("ItlwmNativeDataPathResult", static_cast<uint32_t>(result), 32);
            provider->setProperty("ItlwmNativeDataPathEnabled", nativeRegistration.packetQueuesEnabled(), 32);
            provider->setProperty("ItlwmNativeDataPathBSDFlags", ifnet ? ifnet_flags(ifnet) : 0, 32);
            // r228 A: sample the same queue state again at the data-path boundary.
            // The start-time sample is taken before any BSD stack exists; this one
            // is taken when the path is asked to run, which is when the netif nexus
            // callbacks (nxp_qset_init/nxp_queue_init) would already have run if the
            // Skywalk BSD client owns the interface.
            bool itlwmPublishQueueState(IOService *target, const char *prefix, void *queue);
            itlwmPublishQueueState(provider, "ItlwmNativeTxQueueLink", nativeRegistration.txQueue());
            itlwmPublishQueueState(provider, "ItlwmNativeRxQueueLink", nativeRegistration.rxQueue());
            // The BSD client IOKit matches onto the interface is created
            // asynchronously after registerService, so the start-time sample can
            // legitimately show no children; this later sample is the one that
            // decides whether the client exists and whether its start completed
            // (state bit 0) and left the service enabled (bit 1).
            bool itlwmPublishInterfaceChildren(IOService *target, void *interface, unsigned *count);
            itlwmPublishInterfaceChildren(provider, fNetIf, nullptr);
        }
#endif
        return result;
    };
    auto close = [&]() {
        if (bsdInterface) bsdInterface->stopOutputThread();
        nativeRegistration.setPacketQueuesEnabled(false);
        return ifnet && ifnet_set_flags(ifnet, 0, IFF_RUNNING) ? kIOReturnError : kIOReturnSuccess;
    };
    // Down/stop works after nativeAvailable closes, while the gate still owns
    // registration and BSD lifetime. Close queues even if flag clearing fails.
    if (!enabled) return finish(close());
    if (!nativeAvailable() || !nativeInterfaceStarted || !ifnet || !nativePortAvailable()) {
        close();
        return finish(kIOReturnNotReady);
    }
    auto *ic = fHalService->get80211Controller();
    uint8_t current[6] = {};
    if (ifnet_lladdr_copy_bytes(ifnet, current, sizeof(current)) ||
        (memcmp(current, ic->ic_myaddr, sizeof(current)) &&
         ifnet_set_lladdr(ifnet, ic->ic_myaddr, sizeof(current)))) {
        close();
        return finish(kIOReturnError);
    }
    // Use the actual firmware identity, including an accepted private MAC.
    // Never replace it with the factory MAC or change the user's MAC policy.
    auto result = nativeRegistration.setPacketQueuesEnabled(true);
    if (result == kIOReturnSuccess && ifnet_set_flags(ifnet, IFF_RUNNING, IFF_RUNNING))
        result = kIOReturnError;
    // The BSD output thread starts stopped and must be started before it drains
    // the output queue into outputPacket, which hands each frame to net80211's
    // if_snd -> if_start. The V2 setLinkStatus deliberately skipped this on the
    // assumption the Skywalk TX submission queue would carry it, but that queue has
    // no netif_queue wired, so every transmit was lost and the DHCP discover never
    // left the interface (Opkts stayed 0).
    if (result == kIOReturnSuccess && bsdInterface)
        bsdInterface->startOutputThread();
    if (result != kIOReturnSuccess) close();
    return finish(result);
}

// Called by the native connection owner on the shared workqueue. The Apple
// interface publishes controller/Skywalk link state itself; do not recurse
// through legacy setLinkStateGated or start the old BSD output pull thread.
IOReturn AirportItlwm::publishNativeLink(bool connected, UInt reason)
{
    if (!_fWorkloop || !_fWorkloop->inGate()) return kIOReturnCannotLock;
    if (!nativeAvailable() || !nativeInterfaceStarted || !fNetIf ||
        !nativeRegistration.prepared()) return kIOReturnNotReady;
    if (connected && !nativePortAvailable()) return kIOReturnNotReady;
    const auto dataPath = setNativeDataPath(connected);
    if (dataPath != kIOReturnSuccess) return dataPath;
    auto failed = [&](IOReturn error) {
        setNativeDataPath(false);
        fNetIf->setRunningState(false);
        fNetIf->setLinkState(kIO80211NetworkLinkDown, reason, false, 0);
        fNetIf->reportLinkStatus(0x80000001u, 0x80);
        return error;
    };
    ether_addr address = {};
    memcpy(address.octet, nativeJoin.completion.candidates[0].bssid, sizeof(address.octet));
    static_cast<IO80211InfraInterface *>(fNetIf)->setCurrentApAddress(connected ? &address : nullptr);
    const auto state = connected ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown;
    if (!fNetIf->setLinkState(state, reason, false, 0)) return failed(kIOReturnError);
    const auto running = fNetIf->setRunningState(connected);
    if (running != kIOReturnSuccess) return failed(running);
    // Infra::setLinkState ignores reportLinkStatus's IOReturn. Skywalk caches
    // state before posting, so bit 31 forces retry even when the cache already
    // contains the requested status. The explicit post must succeed as well.
    const auto reported = fNetIf->reportLinkStatus(0x80000000u | (connected ? 3u : 1u), 0x80);
    return reported == kIOReturnSuccess ? reported : failed(reported);
}
