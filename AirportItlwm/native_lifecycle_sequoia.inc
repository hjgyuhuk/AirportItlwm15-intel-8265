// Sequoia owns registration through system APIs; the Sonoma compatibility
// pointer substitution above is deliberately not compiled into this target.
void AirportItlwm::publishNativeRegistrationDiagnostics(const char *phase)
{
#if SEQUOIA_DIAGNOSTICS
    IOService *provider = getProvider();
    if (!provider) return;

    extern uint32_t itlwmKernelMatchLevel();
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeKernelMatchLevel", itlwmKernelMatchLevel(), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationPhase", phase ? phase : "unknown");
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationPrepared", nativeRegistration.prepared());
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeInterfaceStarted", nativeInterfaceStarted);
    // The concrete BSD publication transition is recorded separately by
    // AirportItlwmEthernetInterface; keep this snapshot independent of an
    // unexposed IOService registration predicate.
    // The controller pointer is allocated before BSD attachment. Treat the
    // child as published only when the family has supplied a real ifnet (and,
    // when available, Skywalk has recorded its BSD pointer), otherwise the
    // diagnostic would report success for a name-less enX shell.
    bool realBSD = bsdInterface && bsdInterface->getIfnet();
    if (fNetIf && fNetIf->mExpansionData)
        realBSD = realBSD || fNetIf->mExpansionData->fBSDInterface != nullptr;
    if (fNetIf && fNetIf->mExpansionData2)
        realBSD = realBSD || fNetIf->mExpansionData2->fBSDInterface != nullptr;
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDPublished", realBSD &&
                          nativeInterfaceStarted && phase &&
                          strcmp(phase, "bsd-registered") == 0);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationBoundary",
                          nativeRegistration.usedInfraBoundary()
                              ? "io80211-infra-ethernet"
                              : "generic-ethernet-only");

    auto pointer = [](const void *value) -> uint64_t {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
    };
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationInterface", pointer(fNetIf), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationPool", pointer(nativeRegistration.pool()), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationTxQueue", pointer(nativeRegistration.txQueue()), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationRxQueue", pointer(nativeRegistration.rxQueue()), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeInterfaceServicePublished", nativeInterfaceServicePublished);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRunningStateResult",
                          static_cast<UInt64>(nativeRunningStateResult), 32);

    void *networkInfo = nullptr;
    void *ethernetInfo = nullptr;
    void *networkBSD = nullptr;
    void *ethernetBSD = nullptr;
    if (fNetIf) {
        if (fNetIf->mExpansionData) {
            networkInfo = fNetIf->mExpansionData->fRegistrationInfo;
            networkBSD = fNetIf->mExpansionData->fBSDInterface;
        }
        if (fNetIf->mExpansionData2) {
            ethernetInfo = fNetIf->mExpansionData2->fRegistrationInfo;
            ethernetBSD = fNetIf->mExpansionData2->fBSDInterface;
        }
    }
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeNetworkRegistrationInfo", pointer(networkInfo), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeEthernetRegistrationInfo", pointer(ethernetInfo), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeNetworkBSDInterface", pointer(networkBSD), 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeEthernetBSDInterface", pointer(ethernetBSD), 64);

    // getBSDName() is a read-only accessor.  Apple returns an empty name until
    // BSD attachment; recording only its bounded length avoids retaining any
    // mutable string in the provider diagnostics.
    uint32_t nameLength = 0;
    if (fNetIf && nativeInterfaceStarted) {
        const char *name = fNetIf->getBSDName();
        if (name) {
            while (nameLength < IFNAMSIZ && name[nameLength] != '\0') ++nameLength;
        }
    }
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDNameLength", nameLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDAttachAttempts", nativeBSDAttachAttempts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDAttachLastResult",
                          static_cast<UInt64>(nativeBSDAttachLastResult), 32);
#else

#endif
}

IOReturn AirportItlwm::tryNativeBSDChildAttach()
{
    if (!bsdInterface) {
        nativeBSDAttachLastResult = kIOReturnNotReady;
        return nativeBSDAttachLastResult;
    }

    // A service notification can race this explicit hand-off. A child with a
    // real ifnet has already completed the only operation we need, so do not
    // call the private attach path a second time.
    if (bsdInterface->getIfnet()) {
        nativeBSDAttachLastResult = kIOReturnSuccess;
        return nativeBSDAttachLastResult;
    }

    if (nativeBSDAttachAttempts != UINT32_MAX)
        ++nativeBSDAttachAttempts;
    nativeBSDAttachLastResult = bsdInterface->attachToDataLinkLayer(0, nullptr);
    IOService *provider = getProvider();
    if (provider) {
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDAttachAttempts", nativeBSDAttachAttempts, 32);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDAttachLastResult",
                              static_cast<UInt64>(nativeBSDAttachLastResult), 32);
    }
    return nativeBSDAttachLastResult;
}

// Keep the kernel-only Apple boundary behind the registration helper's small
// callback.  This emits the real 24G830 symbol
// IO80211InfraInterface::registerInfraEthernetInterface and leaves the host
// registration fixture on its generic path.  The infra method itself performs
// only the audited role/MAC normalization before forwarding to Skywalk.
static IOReturn registerNativeInfraEthernet(
    IOSkywalkEthernetInterface *interface,
    IOSkywalkEthernetInterface::RegistrationInfo *info,
    IOSkywalkPacketQueue **queues, unsigned int count,
    IOSkywalkPacketBufferPool *txPool, IOSkywalkPacketBufferPool *rxPool)
{
    if (!interface || !info || !queues || count != 2 || !txPool || !rxPool)
        return kIOReturnBadArgument;
    return static_cast<IO80211InfraInterface *>(interface)->registerInfraEthernetInterface(
        info, queues, count, txPool, rxPool);
}

bool AirportItlwm::nativeAvailable()
{
    if (!nativePMLock) return false;
    IOLockLock(nativePMLock);
    const bool ready = nativeReady && !nativeStopping;
    IOLockUnlock(nativePMLock);
    return ready;
}

bool AirportItlwm::nativeRadioAvailable()
{
    if (!_fWorkloop || !_fWorkloop->inGate() || !nativePMLock || nativeRadioTransition) return false;
    IOLockLock(nativePMLock);
    const bool ready = nativeReady && !nativeStopping && pmPowerState == kPowerStateOn;
    IOLockUnlock(nativePMLock);
    return ready;
}

bool AirportItlwm::nativeHardwareQueryAvailable()
{
    if (!_fWorkloop || !_fWorkloop->inGate() || !nativePMLock) return false;
    IOLockLock(nativePMLock);
    const bool stopping = nativeStopping;
    IOLockUnlock(nativePMLock);
    // Static queries are also needed during interface startup, before ready.
    // Stop closes admission and drains this gate before releasing the HAL.
    return !stopping && nativeHalAttached && fHalService &&
        fHalService->get80211Controller() && fHalService->getDriverInfo();
}

IOReturn AirportItlwm::getNativeVersion(apple80211_version_data *data, bool driver)
{
    if (!data) return kIOReturnBadArgument;
    if (!_fWorkloop) return kIOReturnNotReady;
    struct Query { void *data; bool driver; } query = {data, driver};
    return _fWorkloop->runAction([](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<AirportItlwm *>(owner);
        if (!self->nativeHardwareQueryAvailable()) return kIOReturnNotReady;
        auto *query = static_cast<Query *>(arg);
        const char *firmware = self->fHalService->getDriverInfo()->getFirmwareVersion();
        if (!firmware) return kIOReturnNotReady;
        apple80211_version_data result = {};
        result.version = APPLE80211_VERSION;
        if (query->driver)
            snprintf(result.string, sizeof(result.string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, firmware);
        else
            snprintf(result.string, sizeof(result.string), "%s", firmware);
        result.string_len = static_cast<uint16_t>(strlen(result.string));
        memcpy(query->data, &result, sizeof(result));
        return kIOReturnSuccess;
    }, this, &query);
}

IOReturn AirportItlwm::getNativeCountry(apple80211_country_code_data *data)
{
    if (!data) return kIOReturnBadArgument;
    if (!_fWorkloop) return kIOReturnNotReady;
    return _fWorkloop->runAction([](OSObject *owner, void *data, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<AirportItlwm *>(owner);
        if (!self->nativeHardwareQueryAvailable()) return kIOReturnNotReady;
        const char *firmware = self->fHalService->getDriverInfo()->getFirmwareCountryCode();
        if (!firmware) return kIOReturnNotReady;

        // AX210 firmware commonly reports the world placeholder "ZZ".  On
        // Sequoia that value makes WCL mark every scan as restricted and
        // reject it with 0x10 before the delivered results reach CoreWLAN.
        // Match the legacy itlwm country selection order while keeping the
        // native query read-only: an explicit two-letter boot override wins,
        // then a country previously supplied by the compatibility path when
        // firmware is still ZZ, and finally the firmware value itself.
        char override[3] = {};
        PE_parse_boot_argn("itlwm_cc", override, sizeof(override));
        const auto validCountry = [](const char *cc) {
            return cc && ((cc[0] >= 'A' && cc[0] <= 'Z') || (cc[0] >= 'a' && cc[0] <= 'z')) &&
                   ((cc[1] >= 'A' && cc[1] <= 'Z') || (cc[1] >= 'a' && cc[1] <= 'z')) &&
                   cc[2] == '\0';
        };
        const char *country = firmware;
        if (validCountry(override)) {
            country = override;
        } else if (firmware[0] == 'Z' && firmware[1] == 'Z' &&
                   validCountry(self->geo_location_cc)) {
            country = self->geo_location_cc;
        }
        apple80211_country_code_data result = {};
        result.version = APPLE80211_VERSION;
        result.cc[0] = country[0] >= 'a' && country[0] <= 'z' ? country[0] - ('a' - 'A') : country[0];
        result.cc[1] = country[1] >= 'a' && country[1] <= 'z' ? country[1] - ('a' - 'A') : country[1];
        memcpy(data, &result, sizeof(result));
        return kIOReturnSuccess;
    }, this, data);
}

// Normal stages only publish memory samples. Console output runs on a
// separate diagnostic workloop, outside the HAL gate. The historical explicit
// itlwm_spin=N capture mode remains opt-in; it never initiates a reboot.
static int gItlwmSpinStage = -2;

static const char *gItlwmStageLabel = "00_no_stage";
// Black box on the provider node. The IOPCIEDeviceWrapper is registered and
// matched before AirportItlwm starts, and stays registered/active even when the
// controller never registers or its start returns false. Properties written
// there therefore survive a failed start, a hard hang and a power-off, and are
// readable with plain ioreg from the next boot - which the controller's own
// properties are not, since a failed service is released with them. Kext IOLog
// output is also not captured by the unified log on this system, so this is the
// only durable record of a start attempt.
static IOService *gItlwmStageProvider = nullptr;

// The newest stage alone cannot describe a failure: on the r102 boot "90_fail"
// replaced the label of the call that gave up, so the black box reported that a
// start had failed without saying where. Keep the last non-terminal label under
// its own key and publish both, so a graceful failure names its own frontier.
static const char *gItlwmLastOkStage = "00_no_stage";

static void itlwmRememberLastOkStage(const char *label)
{
    if (strcmp(label, "90_fail") == 0 || strcmp(label, "17_complete") == 0) return;
    __atomic_store_n(&gItlwmLastOkStage, label, __ATOMIC_RELAXED);
}

static void itlwmPublishStageOnProvider()
{
    IOService *provider = __atomic_load_n(&gItlwmStageProvider, __ATOMIC_RELAXED);
    if (!provider) return;
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeStage", __atomic_load_n(&gItlwmStageLabel, __ATOMIC_RELAXED));
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLastStage", __atomic_load_n(&gItlwmLastOkStage, __ATOMIC_RELAXED));
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeHal", gItlwmHalProgress.current());
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeEvents", gItlwmHalProgress.count(), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeHalResult", static_cast<uint32_t>(gItlwmHalProgress.lastResult()), 32);
    OSDictionary *iwx = OSDictionary::withCapacity(ItlBootProgress::DetailCount);
    if (iwx) {
        for (unsigned i = 0; i < ItlBootProgress::DetailCount; ++i) {
            OSNumber *value = OSNumber::withNumber(gItlwmHalProgress.iwxValue(
                static_cast<ItlBootProgress::IwxDetail>(i)), 32);
            if (value) { iwx->setObject(ItlBootProgress::iwxName(i), value); value->release(); }
        }
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeIwx", iwx);
        iwx->release();
    }
}

void AirportItlwm::publishNativeScanDiagnostics()
{
#if SEQUOIA_DIAGNOSTICS
    IOService *provider = __atomic_load_n(&gItlwmStageProvider, __ATOMIC_RELAXED);
    if (!provider) return;
    const auto &d = nativeScan.diagnostics;
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheFrames", d.cache.frames, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheEligibilityRejected", d.cache.eligibilityRejected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheFrameRejected", d.cache.frameRejected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheFilterRejected", d.cache.filterRejected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheChannelRejected", d.cache.channelRejected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheAllocationFailed", d.cache.allocationFailed, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheCopyFailed", d.cache.copyFailed, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheLayoutFailed", d.cache.layoutFailed, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheParserCalls", d.cache.parserCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheNodeHits", d.cache.nodeHits, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheNodesAtScanEnd", d.cache.nodesAtScanEnd, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheNodeLimit", d.cache.nodeLimit, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheLastFrameLength", d.cache.lastFrameLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequests", d.requests, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastRequestResult", d.lastRequestResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestedChannels", d.requestedChannels, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestActive", d.requestActive, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWorkerStarts", d.workerStarts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanEnableResult", d.enableResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanNativeResult", d.nativeScanResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanFailStage", d.nativeScanFailStage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanFailChannel", d.nativeScanFailChannel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanChannelCount", d.nativeScanChannelCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWireCount", d.nativeScanWireCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWireCapacity", d.nativeScanWireCapacity, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanTxAnt", d.nativeScanTxAnt, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRxAnt", d.nativeScanRxAnt, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanSourceValid", d.nativeScanSourceValid, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUsableCount", d.nativeScanUsableCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanSubmitReject", d.nativeScanSubmitReject, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUmac", d.nativeScanUmac, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanKeepRadio", d.nativeScanKeepRadio, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastTerminalCode", d.lastTerminalCode, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRawTerminalCode", d.rawTerminalCode, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRawIterationCode", d.rawIterationCode, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRawTerminalStatus", d.rawTerminalStatus, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCoverageRequested", d.coverageRequested, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCoverageIterations", d.coverageIterations, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCoverageScanned", d.coverageScanned, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCoverageTerminalStatus", d.coverageTerminalStatus, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCoverageIterationStatus", d.coverageIterationStatus, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCoverageMalformed", d.coverageMalformed, 32);
    {
        char list[192] = {};
        size_t used = 0;
        for (uint32_t i = 0; i < d.nativeScanUsableCount && used + 6 < sizeof(list); ++i) {
            const int written = snprintf(list + used, sizeof(list) - used, "%u%c",
                d.nativeScanUsableList[i], i + 1 < d.nativeScanUsableCount ? ',' : 0);
            if (written < 0) break;
            used += static_cast<size_t>(written) < sizeof(list) - used
                ? static_cast<size_t>(written) : sizeof(list) - used - 1;
        }
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUsableList", list);
    }
    {
        char list[192] = {};
        size_t used = 0;
        for (uint32_t i = 0; i < d.nativeScanChannelCount && used + 6 < sizeof(list); ++i) {
            const int written = snprintf(list + used, sizeof(list) - used, "%u%c",
                d.nativeScanChannelList[i], i + 1 < d.nativeScanChannelCount ? ',' : 0);
            if (written < 0) break;
            used += static_cast<size_t>(written) < sizeof(list) - used
                ? static_cast<size_t>(written) : sizeof(list) - used - 1;
        }
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanChannelList", list);
    }
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanSubmissions", d.submissions, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastSubmission", d.lastSubmission, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanNotifications", d.notifications, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastNotification", d.lastNotification, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastNotificationBytes", d.lastNotificationBytes, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanTerminals", d.terminals, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastOutcome", d.lastOutcome, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsQueued", d.beaconsQueued, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsRejected", d.beaconsRejected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanDeliveryResults", d.deliveryResults, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanDeliveryCompletions", d.deliveryCompletions, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanDeliveryRetries", d.deliveryRetries, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanDeliveryNotReady", d.deliveryNotReady, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWclResultPosts", d.wclResultPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWclFsmResultPosts", d.wclFsmResultPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWclCacheResultPosts", d.wclCacheResultPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWclCompletionPosts", d.wclCompletionPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLegacyDone", d.legacyScanDone, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLegacyCompletionRoute", d.legacyCompletionRoute, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLegacyCompletionAttempts", d.legacyCompletionAttempts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLegacyCompletionInterfacePosts", d.legacyCompletionInterfacePosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLegacyCompletionControllerPosts", d.legacyCompletionControllerPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacyScanRequests",
                          __atomic_load_n(&nativeLegacyScanRequests, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacyScanAccepted",
                          __atomic_load_n(&nativeLegacyScanAccepted, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacyScanLastCommand",
                          __atomic_load_n(&nativeLegacyScanLastCommand, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacyScanLastResult",
                          __atomic_load_n(&nativeLegacyScanLastResult, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacySetScanRequests",
                          __atomic_load_n(&nativeLegacySetScanRequests, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacySetScanMultipleRequests",
                          __atomic_load_n(&nativeLegacySetScanMultipleRequests, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLegacySetScanLastResult",
                          __atomic_load_n(&nativeLegacySetScanLastResult, __ATOMIC_RELAXED), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastDelivery", d.lastDelivery, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanNotReadyRequests", d.notReadyRequests, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUnsupportedRequests", d.unsupportedRequests, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastUnsupportedSelector", d.lastUnsupportedSelector, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUnsupportedSelector0", d.unsupportedSelectors[0], 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUnsupportedSelector1", d.unsupportedSelectors[1], 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUnsupportedSelector2", d.unsupportedSelectors[2], 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanUnsupportedSelector3", d.unsupportedSelectors[3], 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanConfigurationSetters", d.configurationSetters, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastConfigurationSetter", d.lastConfigurationSetter, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheRequests", d.cacheRequests, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheSuccesses", d.cacheSuccesses, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheNotReady", d.cacheNotReady, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheLastResult", d.cacheLastResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheLastCount", d.cacheLastCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanCacheLastValidEntries", d.cacheLastValidEntries, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanPending", static_cast<uint32_t>(nativeScan.delivery.pending()), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanWorkerActive", nativeScan.workerActive());
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanInventoryCount", d.inventoryCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanInventoryLimit", d.inventoryLimit, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRejectStage", d.rejectStage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRejectChannel", d.rejectChannel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanPlanResult", d.planResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanControllerState", d.controllerState, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanControllerHasBss", d.controllerHasBss, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestVersion", d.requestVersion, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestType", d.requestScanType, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestChannels", d.requestChannelCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestSsids", d.requestSsidCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestPrivateMac", d.requestPrivateMac, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestFlags", d.requestScanFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestHomeAway", d.requestHomeAwayTime, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestControl0c", d.requestControl0c, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestBssType", d.requestBssType, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestActiveDwell", d.requestActiveDwell, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestPassiveDwell", d.requestPassiveDwell, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestRestTime", d.requestRestTime, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestShortSSID", d.requestShortSSID, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestShapeSequence", d.requestShapeSequence, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestSsidLength", d.requestSsidLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestFirstChannel", d.requestFirstChannel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanRequestFirstChannelFlags", d.requestFirstChannelFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanPlanReason", d.planReason, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanPlanRejectCount", d.planRejectCount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanPlanRejectReasons", d.planRejectReasons, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastPlanFailureRequest", d.lastPlanFailureRequest, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastPlanFailureReason", static_cast<uint32_t>(d.lastPlanFailure.reason), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastPlanFailureChannelIndex", d.lastPlanFailure.channelIndex, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastPlanFailureChannel", d.lastPlanFailure.channel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastPlanFailureFlags", d.lastPlanFailure.requestedFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastPlanFailureInventoryFlags", d.lastPlanFailure.inventoryFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanAcceptedRequest", d.acceptedRequest, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanProbeChannels", d.probeChannels, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanPassiveFallbacks", d.passiveFallbacks, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsDecoded", d.beaconsDecoded, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsDecodeRejected", d.beaconsDecodeRejected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsFiltered", d.beaconsFiltered, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithSSID", d.beaconsWithSSID, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithIE", d.beaconsWithInformationElements, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsQueuedWithSSID", d.beaconsQueuedWithSSID, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconSSIDLength", d.lastBeaconSSIDLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconIELength", d.lastBeaconIELength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconChannel", d.lastBeaconChannel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconFlags", d.lastBeaconFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithDS", d.beaconsWithDS, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithDSBadLength", d.beaconsWithDSBadLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithIBSS", d.beaconsWithIBSS, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithIBSSBadLength", d.beaconsWithIBSSBadLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithQBSS", d.beaconsWithQBSS, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithQBSSBadLength", d.beaconsWithQBSSBadLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithVHT", d.beaconsWithVHT, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithVHTBadLength", d.beaconsWithVHTBadLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBeaconsWithMalformedIE", d.beaconsWithMalformedIE, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconIECount", d.lastBeaconIECount, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconFirstIE", d.lastBeaconFirstIE, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconSSIDIELength", d.lastBeaconSSIDIELength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconDSLength", d.lastBeaconDSLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconIBSSLength", d.lastBeaconIBSSLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconQBSSLength", d.lastBeaconQBSSLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanLastBeaconVHTLength", d.lastBeaconVHTLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanResultRoute", d.resultRoute, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeDebugScanRoute", d.runtimeRoute, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeDebugScanDoneRoute", d.runtimeDoneRoute, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueScheduleResult", d.glueScheduleResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueScheduleObserved", d.glueScheduleObserved, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueScheduleGate", d.glueScheduleGate, 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueWakeByteAfterSchedule", d.glueWakeByteAfterSchedule, 32);
    // Keep the breadcrumb truthful when the route is selected by boot arg:
    // fsm posts Apple's 0xc9 beacon entry, cache posts 0xce, and dual emits
    // both so a single scalar cannot claim one route.
    uint32_t resultMessage = UINT32_MAX;
    if (d.resultRoute == 1) resultMessage = SequoiaWCL::kBeaconMessage;
    else if (d.resultRoute == 0) resultMessage = SequoiaWCL::kScanResultMessage;
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanResultMessage", resultMessage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeLastResultMsgIndex", d.lastResultMsgIndex, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclDirectPosts", d.wclDirectPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclDirectLastResult", d.wclDirectLastResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclDirectFailures", d.wclDirectFailures, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclDirectFirstFailure", d.wclDirectFirstFailure, 32);
    // r224: firmware-sequence markers (0x7d start / 0x7e complete).
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclStartPosts", d.wclStartPosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclStartLastResult", d.wclStartLastResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCompleteFirmwarePosts", d.wclCompleteFirmwarePosts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCompleteFirmwareLastResult", d.wclCompleteFirmwareLastResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanSeqStarted", d.scanSeqStarted);
    // r226: these were updated on every request since r225 but absent from the
    // publish whitelist, so the r225 boot could not observe the gate state or
    // the Q9 cache-count readback at all.
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocScanEnabled", d.assocScanEnabled, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocScans", d.assocScans, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocApChannel", d.assocApChannel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocBeaconsQueued", d.assocBeaconsQueued, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocBeaconChannels", d.assocBeaconChannels, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocWithheld", d.assocWithheld, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocWithheldReason", d.assocWithheldReason, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocLastOutcome", d.assocLastOutcome, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocBeaconChannelMask0", d.assocBeaconChannelMask[0], 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocBeaconChannelMask1", d.assocBeaconChannelMask[1], 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocBeaconChannelMask2", d.assocBeaconChannelMask[2], 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeAssocBeaconChannelMask3", d.assocBeaconChannelMask[3], 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCacheCountBefore", d.wclCacheCountBefore, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCacheCountAfter", d.wclCacheCountAfter, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCacheReadError", d.wclCacheReadError, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCacheStorePtr", d.wclCacheStorePtr, 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCacheQueuePtr", d.wclCacheQueuePtr, 64);
    // r227: WCL capability-query acceptance. wclQueryGate is captured once at
    // init (1 = on, 0 = itlwm_wcl_queries=off fallback) and the counters show
    // which of the three GETs airportd actually asked for.
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclQueryGate", d.wclQueryGate, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclQueryMaxNss", d.wclQueryMaxNss, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclQueryTrafficCounters", d.wclQueryTrafficCounters, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclQueryChannelsInfo", d.wclQueryChannelsInfo, 32);
#else

#endif
}

void AirportItlwm::captureNativeJoinDiagnostics(bool reset)
{
#if SEQUOIA_DIAGNOSTICS
    auto *hal = fHalService ? fHalService->getNativeHal() : nullptr;
    if (!hal || !_fWorkloop->inGate()) return;
    auto *ic = hal->nativeController();
    if (reset && ic) ic->ic_eapol_diagnostic = {};
    nativeJoin.diagnostics.handshake = ItlEapol::capture(ic, ic ? &ic->ic_if : nullptr, hal->nativePortReady());
#else

#endif
}

void AirportItlwm::publishNativeJoinDiagnostics()
{
#if SEQUOIA_DIAGNOSTICS
    IOService *provider = __atomic_load_n(&gItlwmStageProvider, __ATOMIC_RELAXED);
    if (!provider) return;
    const auto &d = nativeJoin.diagnostics;
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRequests", d.requests, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveRequests", d.leaveRequests, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveResult", d.leaveResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveStage", d.leaveStage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveFailures", d.leaveFailures, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveLastFailure", d.leaveLastFailure, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveLastFailureStage", d.leaveLastFailureStage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveLastFailureRequest", d.leaveLastFailureRequest, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveWasBusy", d.leaveWasBusy, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveDrained", d.leaveDrained, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveBusyAfter", d.leaveBusyAfter, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLeaveTxError", d.leaveTxError, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLastRequestResult", d.lastRequestResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPolicyResult", d.policyResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPolicyMask", d.policyMask, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinControls1e0", d.controls1e0, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinControls1e4", d.controls1e4, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinControls1e8", d.controls1e8, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinControls210", d.controls210, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinControls3d0", d.controls3d0, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinBatterySaveMode", d.batterySaveMode, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinExtraKeyLength", d.extraKeyLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinExtraIELength", d.extraIELength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinWorkerStarts", d.workerStarts, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEnableResult", d.enableResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinStartResult", d.startResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCandidateMask", d.candidate.mask, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinAdmissionStage", d.candidate.stage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCandidateNodes", d.candidate.nodes, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCandidateSSIDLength", d.candidate.ssidLength, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCandidateChannel", d.candidate.channel, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCandidateAgeMs", d.candidate.ageMs, 64);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinAuthEvents", d.authEvents, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinAssocEvents", d.assocEvents, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinAuthStatus", d.authStatus, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinAssocStatus", d.assocStatus, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinStateError", d.stateError, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinFinalError", d.finalError, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinAssociationPosted", d.associationPosted, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCompletionPosted", d.completionPosted, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCompletionStatus", d.completionStatus, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinCompletionPostResult", d.completionPostResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkUpPosted", d.linkUpPosted, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkDownPosted", d.linkDownPosted, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkPostResult", d.linkPostResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkCalls", d.linkCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkResult", d.linkResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkFlags", d.linkFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinBssCalls", d.bssCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinBssResult", d.bssResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinExtBssCalls", d.extBssCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinExtBssResult", d.extBssResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinFastLaneCalls", d.fastLaneCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinFastLaneResult", d.fastLaneResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinFastLaneFlags", d.fastLaneFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinLinkRequested", d.linkRequested, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinConnected", d.connected, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinDrained", d.drained, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRejectStage", d.rejectStage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolRx", d.handshake.eapol.rx, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolM1", d.handshake.eapol.rxM1, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolM3", d.handshake.eapol.rxM3, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolM3Stage", d.handshake.eapol.m3Stage, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolDeauthReason", d.handshake.eapol.deauthReason, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolGroup", d.handshake.eapol.rxGroup, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolM2Attempts", d.handshake.eapol.txM2, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolM4Attempts", d.handshake.eapol.txM4, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolGroupTxAttempts", d.handshake.eapol.txGroup, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinEapolTxError", d.handshake.eapol.txError, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPairKeyCalls", d.handshake.eapol.pairCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPairKeyResult", d.handshake.eapol.pairResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinGroupKeyCalls", d.handshake.eapol.groupCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinGroupKeyResult", d.handshake.eapol.groupResult, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinBsdState", d.handshake.state, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinSupplicantState", d.handshake.supplicant, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeNet80211State",
                          static_cast<UInt32>(fHalService->get80211Controller()->ic_state), 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeTxOutputCalls", nativeTxOutputCalls, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeTxOutputGated", nativeTxOutputGated, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPortValid", d.handshake.port, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPortReady", d.handshake.ready, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinNodeFlags", d.handshake.nodeFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinIfFlags", d.handshake.ifFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinBsdLinkState", d.handshake.linkState, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPairKeyFlags", d.handshake.pairFlags, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinPairKeyPresent", d.handshake.pairPresent, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinGroupKeyMask", d.handshake.groupMask, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxKeys", d.handshake.rxKeys, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxReplay", d.handshake.replay, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxBadMic", d.handshake.badMic, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxNoMbuf", d.handshake.noMbuf, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxWrongBss", d.handshake.wrongBss, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxWrongDir", d.handshake.wrongDir, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxDecap", d.handshake.decap, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxUnencrypted", d.handshake.unencrypted, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeJoinRxDecrypt", d.handshake.decrypt, 32);
#else

#endif
}

// Breadcrumb in NVRAM. A wedged boot ends in a hard reset that destroys the
// IORegistry black box and, when the console freezes mid-print, leaves no photo
// worth reading either. NVRAM is the only channel that survives it, so the
// frontier is mirrored there: after the next internal boot
// `nvram -p | grep itlwm` (or `ioreg -c IODTNVRAM`) reports exactly where the
// start stopped, with no recording and no guesswork. Best effort only: a failure
// to resolve IODTNVRAM must never delay or fail the start, and the resolve is
// bounded by a short timeout.
static IOService *gItlwmNvramService = nullptr;
static uint64_t gItlwmNvramNextTry = 0;
static char gItlwmNvramLast[96] = {};

// IOKit class matching is exact, and this system's NVRAM service is an
// AppleEFINVRAM (a subclass of IODTNVRAM, kext com.apple.driver.AppleEFINVRAM).
// Matching the base class name never succeeds, and giving up after one attempt
// silently disabled the whole channel, so try both names and retry.
static const char *const kItlwmNvramClasses[] = {"AppleEFINVRAM", "IODTNVRAM"};

static IOService *itlwmResolveNvramService()
{
    IOService *existing = __atomic_load_n(&gItlwmNvramService, __ATOMIC_RELAXED);
    if (existing) return existing;
    const uint64_t now = mach_absolute_time();
    if (now < gItlwmNvramNextTry) return nullptr;
    // One attempt per second at most, and each attempt is bounded, so a missing
    // service costs the start path almost nothing.
    gItlwmNvramNextTry = now + 1000000000ULL;
    for (const char *name : kItlwmNvramClasses) {
        OSDictionary *match = IOService::serviceMatching(name);
        if (!match) continue;
        IOService *service = IOService::waitForMatchingService(match, 50ULL * 1000000ULL);
        match->release();
        if (service) {
            __atomic_store_n(&gItlwmNvramService, service, __ATOMIC_RELAXED);
            return service;
        }
    }
    return nullptr;
}

static void itlwmWriteNvramBreadcrumb()
{
    IOService *nvram = itlwmResolveNvramService();
    if (!nvram) return;
    char text[96];
    snprintf(text, sizeof(text), "%s hal=%s",
             __atomic_load_n(&gItlwmStageLabel, __ATOMIC_RELAXED), gItlwmHalProgress.current());
    if (strncmp(text, gItlwmNvramLast, sizeof(gItlwmNvramLast)) == 0) return;
    strlcpy(gItlwmNvramLast, text, sizeof(gItlwmNvramLast));
    nvram->setProperty("itlwm-stage", text);
}

#if SEQUOIA_DIAGNOSTICS
#define ITLWM_NATIVE_STAGE(num, label) do { \
    __atomic_store_n(&gItlwmStageLabel, label, __ATOMIC_RELAXED); \
    itlwmRememberLastOkStage(label); \
    itlwmPublishStageOnProvider(); \
    itlwmWriteNvramBreadcrumb(); \
    /* Console trace for the early start region only. A stalled start blocks the
       IOKit configuration thread before the reporter's first two-second tick, so
       these in-place lines are the only record that survives a hard power-off;
       the later stages rely on the reporter and the provider black box. */ \
    if ((num) <= 120) ITLWM_LOG("itlwm-native: stage %s\n", label); \
} while (0)
#else
// A release build records no stage: no property, no breadcrumb in the
// non-volatile store, no console line.
#define ITLWM_NATIVE_STAGE(num, label) do { } while (0)
#endif

#if SEQUOIA_DIAGNOSTICS
// The capture mode stops the driver at a stage so a console log can be
// taken. It is opt-in and never initiates a reboot.
#define ITLWM_SPIN_CAPTURE_STAGE(num, label) do { \
    if (gItlwmSpinStage < 0) { \
        int _s = 0; \
        if (PE_parse_boot_argn("itlwm_spin", &_s, sizeof(_s))) gItlwmSpinStage = _s; \
        else gItlwmSpinStage = -1; \
    } \
    if (gItlwmSpinStage >= 0 && (num) >= gItlwmSpinStage) { \
        for (;;) { \
            ITLWM_LOG("itlwm-native: SPINNING at stage %s for capture (itlwm_spin=%d)\n", label, gItlwmSpinStage); \
            IOSleep(2000); \
        } \
    } \
} while (0)
#else
#define ITLWM_SPIN_CAPTURE_STAGE(num, label) do { } while (0)
#endif

void AirportItlwm::nativeStageReport(OSObject *owner, IOTimerEventSource *sender)
{
#if SEQUOIA_DIAGNOSTICS
    auto *self = static_cast<AirportItlwm *>(owner);
    if (sender != self->nativeBootTimer) return;
    const unsigned count = gItlwmHalProgress.count();
    const char *stage = __atomic_load_n(&gItlwmStageLabel, __ATOMIC_RELAXED);
    // The trial boots end in a hard power-off where the console is the ONLY
    // surviving channel, and the post-startup phase exists only as a published
    // property, so carry those values in this same line. Reading our own
    // properties is safe here; never chase HAL/object hardware pointers.
    const char *phase = "none";
    unsigned scanError = 0, joinError = 0;
    // This is a static callback, so go through the owner; qualify the base class
    // because a derived interface declares copyProperty(const OSSymbol *), which
    // hides the const char * overload of IORegistryEntry.
    OSString *publishedPhase = OSDynamicCast(OSString,
        self->IORegistryEntry::copyProperty("SequoiaNativeStage"));
    if (publishedPhase) {
        const char *value = publishedPhase->getCStringNoCopy();
        if (value) phase = value;
    }
    OSNumber *publishedScan = OSDynamicCast(OSNumber,
        self->IORegistryEntry::copyProperty("SequoiaNativeScanError"));
    if (publishedScan) scanError = publishedScan->unsigned32BitValue();
    OSNumber *publishedJoin = OSDynamicCast(OSNumber,
        self->IORegistryEntry::copyProperty("SequoiaNativeJoinError"));
    if (publishedJoin) joinError = publishedJoin->unsigned32BitValue();
    ITLWM_LOG("itlwm-native: boot tick=%u events=%u stage=%s hal=%s phase=%s scan=%u join=%u\n",
          ++self->nativeBootTicks, count, stage, gItlwmHalProgress.current(),
          phase, scanError, joinError);
    // Mirror onto the provider node, which survives a failed controller start
    // in this boot. IORegistry does not survive a hard power-off or reset.
    IOService *stageProvider = __atomic_load_n(&gItlwmStageProvider, __ATOMIC_RELAXED);
    if (stageProvider) {
        ITLWM_PUBLISH_DIAGNOSTIC(stageProvider, "ItlwmNativePhase", phase);
        ITLWM_PUBLISH_DIAGNOSTIC(stageProvider, "ItlwmNativeScan", static_cast<uint64_t>(scanError), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(stageProvider, "ItlwmNativeJoin", static_cast<uint64_t>(joinError), 32);
    }
    itlwmPublishStageOnProvider();
    // Refresh the breadcrumb from the reporter as well, so a frontier that stops
    // moving *between* stages (the case a hard reset hides) is still recorded.
    itlwmWriteNvramBreadcrumb();
    if (publishedPhase) publishedPhase->release();
    if (publishedScan) publishedScan->release();
    if (publishedJoin) publishedJoin->release();
    // A short history distinguishes a frontier that advances between samples
    // from a stationary one. Never chase object/hardware pointers here.
    if (count != self->nativeBootLastEvents) {
        const unsigned start = count > 4 ? count - 4 : 0;
        for (unsigned i = start; i < count; ++i) {
            const char *label = gItlwmHalProgress.sample(i);
            if (label) ITLWM_LOG("itlwm-native: recent[%u]=%s\n", i, label);
        }
        self->nativeBootLastEvents = count;
    }
    // Keep reporting after the start path has settled: a hard hang happens
    // during association, and the frozen screen is the only record of it. Once
    // settled the rate drops so an ordinary working boot stays readable.
    const bool settled = strcmp(stage, "17_complete") == 0 || strcmp(stage, "90_fail") == 0;
    if (settled && !self->nativeBootSettled) self->nativeBootSettled = true;
    // Diagnostics must not turn a stalled startup into a machine panic.
    if (stage != self->nativeStallStage || count != self->nativeStallEvents) {
        self->nativeStallStage = stage;
        self->nativeStallEvents = count;
        self->nativeStallTicks = 0;
    } else {
        ++self->nativeStallTicks;
    }
    if (!self->nativeBootSettled && self->nativeStallTicks >= 13) {
        IOService *provider = self->getProvider();
        if (provider) {
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeStalled", true);
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeStallSeconds", self->nativeStallTicks * 2, 32);
        }
    }
    sender->setTimeoutMS(self->nativeBootSettled ? 10000 : 2000);
#else

#endif
}

bool AirportItlwm::startNativeStageReporter()
{
#if SEQUOIA_DIAGNOSTICS
    // IOWorkLoop::workLoop() allocates a new loop. Do not reuse _fWorkloop or
    // the compatibility layer's gWorkLoop, where attach holds the HAL gate.
    if (nativeBootWorkLoop || nativeBootTimer) return false;
    nativeBootTicks = nativeBootLastEvents = 0;
    nativeBootWorkLoop = IOWorkLoop::workLoop();
    if (!nativeBootWorkLoop) return false;
    nativeBootTimer = IOTimerEventSource::timerEventSource(this, nativeStageReport);
    if (!nativeBootTimer || nativeBootWorkLoop->addEventSource(nativeBootTimer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(nativeBootTimer);
        OSSafeReleaseNULL(nativeBootWorkLoop);
        return false;
    }
    nativeBootTimer->setTimeoutMS(2000);
    return true;
#else
    return false;
#endif
}

void AirportItlwm::stopNativeStageReporter()
{
#if SEQUOIA_DIAGNOSTICS
    if (nativeBootTimer && nativeBootWorkLoop) {
        // Drain an in-flight callback before releasing its borrowed owner.
        // A sleeping detached kernel thread would instead outlive this object
        // and could still execute module code after the kext becomes unloadable.
        nativeBootWorkLoop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<AirportItlwm *>(owner);
            self->nativeBootTimer->cancelTimeout();
            self->nativeBootTimer->disable();
            return kIOReturnSuccess;
        }, this);
        nativeBootWorkLoop->removeEventSource(nativeBootTimer);
    }
    OSSafeReleaseNULL(nativeBootTimer);
    OSSafeReleaseNULL(nativeBootWorkLoop);
#else

#endif
}

// r230: bounded sampler for the Skywalk BSD client's in-flight start. See
// itlwmPublishBsdClientProbeSample for why each field is read. Absolute sample
// times in milliseconds: dense at first so an immediate failure in the pool
// compare or in registerNexusControllerProvider is still bracketed, then sparse
// to cover the 50x IOSleep(100) retry loop (about five seconds), the service
// publication that follows a success, and the teardown after a failure.
static const uint16_t kNativeBsdProbeOffsets[] = {
    5, 10, 20, 40, 80, 150, 300, 500, 800, 1200,
    2000, 3000, 4000, 5000, 6000, 7000, 9000, 12000,
};

bool AirportItlwm::startNativeBsdProbe()
{
#if SEQUOIA_DIAGNOSTICS
    // Dedicated loop for the same reason as the stage reporter: the
    // controller's loop may be busy while the client's start runs elsewhere.
    if (nativeBsdProbeWorkLoop || nativeBsdProbeTimer) return false;
    nativeBsdProbeWorkLoop = IOWorkLoop::workLoop();
    if (!nativeBsdProbeWorkLoop) return false;
    nativeBsdProbeTimer = IOTimerEventSource::timerEventSource(this, nativeBsdProbeAction);
    if (!nativeBsdProbeTimer ||
        nativeBsdProbeWorkLoop->addEventSource(nativeBsdProbeTimer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(nativeBsdProbeTimer);
        OSSafeReleaseNULL(nativeBsdProbeWorkLoop);
        return false;
    }
    nativeBsdProbeSeq = 1;
    nativeBsdProbeTimer->setTimeoutMS(kNativeBsdProbeOffsets[0]);
    return true;
#else
    return false;
#endif
}

void AirportItlwm::stopNativeBsdProbe()
{
#if SEQUOIA_DIAGNOSTICS
    if (nativeBsdProbeTimer && nativeBsdProbeWorkLoop) {
        // Drain an in-flight callback before releasing its borrowed owner.
        nativeBsdProbeWorkLoop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<AirportItlwm *>(owner);
            if (self->nativeBsdProbeTimer) {
                self->nativeBsdProbeTimer->cancelTimeout();
                self->nativeBsdProbeTimer->disable();
            }
            self->nativeBsdProbeSeq = 0;
            return kIOReturnSuccess;
        }, this);
        nativeBsdProbeWorkLoop->removeEventSource(nativeBsdProbeTimer);
    }
    OSSafeReleaseNULL(nativeBsdProbeTimer);
    OSSafeReleaseNULL(nativeBsdProbeWorkLoop);
    nativeBsdProbeSeq = 0;
#else

#endif
}

void AirportItlwm::nativeBsdProbeAction(OSObject *owner, IOTimerEventSource *timer)
{
#if SEQUOIA_DIAGNOSTICS
    auto *self = static_cast<AirportItlwm *>(owner);
    if (!self || timer != self->nativeBsdProbeTimer || !self->nativeBsdProbeSeq) return;
    IOService *provider = self->getProvider();
    // Same block-scope declaration the other probes use.
    void itlwmPublishBsdClientProbeSample(IOService *target, void *interface, unsigned seq);
    if (provider && self->fNetIf)
        itlwmPublishBsdClientProbeSample(provider, self->fNetIf, self->nativeBsdProbeSeq);
    constexpr unsigned count = sizeof(kNativeBsdProbeOffsets) / sizeof(kNativeBsdProbeOffsets[0]);
    const unsigned seq = self->nativeBsdProbeSeq;
    if (seq >= count) {
        self->nativeBsdProbeSeq = 0;
        if (provider) ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBsdProbeDone", true);
        return;
    }
    ++self->nativeBsdProbeSeq;
    timer->setTimeoutMS(kNativeBsdProbeOffsets[seq] - kNativeBsdProbeOffsets[seq - 1]);
#else

#endif
}

// This is not a diagnostic. WCL waits for the driver-available envelope
// before it starts a scan or an association; a driver that never posts it can
// still be listed but never joins.
void AirportItlwm::announceNativeDriverAvailable(uint32_t phase)
{
    // WCL expects the complete driver-available envelope: the packed
    // apple80211 payload followed by the 0x40-byte bulletin prefix used by
    // Sequoia's other WCL messages. The interface copies the bytes while this
    // call runs, so a stack envelope is sufficient and cannot outlive the
    // driver.
    struct Announcement {
        apple80211_driver_available_data data;
        uint8_t prefix[0x40];
    } announcement{};
    announcement.data.event = 1;
    announcement.data.avaliable = 1;
    static_assert(sizeof(Announcement) == 0xf8, "driver-available message size");
    if (fNetIf) {
        fNetIf->postMessage(APPLE80211_M_DRIVER_AVAILABLE, &announcement,
                            sizeof(announcement), true);
        if (nativeDriverAvailableAnnouncements != UINT32_MAX)
            ++nativeDriverAvailableAnnouncements;
        nativeDriverAvailableLastPhase = phase;
    }
#if SEQUOIA_DIAGNOSTICS
    IOService *provider = __atomic_load_n(&gItlwmStageProvider, __ATOMIC_RELAXED);
    if (provider) {
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeDriverAvailableAnnouncements",
                              nativeDriverAvailableAnnouncements, 32);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeDriverAvailableLastPhase",
                              nativeDriverAvailableLastPhase, 32);
    }
    ITLWM_LOG("itlwm-native: announced driver availability phase=%u count=%u (%zu bytes)\n",
          phase, nativeDriverAvailableAnnouncements, sizeof(announcement));
#endif
}

static void itlwmPublishBootProgress(IOService *service)
{
    ITLWM_PUBLISH_DIAGNOSTIC(service, "SequoiaNativeBootStage", __atomic_load_n(&gItlwmStageLabel, __ATOMIC_RELAXED));
    ITLWM_PUBLISH_DIAGNOSTIC(service, "SequoiaNativeBootHal", gItlwmHalProgress.current());
    const unsigned end = gItlwmHalProgress.count();
    ITLWM_PUBLISH_DIAGNOSTIC(service, "SequoiaNativeBootEvents", end, 32);
    OSArray *history = OSArray::withCapacity(ItlBootProgress::capacity);
    if (!history) return;
    const unsigned start = end > ItlBootProgress::capacity ? end - ItlBootProgress::capacity : 0;
    for (unsigned i = start; i < end; ++i) {
        const char *label = gItlwmHalProgress.sample(i);
        OSString *value = label ? OSString::withCString(label) : nullptr;
        if (value) { history->setObject(value); value->release(); }
    }
    ITLWM_PUBLISH_DIAGNOSTIC(service, "SequoiaNativeBootRecentHalSamples", history);
    history->release();
}

bool AirportItlwm::startNative(IOService *provider)
{
    // Publish into the provider before anything can fail: it is the only node
    // that outlives a failed start.
    __atomic_store_n(&gItlwmStageProvider, provider, __ATOMIC_RELAXED);
    ITLWM_NATIVE_STAGE(10, "01_entry");
    auto *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper || !wrapper->fHalService || !pciNub || !_fWorkloop ||
        _fCommandGate || nativeStopping || nativeSuperStarted) {
        // Name the exact failing precondition. With no console capture and no
        // unified-log capture for kext IOLog, this note is the only record.
        if (provider) {
            char note[192];
            snprintf(note, sizeof(note),
                     "wrapper=%d hal=%d nub=%d loop=%d gate=%d stopping=%d started=%d",
                     wrapper != nullptr, wrapper && wrapper->fHalService != nullptr,
                     pciNub != nullptr, _fWorkloop != nullptr, _fCommandGate != nullptr,
                     nativeStopping, nativeSuperStarted);
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeNote", note);
        }
        ITLWM_NATIVE_STAGE(20, "02_precondition_failed");
        return false;
    }
    const IONetworkMedium *primary = nullptr;
    ether_addr address = {};
    int bootValue = 0;
    bool systemStarted = false;
    UInt8 interfaceBuiltIn = 0;
    char wclCompat[8] = {};
    nativeWclCompatR162 = PE_parse_boot_argn("itlwm_wcl_compat", wclCompat,
                                             sizeof(wclCompat)) &&
                          !strcmp(wclCompat, "r162");
    if (provider)
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclCompatR162", nativeWclCompatR162);
    IOReturn registrationResult = kIOReturnNotReady;
    IOReturn bsdAttachResult = kIOReturnNotReady;
    nativePMLock = IOLockAlloc();
    if (!nativePMLock) return false;
    // Start a separate diagnostic loop before any blocking startup call.
#if SEQUOIA_DIAGNOSTICS
    startNativeStageReporter(); // Best effort: diagnostics must not fail start.
#endif
    fHalService = wrapper->fHalService;
    wrapper->fHalService = nullptr; // Transfer the allocation reference exactly once.
    _fWorkloop->retain();
    nativeLoopRetained = true;
    UInt8 builtIn = 0;
    setProperty("built-in", &builtIn, sizeof(builtIn));
    setProperty("DriverKitDriver", kOSBooleanFalse);
    ITLWM_NATIVE_STAGE(30, "03_before_super_start");
    ITLWM_NATIVE_STAGE(31, "03a_before_cc_logs");
    if (!initCCLogs()) goto fail;
    nativeFaultAdapter = itlwmCreateFaultAdapter(driverFaultReporter, _fWorkloop, &nativeRawFaultReporter);
    if (provider) {
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeFaultStreamValid",
                              driverFaultReporter && driverFaultReporter->metaCast("CCDataStream"));
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeFaultAdapterValid", nativeFaultAdapter != nullptr);
    }
    if (!nativeFaultAdapter) goto fail;
    ITLWM_NATIVE_STAGE(32, "03b_after_cc_logs");
    systemStarted = super::start(provider);
    ITLWM_NATIVE_STAGE(40, "04_after_super_start");
    nativeSuperStarted = systemStarted ||
        SequoiaInterfaceLifecycle::controllerBaseStarted(this, provider);
    if (!systemStarted) goto fail;
    // findAndAttachToFaultReporter retains our adapter itself. Verify rather
    // than overwrite its owned reference (the r121 write was redundant).
    if (!itlwmFaultAdapterInstalled(this, nativeFaultAdapter)) {
        if (provider) ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePeerFailure", "controller-fault-adapter-mismatch");
        goto fail;
    }
    nativeFaultReporterAttached = itlwmAttachFaultReporter(nativeRawFaultReporter, this);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeFaultReporterAttached", nativeFaultReporterAttached);
    if (!nativeFaultReporterAttached) goto fail;
    ITLWM_NATIVE_STAGE(41, "04a_fault_reporter_attached");
    ITLWM_NATIVE_STAGE(50, "05_before_pci_power");
    ITLWM_NATIVE_STAGE(51, "05a_before_pci_power_call");
    if (!initPCIPowerManagment(pciNub)) goto fail;
    ITLWM_NATIVE_STAGE(52, "05b_after_pci_power");
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    ITLWM_NATIVE_STAGE(53, "05c_before_gate_add");
    if (!_fCommandGate || _fWorkloop->addEventSource(_fCommandGate) != kIOReturnSuccess) goto fail;
    ITLWM_NATIVE_STAGE(54, "05d_after_gate_add");
    nativeGateAdded = true;
    ITLWM_NATIVE_STAGE(60, "06_before_medium_tables");
    if (!createMediumTables(&primary) || !setCurrentMedium(primary) || !setSelectedMedium(primary)) goto fail;
    ITLWM_NATIVE_STAGE(70, "07_before_hal_init");
    ITLWM_NATIVE_STAGE(71, "07a_before_hal_init_call");
    if (!fHalService->initWithController(this, _fWorkloop, _fCommandGate)) goto fail;
    ITLWM_NATIVE_STAGE(72, "07b_after_hal_init");
    if (PE_parse_boot_argn("-novht", &bootValue, sizeof(bootValue)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &bootValue, sizeof(bootValue)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;
    ITLWM_NATIVE_STAGE(80, "08_before_hal_attach");
    ITLWM_NATIVE_STAGE(81, "08a_before_hal_attach_call");
    if (!fHalService->attach(pciNub)) goto fail; // HAL unwinds its own failed attach.
    nativeHalAttached = true;
    ITLWM_NATIVE_STAGE(90, "09_after_hal_attach");
    ITLWM_NATIVE_STAGE(95, "09a_scan_sink_enter");
    if (!initNativeScan()) goto fail;
    ITLWM_NATIVE_STAGE(96, "09b_scan_sink_done");
    if (!initNativeJoin()) goto fail;
    ITLWM_NATIVE_STAGE(97, "09c_join_sink_done");
    if (!initNativePacket()) goto fail;
    ITLWM_NATIVE_STAGE(98, "09d_packet_sink_done");
    ITLWM_NATIVE_STAGE(100, "10_after_native_sinks");
    fHalService->get80211Controller()->ic_event_handler = eventHandler;
    fHalService->get80211Controller()->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (!fWatchdogWorkLoop) goto fail;
    watchdogTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer || fWatchdogWorkLoop->addEventSource(watchdogTimer) != kIOReturnSuccess) goto fail;
    nativeWatchdogAdded = true;
    ITLWM_NATIVE_STAGE(105, "10a_watchdog_ready");
    ITLWM_NATIVE_STAGE(110, "11_before_interface_alloc");
    fNetIf = new AirportItlwmSkywalkInterface;
    if (!fNetIf) goto fail;
    IEEE80211_ADDR_COPY(address.octet, fHalService->get80211Controller()->ic_myaddr);
    if (!fNetIf->init(this, &address)) goto fail;
    // Primary infrastructure path. The family invokes createPeerManager only
    // after its controller/workqueue/logger are ready; role 2 instead borrows
    // a parent's manager and is not an initialization workaround.
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);
    ITLWM_NATIVE_STAGE(111, "11a_interface_inited");
    // The real BSD name/unit are assigned by IONetworkStack later. Neither a
    // fabricated en0 property nor early BSD publication is a peer prerequisite.
    if (!fNetIf->setProperty("built-in", &interfaceBuiltIn, sizeof(interfaceBuiltIn))) goto fail;
    // CoreWiFi 24G830's private findWiFiController path walks from each
    // IOSkywalkEthernetInterface through IOService parents.  Its audited
    // controller matcher accepts the chain only when it finds this exact
    // class identity on the IO80211 controller object.
    if (!setProperty("IOClassNameOverride", "IO80211Controller")) goto fail;
    ITLWM_NATIVE_STAGE(112, "11b_interface_properties_ready");
    ITLWM_NATIVE_STAGE(120, "12_before_interface_attach");
    if (!fNetIf->attach(this)) goto fail;
    nativeServiceAttached = true;
    ITLWM_NATIVE_STAGE(130, "13_before_attach_interface");
    if (!attachInterface(fNetIf, this)) goto fail;
    nativeControllerAttached = true;
    ITLWM_NATIVE_STAGE(140, "14_before_bsd_allocate");
    // r228 B: BSD ownership A/B. Apple reaches BSD through IOSkywalkFamily's own
    // IOSkywalkNetworkBSDClient, which IOKit matches onto any published
    // IOSkywalkNetworkInterface: that client attaches the ifnet itself and is the
    // only path that runs the kernel netif nexus callbacks
    // (nxp_qset_init/nxp_queue_init) initializing this driver's packet queues.
    // itlwm_bsd_owner=skywalk skips the borrowed legacy child below and leaves BSD
    // to that client; the default keeps the legacy child, which is the state every
    // earlier boot ran in. The boot arg is read in its own block because this
    // function reaches its failure label with goto.
    {
        char bsdOwner[16] = {};
        nativeSkywalkBSDOwner =
            PE_parse_boot_arg_str("itlwm_bsd_owner", bsdOwner, sizeof(bsdOwner)) &&
            strcmp(bsdOwner, "skywalk") == 0;
    }
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDOwner", nativeSkywalkBSDOwner ? "skywalk" : "legacy");
    // Leave IOInterfaceUnit absent until Apple's naming bridge assigns it.
    // 24H23 IOSkywalkLegacyEthernetInterface::attachToDataLinkLayer forwards
    // the IONetworkStack-assigned unit to this interface and registerService
    // rematches the BSD client. The bridge does not allocate another ifnet.
    // r229's hardcoded unit 0 bypassed that allocator and collided with
    // IntelMausi on this host. Its historical client failure also predated
    // r238's PACKET/RAW pool correction; an unnamed bridge is not a failure.
    // Establish the real controller/ethernet parent relationship without
    // publishing the child.  The third argument is the controller's
    // `publish` flag (confirmed by the 24G830 attachInterface disassembly).
    // A private allocation is sufficient here; do not call
    // deferBSDAttach(true/false).  The 24G830 implementation of
    // deferBSDAttach(false) performs an implicit registerService(0), which
    // would expose the parent before the child and make the later audited
    // registerService(8) duplicate and timing-dependent.
    if (!nativeSkywalkBSDOwner &&
        !IONetworkController::attachInterface(reinterpret_cast<IONetworkInterface **>(&bsdInterface), false))
        goto fail;
    ITLWM_NATIVE_STAGE(141, nativeSkywalkBSDOwner ? "14a_bsd_owner_skywalk"
                                                  : "14a_bsd_allocated_private");
    // Apple calls IO80211InfraInterface::registerInfraEthernetInterface from
    // inside its Skywalk start path, after Infra::start has created the glue,
    // scan manager and peer/controller state. Keep that ordering here too;
    // calling the helper before start is not equivalent to Apple's boundary.
    ITLWM_NATIVE_STAGE(150, "15_before_skywalk_start");
    if (!fNetIf->start(this)) goto fail;
    nativeInterfaceStarted = true;
    ITLWM_NATIVE_STAGE(160, "16_after_skywalk_start");
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationResult", static_cast<UInt64>(kIOReturnNotReady), 32);
    registrationResult = nativeRegistration.prepare(fNetIf, _fWorkloop, this, address.octet,
        [](OSObject *owner, IOSkywalkTxSubmissionQueue *queue, IOSkywalkPacket *const *packets, UInt count, void *) -> UInt {
            return static_cast<AirportItlwm *>(owner)->nativeTransmitPackets(queue, packets, count);
        }, nullptr, registerNativeInfraEthernet);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRegistrationResult", static_cast<UInt64>(registrationResult), 32);
    publishNativeRegistrationDiagnostics("ethernet-registered");
    if (registrationResult != kIOReturnSuccess) goto fail;
    ITLWM_NATIVE_STAGE(165, "16a_registration_prepared");
    // registerEthernetInterface() has now populated both Skywalk registration
    // records. Complete the BSD child before making
    // the parent visible.  The Apple implementation has already published
    // and attached its child by the time the `start` tail calls
    // registerService(8); publishing a parent with no ifnet first leaves
    // airportd with a name-only shell and it does not reliably rematch the
    // child later.  r188e reversed that order and still produced the exact
    // failure this ordering is intended to avoid.
    ITLWM_NATIVE_STAGE(166, "16b_bsd_attach_ready");
    IOLockLock(nativePMLock);
    nativeReady = true;
    IOLockUnlock(nativePMLock);
    // The initial controller attach happened privately before registration.
    // Calling the real child callback here is the equivalent of the family's
    // post-registration match, and publishes the BSD service only after the
    // ifnet and IO80211 subtype have been established.
    if (nativeSkywalkBSDOwner) {
        // Our custom legacy child is absent. Apple's naming bridge may attach
        // after publication, but only its NetworkBSDClient allocates the ifnet.
        bsdAttachResult = kIOReturnSuccess;
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDStage", "bsd-owner-skywalk");
    } else {
        bsdAttachResult = tryNativeBSDChildAttach();
        if (bsdAttachResult != kIOReturnSuccess || !bsdInterface || !bsdInterface->getIfnet()) {
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDStage", "bsd-attach-pending");
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDResult",
                                  static_cast<UInt64>(bsdAttachResult), 32);
            // Do not publish a parent that has no real ifnet.  On 24G830 the
            // first airportd match is sticky; exposing an incomplete shell here
            // recreates the name-only enX state that this ordering fix is meant
            // to eliminate.  Cleanup keeps the failure retryable on the next
            // boot instead of leaving a partially discoverable interface.
            goto fail;
        }
    }
    // AppleBCMWLANSkywalkInterface::start publishes its parent service only
    // after registerInfraEthernetInterface returns and its child is ready.
    // Keep this publication after BSD attach so CoreWLAN's first match sees a
    // complete Wi-Fi interface rather than an empty enX shell.
    // r230: IOKit matches and starts the BSD client on another thread after
    // this publication, and discards it once its start fails, so a synchronous
    // read here only ever sees the pre-start object (r229's A-probe). Sample
    // from publication onward instead. Best effort: a diagnostic loop must
    // never fail the boot.
#if SEQUOIA_DIAGNOSTICS
    if (nativeSkywalkBSDOwner)
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBsdProbeArmed", startNativeBsdProbe());
#endif
    fNetIf->registerService(8);
    nativeInterfaceServicePublished = true;
    // r231: the timer probe cannot win this race - r230's first sample at 5 ms
    // already found the client gone, and r229's synchronous census (taken
    // microseconds after this point) only ever saw the attached pre-start
    // object. Sample the entire start lifetime from this thread instead: a
    // microsecond-granularity burst that publishes each state change of the
    // client, its IOSkywalkBSDClient::start progress fields, and the
    // interface's name-write state. Diagnostic only: bounded, allocation-free,
    // and it never fails the boot.
    if (nativeSkywalkBSDOwner) {
        void itlwmBsdBurstReset();
        bool itlwmBsdBurstSample(IOService *target, void *interface, unsigned iteration);
        void itlwmBsdBurstFinalize(IOService *target);
        itlwmBsdBurstReset();
        constexpr unsigned kBurstSamples = 3000;
        for (unsigned burst = 0; burst < kBurstSamples; ++burst) {
            itlwmBsdBurstSample(provider, fNetIf, burst);
            IODelay(5);
        }
        itlwmBsdBurstFinalize(provider);
    }
    // AppleBCMWLANSkywalkInterface::start publishes its own Skywalk service
    // with option 8 immediately after registerInfraEthernetInterface returns,
    // then clears the running state.  Publishing only the BSD child leaves an
    // enX node visible while CoreWLAN still has no AirPort entity.  Mirror the
    // audited ordering and retain the result for the post-boot checker.
    // AppleBCMWLANSkywalkInterface::start publishes its own Skywalk service with
    // option 8 immediately after registerInfraEthernetInterface returns. Apple
    // calls IOService::registerService last, but this driver's path is not the
    // same class, and publishing the Skywalk interface here proved to stop the
    // Ethernet interface from ever being attached: AirportItlwmEthernetInterface
    // performs the whole BSD attach, including its own registerService, from
    // attachToDataLinkLayer, which r183b and r184 never reached. Both boots ended
    // with BSDStage awaiting-bsd-attach, BSDNameLength 0, both BSD interface
    // pointers zero, ScanRequests 0 and ScanBeaconsDecoded 0, and airportd
    // reporting "Did NOT find matching WiFi interface" with getifaddrs count 0 -
    // Wi-Fi shows as off because there is no enX for it at all.
    //
    // Make it opt-in so the driver returns to the ordering that did publish BSD,
    // while keeping the audited call available for a direct comparison without a
    // rebuild. The boot arg is read in its own block because this function reaches
    // its failure label with goto.
    {
        // The system's AppleBCMWLANSkywalkInterface publishes unconditionally
        // after registerInfraEthernetInterface; this candidate mirrors that
        // audited boundary. The historical boot argument is intentionally no
        // longer consulted so a trial cannot silently omit the service.
    }
    // The audited Apple call is a virtual through the interface's vtable at slot
    // 0x930 with a zeroed argument, read here as setRunningState(false). Two
    // things make that reading unsafe to apply unconditionally. The slot is not
    // this port's layout, which has already mis-mapped createPeerManager and
    // createVirtualInterface on this family, and this driver maintains its own
    // running state from the link path in native_link_sequoia.inc, so a startup
    // request to stop running competes with the first link transition.
    //
    // On the r183b boot the startup call coincided with every scan request being
    // answered upstream. airportd produced 473 SCAN_MANAGER_EVENT_SCAN_REQ and 44
    // SCAN_COMPLETE while this driver recorded ScanRequests 0, ScanAcceptedRequest
    // 0, ScanTerminals 0, ScanNotifications 0 and ScanBeaconsDecoded 0 - against
    // tens of thousands decoded on the candidate before it - and no BSD interface
    // attached at all (BSDStage awaiting-bsd-attach, BSDNameLength 0, both BSD
    // interface pointers zero). Make it opt-in and off by default, so the
    // interface comes up in a state that can serve the next scan, and publish
    // whether it ran so the two readings stay distinguishable after boot.
    // The boot arg is read in its own block: this function reaches its failure
    // label with goto, and a declaration at function scope between here and the
    // label would be jumped over.
    {
        int clearRunningState = 0;
        PE_parse_boot_argn("itlwm_clear_running_state", &clearRunningState,
                           sizeof(clearRunningState));
        nativeRunningStateApplied = clearRunningState != 0;
        if (nativeRunningStateApplied)
            nativeRunningStateResult = fNetIf->setRunningState(false);
    }
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeInterfaceServicePublished", nativeInterfaceServicePublished);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRunningStateApplied", nativeRunningStateApplied);
    ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeRunningStateResult",
                          static_cast<UInt64>(nativeRunningStateResult), 32);
    this->publishNativeScanDiagnostics();
    // Active, not merely valid. ControlCenter reports this interface as
    // "Wi-Fi status is active (inactive, UI state: 10) with no connected network" and
    // oscillates between UI state 90 (animating) and 10 (inactive) - it never reaches
    // the state in which its Wi-Fi module lists nearby networks - while WiFiAgent, the
    // menu's own agent, already holds the scan results (its log walks a list of 220
    // networks waiting for password sync). Every setLinkStatus call in this driver
    // passed kIONetworkLinkValid alone, so the interface was published as up but never
    // active.
    setLinkStatus(nativeWclCompatR162
                      ? kIONetworkLinkValid
                      : (kIONetworkLinkValid | kIONetworkLinkActive));
    ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativeStage", "interface-prepared-join-pending");
    // Every driver event, including the announcement just below, reaches the WCL only
    // when the interface's glue ivar (+0xd8) is set: IO80211SkywalkInterface::
    // postMessageInternal loads it and jumps to IO80211Glue::routeEventToWcl, and
    // falls back to postMessageSync when it is null. Four candidates changed the
    // scan-result message id and then the call site without ever reading this field,
    // so read it now, and build the glue with the family's own factory if the family
    // left it empty.
    {
        bool itlwmEnsureInterfaceGlue(void *interface, void **installed);
        void *interfaceGlue = nullptr;
        itlwmEnsureInterfaceGlue(fNetIf, &interfaceGlue);
        // r223: install the direct WCL board route for scan deliveries. The
        // pending-event queue's last hop (IO80211Glue::receiveMessageInternal)
        // hands WCLGlue a stale pre-allocated message instead of the posted one,
        // so every WCL scan message posted through the interface is dropped.
        // Apple's ScanAdapter calls WCLBulletinBoard::sendMessage directly;
        // itlwmPostWclScanResult (SequoiaPeerFactory.cpp) does the same. The
        // route pointer lives in native_scan_sequoia.inc, which this translation
        // unit includes AFTER this file, so the install must go through that
        // file's setter: r223/d149 declared a same-named pointer here instead,
        // which created a second function-scope object, left the delivery's
        // pointer null and made the whole direct route dead code (a full boot
        // reported ItlwmNativeWclDirectPosts=0 with 124 queue posts).
        // itlwm_scan_post=queue keeps the old interface route for an A/B boot
        // without a rebuild; anything else (including no argument) installs it.
        bool itlwmPostWclScanResult(void *interface, uint32_t msgId, void *bytes,
                                    size_t size, uint32_t *result);
        void itlwmSetWclDirectPostRoute(SequoiaWCL::WclDirectPostRoute);
        char postRoute[8] = {};
        const bool directPosts = !PE_parse_boot_argn("itlwm_scan_post", postRoute, sizeof(postRoute)) ||
                                 strcmp(postRoute, "queue") != 0;
        if (directPosts) itlwmSetWclDirectPostRoute(itlwmPostWclScanResult);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclDirectRouteInstalled", directPosts);
        ITLWM_LOG("itlwm-native: interface glue ivar+0xd8 = %p\n", interfaceGlue);
        // Publish it too: the driver's IOLog output does not reach the unified log,
        // so a property is what makes this readable after a boot.
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeInterfaceGlue",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(interfaceGlue)), 64);
        // The controller half of the same question: the two loads the family's
        // postMessage performs before its tail jump into routeEventToWcl.
        bool itlwmControllerDeliveryChain(void *controller, void **first, void **second);
        void *chainGate = nullptr;
        void *chainDelivery = nullptr;
        itlwmControllerDeliveryChain(this, &chainGate, &chainDelivery);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeControllerChain",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(chainGate)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeDeliveryObject",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(chainDelivery)), 64);
        ITLWM_LOG("itlwm-native: delivery chain gate=%p delivery=%p\n", chainGate, chainDelivery);
        // The event queue's own state: routeEventToWcl sets gate+0x30 when it
        // queues an event and the worker clears it when it drains. A byte stuck at
        // one means events are being queued and never consumed.
        bool itlwmGlueWakeState(void *interface, void **gate, unsigned *wake);
        void *eventGate = nullptr;
        unsigned wakeByte = 0;
        itlwmGlueWakeState(fNetIf, &eventGate, &wakeByte);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueGate",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eventGate)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueWakeByte", wakeByte, 32);
        // receiveMessageInternal's own three loads: glue ivars+0x8 -> +0x120 ->
        // +0xb08. The last value is the WCL delivery object each queued event is
        // handed to; a zero there means every event is dropped inside the glue,
        // after the queue but before any scan callback.
        bool itlwmGlueDeliveryChain(void *interface, void **objectA, void **objectB, void **delivery);
        void *glueObjectA = nullptr;
        void *glueObjectB = nullptr;
        void *glueDelivery = nullptr;
        const bool glueChainOk = itlwmGlueDeliveryChain(fNetIf, &glueObjectA, &glueObjectB, &glueDelivery);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueDeliveryOk", glueChainOk ? 1 : 0, 32);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueObjectA",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(glueObjectA)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueObjectB",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(glueObjectB)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueDelivery",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(glueDelivery)), 64);
        ITLWM_LOG("itlwm-native: glue delivery chain A=%p B=%p delivery=%p\n",
              glueObjectA, glueObjectB, glueDelivery);
        // The WCL message-enable bitmap each queued event passes through in
        // WCLBulletinBoard::sendMessage. msgType 2 (scan results) lives at +0x8d0;
        // the scan-result ids 0xc9/0xcd/0xce/0xed are bits in bytes 25..29. A zero
        // there means the results are dropped before scanupdateHandler runs.
        bool itlwmGlueMsgBitmap(void *interface, void **board, uint8_t *bytes, size_t len);
        void *bulletinBoard = nullptr;
        uint8_t bitmapBytes[32] = {};
        const bool bitmapOk = itlwmGlueMsgBitmap(fNetIf, &bulletinBoard, bitmapBytes, sizeof(bitmapBytes));
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBulletinBoard",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bulletinBoard)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeMsgBitmapOk", bitmapOk ? 1 : 0, 32);
        // Publish the four scan-result bytes plus a packed view of bytes 25..29 so a
        // single property shows whether the scan messages are enabled.
        uint64_t scanBits = 0;
        for (unsigned i = 0; i < 5 && i + 25 < sizeof(bitmapBytes); ++i)
            scanBits |= static_cast<uint64_t>(bitmapBytes[25 + i]) << (8 * i);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanMsgBits", scanBits, 64);
        ITLWM_LOG("itlwm-native: bulletin=%p msgBitmap ok=%d scanBits=%llx\n",
              bulletinBoard, bitmapOk ? 1 : 0, scanBits);
        // Read the base-class init pointer WCLBulletinBoard::initWithOptions calls
        // through the global vtable slot: it returns false and is why the board stays
        // null even after the post-registration rebuild.
        void itlwmReadWclBulletinBoardBaseInit(uint64_t *table, uint64_t *slot98);
        uint64_t baseTable = 0, baseSlot98 = 0;
        itlwmReadWclBulletinBoardBaseInit(&baseTable, &baseSlot98);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBaseInitTable", baseTable, 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBaseInitSlot98", baseSlot98, 64);
        ITLWM_LOG("itlwm-native: base-init table=%llx slot98=%llx\n",
              static_cast<unsigned long long>(baseTable),
              static_cast<unsigned long long>(baseSlot98));
        // Read the correct chain: WCLGlue is at glue ivars+0x18, and its
        // WCLBulletinBoard at WCLGlue ivars+0x8. The +0x8/+0x120/+0xb08 chain above
        // is the event-delivery path, not the board, so BulletinBoard=0 above may be
        // a false negative.
        bool itlwmGlueBulletinBoardCorrect(void *interface, void **board, uint8_t *bytes, size_t len);
        void *boardCorrect = nullptr;
        uint8_t correctBytes[32] = {};
        const bool correctOk = itlwmGlueBulletinBoardCorrect(fNetIf, &boardCorrect, correctBytes, sizeof(correctBytes));
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBulletinBoardCorrect",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(boardCorrect)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeMsgBitmapOkCorrect", correctOk ? 1 : 0, 32);
        uint64_t correctScanBits = 0;
        for (unsigned i = 0; i < 5 && i + 25 < sizeof(correctBytes); ++i)
            correctScanBits |= static_cast<uint64_t>(correctBytes[25 + i]) << (8 * i);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanMsgBitsCorrect", correctScanBits, 64);
        ITLWM_LOG("itlwm-native: board-correct=%p bitmap-ok=%d scanBits=%llx\n",
              boardCorrect, correctOk ? 1 : 0,
              static_cast<unsigned long long>(correctScanBits));
        // Read the WCL controller chain to see where the FSM's message-enable loop
        // stopped, then enable the scan-result messages directly if they are still off.
        bool itlwmReadWclControllerChain(void *interface, void **controller, void **scanManager);
        void *wclController = nullptr, *wclScanManager = nullptr;
        const bool chainOk = itlwmReadWclControllerChain(fNetIf, &wclController, &wclScanManager);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclController",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(wclController)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclScanManager",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(wclScanManager)), 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeWclChainOk", chainOk ? 1 : 0, 32);
        bool itlwmEnableScanMessages(void *interface);
        const bool enabled = itlwmEnableScanMessages(fNetIf);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanMessagesEnabled", enabled ? 1 : 0, 32);
        ITLWM_LOG("itlwm-native: wcl controller=%p scanManager=%p chain=%d enable=%d\n",
              wclController, wclScanManager, chainOk ? 1 : 0, enabled ? 1 : 0);
        // Read the real scan-message bitmap words (2-byte stride) and the block flag.
        void itlwmReadScanBitmapCorrect(void *interface, uint16_t *out, unsigned *blockFlag);
        uint16_t scanBmp[4] = {};
        unsigned blockFlag = 0;
        itlwmReadScanBitmapCorrect(fNetIf, scanBmp, &blockFlag);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBlockFlag", blockFlag, 32);
        uint64_t scanBmpPacked = 0;
        for (int i = 0; i < 4; ++i)
            scanBmpPacked |= static_cast<uint64_t>(scanBmp[i]) << (16 * i);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanBmpCorrect", scanBmpPacked, 64);
        ITLWM_LOG("itlwm-native: block=%u scanBmp=%04x %04x %04x %04x\n",
              blockFlag, scanBmp[0], scanBmp[1], scanBmp[2], scanBmp[3]);
        // r222: dump the scan manager's msgIndex->handler dispatch table. The
        // callMsgCallBack path resolves 0xc90002 (0xc9) / 0xce0002 (0xce) through
        // this table; if either is absent the scan-result message is dropped with
        // E00002BC, which is exactly the "FSM events 0" symptom.
        bool itlwmDumpScanManagerHandlers(void *interface, uint64_t *count,
                                          uint32_t *indexes, void **handlers, size_t maxEntries);
        uint64_t handlerCount = 0;
        uint32_t handlerIndexes[32] = {};
        void *handlerFns[32] = {};
        const bool dumped = itlwmDumpScanManagerHandlers(fNetIf, &handlerCount,
                                                         handlerIndexes, handlerFns, 32);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeHandlerCount", handlerCount, 64);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeHandlerDumped", dumped ? 1 : 0, 32);
#if SEQUOIA_DIAGNOSTICS
        for (unsigned i = 0; i < 32 && i < handlerCount; ++i) {
            char hkey[64];
            snprintf(hkey, sizeof(hkey), "ItlwmNativeHandlerIdx%u", i);
            provider->setProperty(hkey, handlerIndexes[i], 32);
            snprintf(hkey, sizeof(hkey), "ItlwmNativeHandlerFn%u", i);
            provider->setProperty(hkey, reinterpret_cast<uint64_t>(handlerFns[i]), 64);
        }
#endif
        ITLWM_LOG("itlwm-native: handler-table count=%llu dumped=%d\n",
              static_cast<unsigned long long>(handlerCount), dumped ? 1 : 0);
        for (unsigned i = 0; i < 32 && i < handlerCount; ++i)
            ITLWM_LOG("itlwm-native: handler[%u] msgIndex=%08x fn=%p\n",
                  i, handlerIndexes[i], handlerFns[i]);
        // r228 A: the packet queues themselves. Every native RX enqueue has
        // returned 0xE00002D7, which means +0x28 or +0xcc is clear - i.e. the
        // kernel's netif nexus never called IOSkywalkPacketQueue::initialize for
        // this queue set. Read both queues at interface start so one boot shows
        // whether the Skywalk BSD client attached them, separately from whether
        // the enqueue call site is right. +0x78 additionally decides whether an
        // accepted packet reaches the BSD stack instead of the KPipe path.
#if SEQUOIA_DIAGNOSTICS
        bool itlwmPublishQueueState(IOService *target, const char *prefix, void *queue);
        const bool txQueueReady =
            itlwmPublishQueueState(provider, "ItlwmNativeTxQueue", nativeRegistration.txQueue());
        const bool rxQueueReady =
            itlwmPublishQueueState(provider, "ItlwmNativeRxQueue", nativeRegistration.rxQueue());
#endif
        ITLWM_LOG("itlwm-native: txQueue=%p ready=%d rxQueue=%p ready=%d\n",
              nativeRegistration.txQueue(), txQueueReady ? 1 : 0,
              nativeRegistration.rxQueue(), rxQueueReady ? 1 : 0);
        // r228 A: the other half of the same question. IOSkywalkFamily's own IOKit
        // personality instantiates IOSkywalkNetworkBSDClient on any published
        // IOSkywalkNetworkInterface, and only that client's start creates the
        // nexus whose callbacks initialize the queues above. Publish what IOKit
        // attached to the interface and the client's own start state, so a boot
        // separates "no client was ever matched" from "the client ran and failed".
        bool itlwmPublishInterfaceChildren(IOService *target, void *interface, unsigned *count);
        unsigned childCount = 0;
        const bool bsdClient = itlwmPublishInterfaceChildren(provider, fNetIf, &childCount);
        ITLWM_LOG("itlwm-native: interface children=%u bsdClient=%d\n",
              childCount, bsdClient ? 1 : 0);
    }
    // Announce the driver to the WCL, the one event this driver has never sent.
    // Every scan-result query the family serves prints isDriverAvailable=<0> in its
    // IO80211Family IOC DEBUG lines, and WCLSystemStateManager::
    // driverAvailableEventHandler - the handler for APPLE80211_M_DRIVER_AVAILABLE,
    // id 55/0x37 - has never run on any boot. That is why four consecutive
    // candidates that changed the scan-result message id (0xce, 0xc9) and then its
    // route (controller postMessage, interface postMessage) all behaved identically:
    // the family is not withholding the results, it never moved out of the
    // driver-unavailable state, so the scan cache answers every query with
    // GOOD:0:0x0 and the scan FSM never receives a result event.
    //
    // The handler takes the message payload as apple80211_driver_available_data and
    // reads its own offsets - +0x08 (non-zero takes the available transition),
    // +0x10 (reason, rejected only when it equals 0xE0822803) and +0x14 - and it
    // requires the whole message to be 0xf8 bytes, the 0xB8-byte structure plus the
    // 0x40-byte prefix these WCL messages carry. The struct is left in the header's
    // own layout so the family's offsets line up without any packing.
    this->publishNativeJoinDiagnostics();
    // The child callback above checks admission, and stop closes admission
    // before synchronous BSD detach joins callbacks. The child has already
    // published itself from attachToDataLinkLayer; the parent notification
    // above is intentionally the final discovery boundary.
    if (!bsdInterface || !bsdInterface->getIfnet())
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBSDStage",
                              nativeSkywalkBSDOwner ? "bsd-owner-skywalk" : "bsd-attach-pending");
    captureNativeJoinDiagnostics();
    ITLWM_NATIVE_STAGE(165, "16a_bsd_registered");
    registerService();
    ITLWM_NATIVE_STAGE(170, "17_complete");
    // Rebuild the glue after the controller service is published. r214 proved the
    // pre-registration glue build left its WCLBulletinBoard null (WCLGlue ivars+0x8
    // == 0) because the base-class init failed, so every scan-result message was
    // dropped in WCLGlue::receiveMessageInternal before it ever reached
    // scanupdateHandler. This second, post-registration attempt rebuilds only when
    // the board is still missing, and re-reads it so one boot shows the outcome.
    {
        bool itlwmRebuildInterfaceGlue(void *interface, void **installed);
        void *rebuiltGlue = nullptr;
        itlwmRebuildInterfaceGlue(fNetIf, &rebuiltGlue);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeGlueRebuilt", rebuiltGlue ? 1 : 0, 32);
        bool itlwmGlueMsgBitmap(void *interface, void **board, uint8_t *bytes, size_t len);
        void *board2 = nullptr;
        uint8_t bytes2[32] = {};
        itlwmGlueMsgBitmap(fNetIf, &board2, bytes2, sizeof(bytes2));
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeBulletinBoardPostReg",
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(board2)), 64);
        uint64_t scanBits2 = 0;
        for (unsigned i = 0; i < 5 && i + 25 < sizeof(bytes2); ++i)
            scanBits2 |= static_cast<uint64_t>(bytes2[25 + i]) << (8 * i);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeScanMsgBitsPostReg", scanBits2, 64);
        ITLWM_LOG("itlwm-native: post-register rebuild glue=%p bulletin=%p scanBits=%llx\n",
              rebuiltGlue, board2, scanBits2);
    }
    // configd/WCL can only attach their listeners after the BSD and controller
    // services are visible. Repeat the same idempotent availability transition
    // across that registration boundary; the provider counters make this
    // ordering observable after the boot.
    if (!nativeWclCompatR162)
        announceNativeDriverAvailable(1);
    itlwmPublishBootProgress(this);
    ITLWM_LOG("itlwm-native: startup complete events=%u hal=%s\n",
          gItlwmHalProgress.count(), gItlwmHalProgress.current());
    // Keep the reporter alive on purpose: association and the first scan happen
    // after this point, and a hard hang there is otherwise unrecorded. It is
    // stopped by stopNative(), which drains it before the owner goes away.
    return true;
fail:
    ITLWM_NATIVE_STAGE(900, "90_fail");
    itlwmPublishBootProgress(this);
    ITLWM_LOG("itlwm-native: startup failed events=%u hal=%s\n",
          gItlwmHalProgress.count(), gItlwmHalProgress.current());
    stopNative(provider);
    return false;
}

void AirportItlwm::stopNative(IOService *provider)
{
    if (nativePMLock) IOLockLock(nativePMLock);
    if (nativeStopping) {
        if (nativePMLock) IOLockUnlock(nativePMLock);
        return;
    }
    nativeStopping = true;
    nativeReady = false;
    if (nativePMLock) IOLockUnlock(nativePMLock);
    // Drain queries and sleeping MAC/enable/disable operations before touching
    // their HAL, queues or command gate. Acquiring the gate alone cannot drain
    // a firmware commandSleep, which temporarily releases it.
    if (nativeLoopRetained)
        drainNativeRadioTransition();
    quiesceNativeSystemEvents();
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->quiesceNativeTimers();
    stopNativePacket();
    stopNativeScan();
    stopNativeJoin();
    stopNativeBsdProbe();
    unregistPM(); // Drain retained PM workers while command gate/HAL still exist.
    if (nativeWatchdogAdded) {
        fWatchdogWorkLoop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<AirportItlwm *>(owner);
            self->watchdogTimer->cancelTimeout();
            self->watchdogTimer->disable();
            return kIOReturnSuccess;
        }, this);
        fWatchdogWorkLoop->removeEventSource(watchdogTimer);
        nativeWatchdogAdded = false;
    }
    if (nativeHalAttached) {
        auto *ic = fHalService ? fHalService->get80211Controller() : nullptr;
        if (ic) ic->ic_event_handler = nullptr;
        fHalService->detach(pciNub);
        nativeHalAttached = false;
        nativeJoin.reset();
    }
    if (nativeScanStorage) {
        // Both the scan worker and HAL/RX are now joined. Invalidate the
        // delivery's borrowed pointer before releasing its backing allocation.
        nativeScan.hardwareStopped();
        if (nativeScan.delivery.releaseStorage()) {
            IOFree(nativeScanStorage, sizeof(SequoiaWCL::ScanDelivery::Slot) * SequoiaWCL::ScanDelivery::MaxSlots);
            nativeScanStorage = nullptr;
        } else {
            ITLWM_LOG("itlwm: native scan storage still owned at detach; preserving allocation\n");
        }
    }
    if (bsdInterface)
        IONetworkController::detachInterface(bsdInterface, true);
    if (fHalService) {
        // configureInterface lends these pointers before system attach can
        // fail and release the interface without assigning bsdInterface.
        auto *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
        ether_ifdetach(ifp);
        ifp->netStat = nullptr;
    }
    fpNetStats = nullptr;
    OSSafeReleaseNULL(bsdInterface);
    if (nativeInterfaceStarted) {
        fNetIf->stop(this);
        nativeInterfaceStarted = false;
    }
    nativeInterfaceServicePublished = false;
    nativeRunningStateResult = kIOReturnNotReady;
    nativeRunningStateApplied = false;
    nativeRegistration.reset();
    if (nativeControllerAttached) {
        detachInterface(fNetIf, true);
        nativeControllerAttached = false;
    }
    if (nativeServiceAttached) {
        fNetIf->detach(this);
        nativeServiceAttached = false;
    }
    OSSafeReleaseNULL(fNetIf);
    if (nativeSuperStarted) {
        super::stop(provider);
        nativeSuperStarted = false;
    }
    itlwmReleaseFaultAdapter(&nativeFaultAdapter, &nativeRawFaultReporter,
                            nativeFaultReporterAttached ? this : nullptr);
    nativeFaultReporterAttached = false;
    releaseNativeSystemEvents();
    OSSafeReleaseNULL(fHalService);
    OSSafeReleaseNULL(watchdogTimer);
    OSSafeReleaseNULL(fWatchdogWorkLoop);
    if (nativeGateAdded) {
        _fWorkloop->removeEventSource(_fCommandGate);
        nativeGateAdded = false;
    }
    OSSafeReleaseNULL(_fCommandGate);
    if (nativeLoopRetained) {
        _fWorkloop->release(); // Provider retains the global compatibility owner.
        nativeLoopRetained = false;
    }
    // Last: the reporter is diagnostic only, and keeping it alive through the
    // whole teardown still reports the stage reached if teardown itself stalls.
    stopNativeStageReporter();
    // The provider outlives this object but is not ours; stop touching it.
    __atomic_store_n(&gItlwmStageProvider, nullptr, __ATOMIC_RELAXED);
    releaseCCLogs();
}
