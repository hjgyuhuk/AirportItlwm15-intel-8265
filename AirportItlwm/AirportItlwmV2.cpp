//
//  AirportItlwmV2.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmV2.hpp"
#include <HAL/ItlDiagnostics.h>
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>

#include "AirportItlwmSkywalkInterface.hpp"
#include "IOPCIEDeviceWrapper.hpp"
#if __IO80211_TARGET >= __MAC_15_0
#include <Airport/SequoiaSkywalkPacket.h>
#include <Airport/SequoiaPacketIO.h>
#include <Airport/SequoiaInterfaceLifecycle.h>
#include <Airport/SequoiaPeerDiagnostics.h>
#endif

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;

void AirportItlwm::releaseCCLogs()
{
#if __IO80211_TARGET >= __MAC_15_0
    // These factory-owned pipes attach to the controller. Stop detaches them;
    // release alone leaves the registry's ownership cycle intact.
    if (driverLogPipe) driverLogPipe->stopPipe();
    if (driverDataPathPipe) driverDataPathPipe->stopPipe();
    if (driverSnapshotsPipe) driverSnapshotsPipe->stopPipe();
    OSSafeReleaseNULL(driverLogger);
#endif
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
}

void AirportItlwm::releaseAll()
{
#if __IO80211_TARGET >= __MAC_15_0
    // Native resources must be drained with the real provider still alive.
    stopNative(getProvider());
#else
    releaseCCLogs();
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (scanSource) {
            scanSource->cancelTimeout();
            scanSource->disable();
            _fWorkloop->removeEventSource(scanSource);
            scanSource->release();
            scanSource = NULL;
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
#endif
}

void AirportItlwm::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, ic->ic_ac.ac_if.controller);
    if (!that) return;
#if __IO80211_TARGET >= __MAC_15_0
    if (!that->getWorkQueue()->inGate() || !that->nativeAvailable()) return;
    if (msgCode == IEEE80211_EVT_NATIVE_JOIN && data) {
        auto *hal = that->fHalService ? that->fHalService->getNativeHal() : nullptr;
        if (hal && hal->nativeController() == ic)
            hal->nativeJoinEvent(*static_cast<const ItlJoin::Event *>(data));
    }
    // Legacy empty ASSOC_DONE/DEAUTH messages are not native WCL events.
    // The native join owner will post complete, ordered WCL payloads.
    return;
#else
    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
    switch (msgCode) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_DEAUTH:
            interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0);
            break;
        default:
            break;
    }
#endif
}

#if __IO80211_TARGET >= __MAC_15_0
SInt32 AirportItlwm::handleCardSpecific(IO80211SkywalkInterface *interface,
                                        unsigned long request, void *data, bool isSet)
{
    (void)data;
    __atomic_add_fetch(&nativeLegacyScanRequests, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&nativeLegacyScanLastCommand,
                     request > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(request),
                     __ATOMIC_RELAXED);

    // 24G830 performs the real scan through WCL slot 592, then sends the old
    // ioctl 10 as a transaction probe. Returning Unsupported here makes
    // airportd discard the WCL results as a failed outer scan. Accept only the
    // audited legacy scan set probes; do not parse or retain their private
    // payload and never start the compatibility cache worker.
    const bool accepted = nativeAvailable() && interface == fNetIf && isSet &&
        (request == APPLE80211_IOC_SCAN_REQ || request == APPLE80211_IOC_SCAN_REQ_MULTIPLE);
    const uint32_t result = accepted ? kIOReturnSuccess : kIOReturnUnsupported;
    if (accepted) __atomic_add_fetch(&nativeLegacyScanAccepted, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&nativeLegacyScanLastResult, result, __ATOMIC_RELAXED);
    if (getProvider()) {
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacyScanRequests",
                                   __atomic_load_n(&nativeLegacyScanRequests, __ATOMIC_RELAXED), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacyScanAccepted",
                                   __atomic_load_n(&nativeLegacyScanAccepted, __ATOMIC_RELAXED), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacyScanLastCommand",
                                   __atomic_load_n(&nativeLegacyScanLastCommand, __ATOMIC_RELAXED), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacyScanLastResult", result, 32);
    }
    return result;
}

void AirportItlwm::recordNativeLegacyScanSet(bool multiple, IOReturn result)
{
    if (multiple)
        __atomic_add_fetch(&nativeLegacySetScanMultipleRequests, 1u, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&nativeLegacySetScanRequests, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&nativeLegacySetScanLastResult, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
    if (getProvider()) {
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacySetScanRequests",
                                   __atomic_load_n(&nativeLegacySetScanRequests, __ATOMIC_RELAXED), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacySetScanMultipleRequests",
                                   __atomic_load_n(&nativeLegacySetScanMultipleRequests, __ATOMIC_RELAXED), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(getProvider(), "ItlwmNativeLegacySetScanLastResult",
                                   __atomic_load_n(&nativeLegacySetScanLastResult, __ATOMIC_RELAXED), 32);
    }
}
#endif

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    (*ifp->if_watchdog)(ifp);
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
    UInt32 msg = 0;
    AirportItlwm *that = (AirportItlwm *)owner;
    that->fNetIf->postMessage(APPLE80211_M_SCAN_DONE, &msg, 4, 0);
}

bool AirportItlwm::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    // System init can leave a null recursive lock and uninitialized list;
    // its free dereferences both. Reserve cleanup capacity before entering it.
    auto *rollbackLock = IORecursiveLockAlloc();
    if (!rollbackLock) return false;
#endif
    bool ret = super::init(properties);
#if __IO80211_TARGET >= __MAC_15_0
    if (!ret && SequoiaInterfaceLifecycle::prepareFailedControllerInit(this, rollbackLock))
        rollbackLock = nullptr; // The system free path now owns this lock.
    if (rollbackLock) IORecursiveLockFree(rollbackLock);
#endif
    awdlSyncEnable = true;
    power_state = 0;
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    return ret;
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    IOPCIEDeviceWrapper *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    pciNub = wrapper->pciNub;
#if __IO80211_TARGET >= __MAC_15_0
    if (!pciNub || !wrapper->fHalService)
        return nullptr;
#else
    fHalService = wrapper->fHalService;
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
#endif
    return super::probe(provider, score);
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

bool AirportItlwm::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "Itlwm_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
#if __IO80211_TARGET >= __MAC_15_0
    if (!driverLogPipe)
        return false;
    CCStreamOptions loggerOptions = {};
    driverLogger = CCLogStream::withPipeAndName(driverLogPipe, "AirportItlwm", &loggerOptions);
    if (!driverLogger)
        return false;
#endif
    
    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);
    
    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0x200000001;
    driverLogOptions.log_data_type = 2;
    strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pipe_size = 128;
    driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "StateSnapshots", &driverLogOptions);
    XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);
#if __IO80211_TARGET >= __MAC_15_0
    if (!driverSnapshotsPipe)
        return false;
#endif
    
    // The fault reporter is NOT the log stream. IO80211Controller::start only
    // retains it, and IO80211PeerManager reaches the driver's CoreCapture report
    // set through it: a reporter built on the wrong pipe leaves PeerManager
    // dereferencing a null registry key inside OSSymbol::withString, which is
    // the CR2 = 0x38 panic (panic3.txt / panic4.txt). Build it exactly as the
    // pre-r82 lifecycle did, from the StateSnapshots pipe.
    CCStreamOptions reporterOptions = {};
    reporterOptions.stream_type = 1;
    reporterOptions.console_level = static_cast<uint64_t>(-1);
    driverFaultReporter = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &reporterOptions);
    XYLog("%s driverFaultReporterRet %d (FaultReporter stream)\n", __FUNCTION__, driverFaultReporter != nullptr);
    // Missing reporter is fatal later (PeerManager panics), so fail the start
    // here instead and let the boot run without Wi-Fi.
    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter;
}

bool AirportItlwm::start(IOService *provider)
{
#if __IO80211_TARGET >= __MAC_15_0
    return startNative(provider);
#else
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;
    
    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    setProperty("DriverKitDriver", kOSBooleanFalse);
#if __IO80211_TARGET >= __MAC_15_0
    // 24G830 caches getLogger() inside IO80211Controller::start().
    if (!initCCLogs()) {
        releaseCCLogs();
        return false;
    }
#endif
    if (!super::start(provider)) {
#if __IO80211_TARGET >= __MAC_15_0
        releaseCCLogs();
#endif
        return false;
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        super::stop(provider);
#if __IO80211_TARGET >= __MAC_15_0
        releaseCCLogs();
#endif
        return false;
    }
    if (initPCIPowerManagment(pciNub) == false) {
        super::stop(pciNub);
#if __IO80211_TARGET >= __MAC_15_0
        releaseCCLogs();
#endif
        return false;
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        releaseAll();
        return false;
    }
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    fHalService->get80211Controller()->ic_event_handler = eventHandler;
    
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;
    
    if (!fHalService->attach(pciNub)) {
        XYLog("attach fail\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
    _fWorkloop->addEventSource(scanSource);
    scanSource->enable();

    fNetIf = new AirportItlwmSkywalkInterface;
#if __IO80211_TARGET >= __MAC_15_0
    ether_addr selfAddress;
    IEEE80211_ADDR_COPY(selfAddress.octet, fHalService->get80211Controller()->ic_myaddr);
    if (!fNetIf->init(this, &selfAddress)) {
#else
    if (!fNetIf->init(this)) {
#endif
        XYLog("Skywalk interface init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);
    
#if __IO80211_TARGET < __MAC_15_0
    if (!initCCLogs()) {
        XYLog("CCLog init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#endif
    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface, true)) {
        XYLog("attach to IONetworkController interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    memset(&registInfo, 0, sizeof(registInfo));
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    fNetIf->start(this);
    
    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
    registerService();
    return true;
#endif
}

void AirportItlwm::stop(IOService *provider)
{
#if __IO80211_TARGET >= __MAC_15_0
    stopNative(provider);
#else
    XYLog("%s\n", __PRETTY_FUNCTION__);XYLog("%s\n", __PRETTY_FUNCTION__);
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    super::stop(provider);
    disableAdapter(bsdInterface);
    setLinkStatus(kIONetworkLinkValid);
    fHalService->detach(pciNub);
    ether_ifdetach(ifp);
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    releaseAll();
#endif
}

#if __IO80211_TARGET >= __MAC_15_0
#include "native_system_events_sequoia.inc"
#include "native_lifecycle_sequoia.inc"
#include "native_scan_sequoia.inc"
#include "native_join_sequoia.inc"
#include "native_packet_sequoia.inc"
#include "native_link_sequoia.inc"
#endif

#if __IO80211_TARGET >= __MAC_15_0
namespace {
struct NativeDebugRouteUpdate {
    uint32_t route = UINT32_MAX;
    uint32_t doneRoute = UINT32_MAX;
    bool routeSet = false;
    bool doneRouteSet = false;
};

static bool parseNativeDebugRoute(OSObject *value, bool doneRoute, uint32_t &out)
{
    if (auto *number = OSDynamicCast(OSNumber, value)) {
        const uint64_t candidate = number->unsigned64BitValue();
        const uint32_t limit = doneRoute ? 1u : 2u;
        if (candidate <= limit) {
            out = static_cast<uint32_t>(candidate);
            return true;
        }
        return false;
    }
    auto *string = OSDynamicCast(OSString, value);
    if (!string) return false;
    const char *text = string->getCStringNoCopy();
    if (!text) return false;
    if (!strcmp(text, "auto")) {
        out = UINT32_MAX;
        return true;
    }
    if (!doneRoute) {
        if (!strcmp(text, "cache")) { out = 0; return true; }
        if (!strcmp(text, "fsm")) { out = 1; return true; }
        if (!strcmp(text, "dual")) { out = 2; return true; }
    } else {
        if (!strcmp(text, "interface")) { out = 0; return true; }
        if (!strcmp(text, "controller")) { out = 1; return true; }
    }
    return false;
}
}

IOReturn AirportItlwm::setProperties(OSObject *properties)
{
    auto *dictionary = OSDynamicCast(OSDictionary, properties);
    if (!dictionary || !_fWorkloop) return kIOReturnBadArgument;

    NativeDebugRouteUpdate update;
    bool routeSeen = false;
    bool doneRouteSeen = false;
    OSCollectionIterator *iterator = OSCollectionIterator::withCollection(dictionary);
    if (!iterator) return kIOReturnNoMemory;
    OSObject *object = nullptr;
    IOReturn parseResult = kIOReturnSuccess;
    while ((object = iterator->getNextObject())) {
        auto *key = OSDynamicCast(OSString, object);
        if (!key) {
            parseResult = kIOReturnBadArgument;
            break;
        }
        const char *name = key->getCStringNoCopy();
        if (!name) {
            parseResult = kIOReturnBadArgument;
            break;
        }
#if SEQUOIA_DIAGNOSTICS
        if (!strcmp(name, "ItlwmNativeDebugScanRoute")) {
            if (routeSeen || !parseNativeDebugRoute(dictionary->getObject(key), false, update.route)) {
                parseResult = kIOReturnBadArgument;
                break;
            }
            routeSeen = true;
            update.routeSet = true;
        } else if (!strcmp(name, "ItlwmNativeDebugScanDoneRoute")) {
            if (doneRouteSeen || !parseNativeDebugRoute(dictionary->getObject(key), true, update.doneRoute)) {
                parseResult = kIOReturnBadArgument;
                break;
            }
            doneRouteSeen = true;
            update.doneRouteSet = true;
        } else
#endif
        {
            parseResult = kIOReturnUnsupported;
            break;
        }
    }
    iterator->release();
    if (parseResult != kIOReturnSuccess || (!routeSeen && !doneRouteSeen))
        return parseResult == kIOReturnSuccess ? kIOReturnBadArgument : parseResult;

    auto apply = [](OSObject *owner, void *arg0, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<AirportItlwm *>(owner);
        auto *values = static_cast<NativeDebugRouteUpdate *>(arg0);
        if (!self || !values || !self->nativeAvailable()) return kIOReturnNotReady;
        if (values->routeSet) {
            itlwmSetNativeScanRouteOverride(values->route);
            self->nativeScan.diagnostics.runtimeRoute = values->route;
        }
        if (values->doneRouteSet) {
            itlwmSetNativeScanDoneRouteOverride(values->doneRoute);
            self->nativeScan.diagnostics.runtimeDoneRoute = values->doneRoute;
        }
        self->publishNativeScanDiagnostics();
        return kIOReturnSuccess;
    };
    if (_fWorkloop->inGate()) return apply(this, &update, nullptr, nullptr, nullptr);
    return _fWorkloop->runAction(apply, this, &update, nullptr, nullptr, nullptr);
}
#endif

void AirportItlwm::free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
#if __IO80211_TARGET >= __MAC_15_0
    releaseCCLogs();
    if (nativePMLock) { IOLockFree(nativePMLock); nativePMLock = nullptr; }
#endif
    super::free();
}

bool AirportItlwm::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    return _fWorkloop != 0;
}

IO80211WorkQueue *AirportItlwm::getWorkQueue()
#if __IO80211_TARGET >= __MAC_15_0
const
#endif
{
    return _fWorkloop;
}

#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwm::prepareNativeBSDInterface(IO80211SkywalkInterface *candidate,
                                                   ifnet_t ifnet)
{
    if (!candidate || !ifnet) return kIOReturnBadArgument;
    if (!_fWorkloop) return kIOReturnNotReady;

    // A configd callback arrives outside the IO80211 gate.  runAction holds
    // that gate while invoking the same method, so the Apple WCL call below
    // observes inGate()==true.  If this callback ever arrives on the work
    // queue's own thread, do not synchronously re-enter Apple's WCL path:
    // sendIOUCToWcl explicitly rejects that context and would panic the host.
    if (!_fWorkloop->inGate()) {
        return _fWorkloop->runAction(
            [](OSObject *owner, void *interface, void *bsdIfnet, void *, void *) -> IOReturn {
                return static_cast<AirportItlwm *>(owner)->prepareNativeBSDInterface(
                    static_cast<IO80211SkywalkInterface *>(interface),
                    static_cast<ifnet_t>(bsdIfnet));
            }, this, candidate, ifnet);
    }
    if (_fWorkloop->onThread()) return kIOReturnCannotLock;

    // Recheck every borrowed object after entering the gate.  stopNative()
    // closes admission before draining this same gate, so a successful check
    // keeps the interface, registration and HAL alive for the Apple call.
    if (!nativeAvailable() || fNetIf != candidate || !bsdInterface ||
        !nativeInterfaceStarted || !nativeRegistration.prepared())
        return kIOReturnNotReady;
    auto *network = static_cast<IOSkywalkNetworkInterface *>(candidate);
    if (!network->mExpansionData || !network->mExpansionData->fRegistrationInfo)
        return kIOReturnNotReady;
    return candidate->prepareBSDInterface(ifnet, 0);
}
#endif

#if __IO80211_TARGET >= __MAC_15_0
void *AirportItlwm::getLogger() const
{
    return driverLogger;
}
#endif

void *AirportItlwm::getFaultReporterFromDriver()
{
#if __IO80211_TARGET >= __MAC_15_0
    return nativeFaultAdapter;
#else
    return driverFaultReporter;
#endif
}

IOReturn AirportItlwm::enable(IO80211SkywalkInterface *netif)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!nativeAvailable()) return kIOReturnNotReady;
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    const auto enabled = super::enable(netif);
    if (enabled != kIOReturnSuccess) return enabled;
#else
    super::enable(netif);
#endif
    _fCommandGate->enable();
    if (power_state)
        return enableAdapter(bsdInterface);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IO80211SkywalkInterface *netif)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!nativeAvailable()) return kIOReturnNotReady;
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    
    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
    
    return true;
}

IONetworkInterface *AirportItlwm::createInterface()
{
    AirportItlwmEthernetInterface *netif = new AirportItlwmEthernetInterface;
    if (!netif)
        return NULL;
    if (!netif->initWithSkywalkInterfaceAndProvider(this, fNetIf)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportItlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    if (!medium || !IONetworkMedium::addMedium(mediumDict, medium)) {
        OSSafeReleaseNULL(medium);
        mediumDict->release();
        return false;
    }
    medium->release();
    if (primary) {
        *primary = medium;
    }
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    if (!medium || !IONetworkMedium::addMedium(mediumDict, medium)) {
        OSSafeReleaseNULL(medium);
        mediumDict->release();
        if (primary) *primary = nullptr;
        return false;
    }
    medium->release();
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
#if __IO80211_TARGET >= __MAC_15_0
    // BSD observes link/key state here. Native WCL publication is owned by
    // publishNativeLink, never the legacy output thread or empty ioctl events.
    return super::setLinkStatus(status, activeMedium, speed, data);
#else
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            bsdInterface->startOutputThread();
#endif
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
//            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            bsdInterface->stopOutputThread();
            bsdInterface->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
        }
    }
    return ret;
#endif
}

IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    IOReturn ret = that->fNetIf->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    that->fNetIf->setRunningState((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, false);
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->fNetIf->reportLinkStatus(3, 0x80);
    } else {
        that->fNetIf->reportLinkStatus(1, 0);
    }
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    return ret;
}

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
#if __IO80211_TARGET >= __MAC_15_0
    ++nativeTxOutputCalls;
#endif
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN ||
        !ifp->if_snd.queue || !ifp->if_start) {
#if __IO80211_TARGET >= __MAC_15_0
        ++nativeTxOutputGated;
#endif
        if (m && mbuf_type(m) != MBUF_TYPE_FREE) freePacket(m);
        return kIOReturnOutputDropped;
    }
    // Reject before accessing flags or payload. The old path fell through
    // after null input or after freeing an mbuf without a packet header.
    if (!m || mbuf_type(m) == MBUF_TYPE_FREE) {
        if (ifp->netStat) ++ifp->netStat->outputErrors;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR)) {
        if (ifp->netStat) ++ifp->netStat->outputErrors;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return kIOReturnOutputSuccess;
}

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

#if __IO80211_TARGET >= __MAC_15_0
void AirportItlwm::drainNativeRadioTransition()
{
    if (!_fWorkloop || !_fCommandGate) return;
    if (!_fWorkloop->inGate()) {
        _fWorkloop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<AirportItlwm *>(owner)->drainNativeRadioTransition();
            return kIOReturnSuccess;
        }, this);
        return;
    }
    // Stop has already closed admission. A sleeping firmware operation still
    // owns the HAL and command gate even though nativeAvailable() is false.
    while (nativeRadioTransition)
        _fCommandGate->commandSleep(&nativeRadioTransition, THREAD_UNINT);
}
#endif

IOReturn AirportItlwm::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!addrP || addrBytes != sizeof(IOEthernetAddress)) return kIOReturnBadArgument;
    IOEthernetAddress address;
    memcpy(&address, addrP, sizeof(address));
    const uint8_t zero[6] = {};
    if ((address.bytes[0] & 1) || !memcmp(address.bytes, zero, sizeof(zero)))
        return kIOReturnBadArgument;
    if (!_fWorkloop) return kIOReturnNotReady;
    if (!_fWorkloop->inGate())
        return _fWorkloop->runAction([](OSObject *owner, void *value, void *, void *, void *) -> IOReturn {
            return static_cast<AirportItlwm *>(owner)->setHardwareAddress(value, sizeof(IOEthernetAddress));
        }, this, &address);
    if (nativeMacRequests != UINT32_MAX) ++nativeMacRequests;
    auto finish = [this](IOReturn result) {
        if (auto *provider = getProvider()) {
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeMacRequests", nativeMacRequests, 32);
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeMacChanges", nativeMacChanges, 32);
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeMacLastResult", static_cast<uint32_t>(result), 32);
        }
        return result;
    };
    if (!nativeRadioAvailable() || !power_state ||
        (nativeSkywalkBSDOwner ? (!fNetIf || !fNetIf->getBSDInterface()) : !bsdInterface) || !_fCommandGate ||
        !fHalService || !fHalService->getNativeHal()) return finish(kIOReturnNotReady);
    auto *hal = fHalService->getNativeHal();
    auto *ic = hal->nativeController();
    if (!ic) return finish(kIOReturnNotReady);
    // A repeat of the active identity is harmless, including during a join.
    if (!memcmp(ic->ic_myaddr, address.bytes, 6) &&
        !memcmp(ic->ic_ac.ac_enaddr, address.bytes, 6)) return finish(kIOReturnSuccess);
    if (nativeJoin.busy || nativeScan.workerActive() || nativeScan.delivery.busy())
        return finish(kIOReturnBusy);
    if (_fWorkloop->onThread()) return finish(kIOReturnCannotLock);
    // Stop the old firmware context before changing its source address. The
    // transition blocks scan/join/PM admission across HAL commandSleep calls;
    // stop waits for it before releasing the HAL. Failed stop keeps both
    // software addresses intact. The next nativeEnable programs the new MAC.
    nativeRadioTransition = true;
    const auto stopped = hal->nativeDisable(bsdInterface);
    nativeRadioTransition = false;
    _fCommandGate->commandWakeup(&nativeRadioTransition, false);
    if (stopped != kIOReturnSuccess) return finish(stopped);
    if (!nativeRadioAvailable()) return finish(kIOReturnNotReady);
    memcpy(ic->ic_ac.ac_enaddr, address.bytes, 6);
    memcpy(ic->ic_myaddr, address.bytes, 6);
    if (nativeMacChanges != UINT32_MAX) ++nativeMacChanges;
    // IO80211MacAddressAgent owns BSD/property notification after this callback
    // succeeds. Never acknowledge a private address while using the old one.
    return finish(kIOReturnSuccess);
#else
    if (!fNetIf || !addrP)
        return kIOReturnError;
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, (const UInt8 *)addrP);
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
        fHalService->disable(bsdInterface);
        fHalService->enable(bsdInterface);
    }
    return kIOReturnSuccess;
#endif
}

UInt32 AirportItlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportItlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

bool AirportItlwm::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportItlwm::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!cd) return kIOReturnBadArgument;
    if (!_fWorkloop) return kIOReturnNotReady;
    return _fWorkloop->runAction([](OSObject *owner, void *data, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<AirportItlwm *>(owner);
        if (!self->nativeHardwareQueryAvailable()) return kIOReturnNotReady;
        const auto *ic = self->fHalService->get80211Controller();
        if (!ic) return kIOReturnNotReady;
        apple80211_capability_data result = {};
        result.version = APPLE80211_VERSION;
        if (ic->ic_caps & IEEE80211_C_RSN) {
            result.capabilities[0] |= 1u << APPLE80211_CAP_AES_CCM;
            result.capabilities[1] |= 1u << (APPLE80211_CAP_WPA2 - 8);
        }
        if (ic->ic_caps & IEEE80211_C_SHSLOT)
            result.capabilities[1] |= 1u << (APPLE80211_CAP_SHSLOT - 8);
        if (ic->ic_caps & IEEE80211_C_SHPREAMBLE)
            result.capabilities[1] |= 1u << (APPLE80211_CAP_SHPREAMBLE - 8);
        if (ic->ic_caps & IEEE80211_C_QOS)
            result.capabilities[2] |= 1u << (APPLE80211_CAP_WME - 16);
        if (ic->ic_htcaps & IEEE80211_HTCAP_SGI20)
            result.capabilities[2] |= 1u << (APPLE80211_CAP_SHORT_GI_20MHZ - 16);
        // 24G830 Apple80211Scan +0x254 rejects a non-cache multi-SSID
        // request with -3903 unless this bit is set. ScanPlan already owns,
        // validates and submits the bounded directed-SSID probe list.
        result.capabilities[2] |= 0x40;
        bool band2 = false, band5 = false;
        for (unsigned i = 1; i <= IEEE80211_CHAN_MAX; ++i) {
            if (!isset(ic->ic_chan_avail, i) || !ic->ic_channels[i].ic_freq) continue;
            band2 |= IEEE80211_IS_CHAN_2GHZ(&ic->ic_channels[i]);
            band5 |= IEEE80211_IS_CHAN_5GHZ(&ic->ic_channels[i]);
        }
        // 24G830 WCLDeviceConfiguration::isDualBand tests capability bit29.
        if (band2 && band5) result.capabilities[3] |= 0x20;
        // 24G830 Apple80211Scan tests cached capabilities[5] & 0x40
        // before creating/waiting on its scan-done semaphore. Without it,
        // airportd fetches results immediately while our async scan is busy.
        // nativeScanPostGated already emits APPLE80211_M_SCAN_DONE.
        // See SEQUOIA_APPLE80211_DISCOVERY.md for the audited userspace gate.
        result.capabilities[5] |= 0x40;
        // 24H23 Apple80211Scan +0x162 selects the 0x1598-byte SCAN_REQ
        // layout for SSID/BSSID-list requests using capability byte 9 bit 1.
        // Without it userspace sends obsolete ioctl 86, absent from both
        // stock external handler tables, and never reaches our WCL scanner.
        // ioctl 10 lets Apple translate the list into our existing bounded
        // WCL ScanRequest; this does not enable a new Intel firmware mode.
        // See reverse_apple80211_scan_routing.py for the executed branch audit.
        result.capabilities[9] |= 0x02;
        // Sequoia's WCL cache manager is gated by capability byte 10 bit 4
        // (WCLDeviceConfiguration::isBGScanCacheEnabled reads ivars+0xcaa).
        // Without this bit airportd never treats the WCL scan cache as a
        // source of CoreWLAN networks, even when native scan results and the
        // 0xce cache-update events have been delivered successfully.
        // The native path implements the fixed-size cache query below, so this
        // is an advertised capability rather than a speculative feature bit.
        if (!self->nativeWclCompatR162)
            result.capabilities[10] |= 0x10;
        // No WEP/TKIP/WPA1, WOW, MFP, roaming, offload or unaudited bits.
        memcpy(data, &result, sizeof(result));
        return kIOReturnSuccess;
    }, this, cd);
#else
    uint32_t caps = fHalService->get80211Controller()->ic_caps;
    memset(cd, 0, sizeof(struct apple80211_capability_data));
    
    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards
    
    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

    cd->version = APPLE80211_VERSION;
    cd->capabilities[2] = 0xFF; // BURST, WME, SHORT_GI_40MHZ, SHORT_GI_20MHZ, WOW, TSN, ?, ?
    cd->capabilities[3] = 0x2B;
    cd->capabilities[5] = 0x40;
    cd->capabilities[6] = (
//                           1 |    //MFP capable
                           0x8 |
                           0x4 |
                           0x80
                           );
    *(uint16_t *)&cd->capabilities[8] = 0x201;
//
//    cd->capabilities[2] |= 0x10;
//    cd->capabilities[5] |= 0x1;
//
//    cd->capabilities[2] |= 0x2;
//
//    cd->capabilities[3] |= 0x20;
//
//    cd->capabilities[0] |= 0x80;
//
//    cd->capabilities[3] |= 0x80;
//    cd->capabilities[4] |= 0x4;
//
//    cd->capabilities[4] |= 0x1;
//    cd->capabilities[3] |= 0x1;
//    cd->capabilities[6] |= 0x8;
//
//    cd->capabilities[3] |= 3;
//    cd->capabilities[4] |= 2;
//    cd->capabilities[6] |= 0x10;
//    cd->capabilities[5] |= 0x20;
//    cd->capabilities[5] |= 0x80;
//
//    if (cd->capabilities[6] & 0x20) {
//        cd->capabilities[2] |= 8;
//    }
//    cd->capabilities[5] |= 8;
//    cd->capabilities[8] |= 2;
//
//    cd->capabilities[11] |= (2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x80);
    
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
#if __IO80211_TARGET >= __MAC_15_0
    return getNativeVersion(hv, true);
#else
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, fHalService->getDriverInfo()->getFirmwareVersion());
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
#if __IO80211_TARGET >= __MAC_15_0
    return getNativeVersion(hv, false);
#else
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fHalService->getDriverInfo()->getFirmwareVersion(), sizeof(hv->string));
    hv->string_len = strlen(fHalService->getDriverInfo()->getFirmwareVersion());
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
#if __IO80211_TARGET >= __MAC_15_0
    return getNativeCountry(cd);
#else
    char user_override_cc[3];
    const char *cc_fw = fHalService->getDriverInfo()->getFirmwareCountryCode();
    
    if (!cd)
        return kIOReturnError;
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("itlwm_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
#if __IO80211_TARGET >= __MAC_15_0
    // Reporting a new country without applying firmware/NVM restrictions
    // would be misleading. Native regulatory reconfiguration is not implemented.
    return data ? kIOReturnUnsupported : kIOReturnBadArgument;
#else
    XYLog("%s cc=%s\n", __FUNCTION__, data->cc);
    if (data && data->cc[0] != 120 && data->cc[0] != 88) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        fNetIf->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
    }
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!_fWorkloop) return kIOReturnNotReady;
    if (!_fWorkloop->inGate())
        return _fWorkloop->runAction([](OSObject *owner, void *object, void *data, void *, void *) -> IOReturn {
            return static_cast<AirportItlwm *>(owner)->getPOWER(static_cast<OSObject *>(object),
                static_cast<apple80211_power_data *>(data));
        }, this, object, pd);
    if (!nativeAvailable()) return kIOReturnNotReady;
#endif
    if (!pd)
        return kIOReturnError;
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
#if __IO80211_TARGET >= __MAC_15_0
    if (auto *provider = getProvider())
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePowerReadState", power_state, 32);
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!_fWorkloop) return kIOReturnNotReady;
    if (!_fWorkloop->inGate())
        return _fWorkloop->runAction([](OSObject *owner, void *object, void *data, void *, void *) -> IOReturn {
            return static_cast<AirportItlwm *>(owner)->setPOWER(static_cast<OSObject *>(object),
                static_cast<apple80211_power_data *>(data));
        }, this, object, pd);
    if (!nativeAvailable()) return kIOReturnNotReady;
    if (!pd || !pd->num_radios || pd->num_radios > 4 || pd->power_state[0] > 1)
        return kIOReturnBadArgument;
    for (unsigned i = 1; i < pd->num_radios; ++i)
        if (pd->power_state[i] != pd->power_state[0]) return kIOReturnBadArgument;
    // Radio state is not a third IOPM ordinal; the policy table has only 0/1.
    const auto requestedPower = pd->power_state[0];
    const auto result = requestedPower ? enableAdapter(bsdInterface) : disableAdapter(bsdInterface);
    if (auto *provider = getProvider()) {
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePowerRequested", requestedPower, 32);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePowerResult", static_cast<uint32_t>(result), 32);
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePowerPMState", pmPowerState, 32);
        if (fHalService && fHalService->getNativeHal())
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePowerHalError",
                static_cast<uint32_t>(fHalService->getNativeHal()->nativeStateErrorCode()), 32);
    }
    if (result != kIOReturnSuccess) return result;
    power_state = requestedPower;
    if (requestedPower && !nativeFirmwareAnnounced && fNetIf) {
        // 24G830 BCM resetInterfacesComplete emits message 49 with uint32=1.
        // SSM translates this into FW_LOADED, which loads the permanent MAC,
        // capabilities and channels. Driver-available alone does not do this.
        // Send only after real firmware enable succeeds. Queue a copied event;
        // never synchronously enter WCL while holding the HAL command gate.
        uint32_t complete = 1;
        const auto posted = postMessage(fNetIf, APPLE80211_M_RESET_INTERFACE,
                                        &complete, sizeof(complete), true);
        nativeFirmwareAnnounced = posted == kIOReturnSuccess;
        ITLWM_PUBLISH_DIAGNOSTIC(this, "ItlwmNativeFirmwareReadyPosted", nativeFirmwareAnnounced);
        ITLWM_PUBLISH_DIAGNOSTIC(this, "ItlwmNativeFirmwareReadyPostResult", static_cast<UInt64>(posted), 32);
    }
    return kIOReturnSuccess;
#else
    if (!pd)
        return kIOReturnError;
    ITLWM_LOG("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
        if (pd->power_state[0] == 0) {
            changePowerStateToPriv(1);
            if (isRunning) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(2);
            if (!isRunning) {
                enableAdapter(bsdInterface);
            }
        }
        power_state = (pd->power_state[0]);
    }
    
    return kIOReturnSuccess;
#endif
}

#if __IO80211_TARGET < __MAC_15_0
SInt32 AirportItlwm::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s b1: %d b2: %d\n", __FUNCTION__, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2);
    return super::apple80211_ioctl(interface, cmd, data, b1, b2);
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 1 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 2 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}
#endif

IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!_fWorkloop) return kIOReturnNotReady;
    if (!_fWorkloop->inGate())
        return _fWorkloop->runAction([](OSObject *owner, void *interface, void *, void *, void *) -> IOReturn {
            return static_cast<AirportItlwm *>(owner)->enableAdapter(static_cast<IONetworkInterface *>(interface));
        }, this, netif);
    // Each guard gets its own number. A wake that cannot bring the radio back
    // leaves the reason in the registry, so one sleep cycle is enough to tell
    // which condition refused the request.
    const auto refuse = [this](uint64_t reason) -> IOReturn {
        if (auto *provider = getProvider())
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeEnableRefused", reason, 32);
        return kIOReturnNotReady;
    };
    // Skywalk publishes the BSD ifnet through Apple's client, without a custom
    // IONetworkInterface child. Require its real ifnet before enabling radio.
    if (!nativeRadioAvailable()) return refuse(1);
    if (!watchdogTimer || !_fCommandGate) return refuse(2);
    if (nativeSkywalkBSDOwner ? (!fNetIf || !fNetIf->getBSDInterface()) : !netif)
        return refuse(3);
    if (!fHalService || !fHalService->getNativeHal()) return refuse(4);
    // A duplicate request does not start a radio transition. Connected joins
    // keep busy set for their entire lifetime; they must not make an already-on
    // radio report Busy to Apple's power clients.
    auto *hal = fHalService->getNativeHal();
    auto *ic = fHalService->get80211Controller();
    const bool alreadyOn = power_state && ic && (ic->ic_if.if_flags & IFF_RUNNING) &&
        !hal->nativeDetachedState() && !hal->nativeStateErrorCode() &&
        hal->nativeCommandTransportReady();
    if (auto *provider = getProvider())
        ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativePowerAlreadyOn", alreadyOn, 32);
    if (alreadyOn) return kIOReturnSuccess;
    if (nativeJoin.busy || nativeScan.workerActive()) return refuse(5);
    if (_fWorkloop->onThread()) return refuse(6);
    nativeRadioTransition = true;
    const auto result = fHalService->getNativeHal()->nativeEnable(netif);
    nativeRadioTransition = false;
    _fCommandGate->commandWakeup(&nativeRadioTransition, false);
    if (result != kIOReturnSuccess) {
        if (auto *provider = getProvider())
            ITLWM_PUBLISH_DIAGNOSTIC(provider, "ItlwmNativeEnableRefused", 7, 32);
        return result;
    }
    if (!nativeRadioAvailable()) return refuse(8);
    return kIOReturnSuccess; // Native scan workers do not use net80211's watchdog.
#else
    fHalService->enable(netif);
#endif
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!_fWorkloop) return kIOReturnNotReady;
    if (!_fWorkloop->inGate())
        return _fWorkloop->runAction([](OSObject *owner, void *interface, void *, void *, void *) -> IOReturn {
            return static_cast<AirportItlwm *>(owner)->disableAdapter(static_cast<IONetworkInterface *>(interface));
        }, this, netif);
    // Power-off must still drain firmware if the Skywalk BSD client went away.
    if (!nativeAvailable() || !watchdogTimer || !_fCommandGate ||
        (!nativeSkywalkBSDOwner && !netif) ||
        !fHalService || !fHalService->getNativeHal()) return kIOReturnNotReady;
    if (_fWorkloop->onThread()) return kIOReturnCannotLock;
    if (nativeRadioTransition) return kIOReturnBusy;
    nativeRadioTransition = true;
    const auto result = [&]() -> IOReturn {
        if (nativeJoin.busy) {
            cancelNativeJoin(false);
            while (nativeJoin.worker)
                _fCommandGate->commandSleep(&nativeJoin, THREAD_UNINT);
            if (!nativeAvailable()) return kIOReturnNotReady;
            if (!nativeJoin.drained) return kIOReturnError;
        }
        if (nativeScan.workerActive()) {
            cancelNativeScan();
            while (nativeScan.workerActive())
                _fCommandGate->commandSleep(&nativeScan, THREAD_UNINT);
            if (!nativeAvailable()) return kIOReturnNotReady;
        }
        return fHalService->getNativeHal()->nativeDisable(netif);
    }();
    nativeRadioTransition = false;
    _fCommandGate->commandWakeup(&nativeRadioTransition, false);
    if (result != kIOReturnSuccess) return result;
    if (!nativeAvailable()) return kIOReturnNotReady;
#endif
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
#if __IO80211_TARGET < __MAC_15_0
    fHalService->disable(netif);
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
    if (dev == 0)
        return kIOReturnError;
    
    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

bool AirportItlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
#if __IO80211_TARGET >= __MAC_15_0
    // The wrapper owns PCI configuration. HAL attach enables DMA only after
    // that exclusive open; legacy PME/status writes are not part of this port.
    return provider && provider == pciNub;
#else
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
#endif
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

void AirportItlwm::unregistPM()
{
#if __IO80211_TARGET >= __MAC_15_0
    SequoiaThreadCall::destroy(powerOffThreadCall);
    SequoiaThreadCall::destroy(powerOnThreadCall);
#else
    if (powerOffThreadCall) {
        thread_call_free(powerOffThreadCall);
        powerOffThreadCall = NULL;
    }
    if (powerOnThreadCall) {
        thread_call_free(powerOnThreadCall);
        powerOnThreadCall = NULL;
    }
#endif
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;
#if __IO80211_TARGET >= __MAC_15_0
    if (!nativePMLock) return result;
    IOLockLock(nativePMLock);
    if (!nativeReady || nativeStopping || pmPowerState == powerStateOrdinal) {
        IOLockUnlock(nativePMLock);
        return result;
    }
#else
    
    if (pmPowerState == powerStateOrdinal)
        return result;
#endif
    switch (powerStateOrdinal) {
        case kPowerStateOff:
            if (powerOffThreadCall) {
#if __IO80211_TARGET >= __MAC_15_0
                if (powerOffThreadCall->enter()) result = 5000000;
#else
                retain();
                if (thread_call_enter(powerOffThreadCall))
                    release();
                result = 5000000;
#endif
            }
            break;
        case kPowerStateOn:
            if (powerOnThreadCall) {
#if __IO80211_TARGET >= __MAC_15_0
                if (powerOnThreadCall->enter()) result = 5000000;
#else
                retain();
                if (thread_call_enter(powerOnThreadCall))
                    release();
                result = 5000000;
#endif
            }
            break;
            
        default:
            break;
    }
#if __IO80211_TARGET >= __MAC_15_0
    IOLockUnlock(nativePMLock);
#endif
    return result;
}

IOReturn AirportItlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0,
                             thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *)param0;

#if __IO80211_TARGET >= __MAC_15_0
    self->getWorkQueue()->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        static_cast<AirportItlwm *>(owner)->setPowerStateOff();
        return kIOReturnSuccess;
    }, self);
#else
    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOff,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOff();
        self->release();
    }
#endif
}

static void handleSetPowerStateOn(thread_call_param_t param0,
                            thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *) param0;

#if __IO80211_TARGET >= __MAC_15_0
    self->getWorkQueue()->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        static_cast<AirportItlwm *>(owner)->setPowerStateOn();
        return kIOReturnSuccess;
    }, self);
#else
    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOn,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOn();
        self->release();
    }
#endif
}

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn ret;
    
    pmPowerState = kPowerStateOn;
    pmPolicyMaker = policyMaker;
    
#if __IO80211_TARGET >= __MAC_15_0
    powerOffThreadCall = SequoiaThreadCall::allocate(handleSetPowerStateOff, this);
    powerOnThreadCall = SequoiaThreadCall::allocate(handleSetPowerStateOn, this);
#else
    powerOffThreadCall = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOff,
                                            (thread_call_param_t)this);
    powerOnThreadCall  = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOn,
                                              (thread_call_param_t)this);
#endif
#if __IO80211_TARGET >= __MAC_15_0
    if (!powerOnThreadCall || !powerOffThreadCall) {
        unregistPM();
        return kIOReturnNoMemory;
    }
#endif
    ret = pmPolicyMaker->registerPowerDriver(this,
                                             powerStateArray,
                                             kPowerStateCount);
    return ret;
}

void AirportItlwm::setPowerStateOff()
{
    XYLog("%s\n", __FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    IOLockLock(nativePMLock);
#endif
    pmPowerState = kPowerStateOff;
#if __IO80211_TARGET >= __MAC_15_0
    IOLockUnlock(nativePMLock);
#endif
#if __IO80211_TARGET >= __MAC_15_0
    // Another radio operation can be asleep inside firmware initialization.
    // PM has already closed radio admission; drain that operation before ACK.
    while (nativeRadioTransition && nativeAvailable())
        _fCommandGate->commandSleep(&nativeRadioTransition, THREAD_UNINT);
    const auto result = disableAdapter(bsdInterface);
    ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOffError", static_cast<uint64_t>(result), 32);
#else
    disableAdapter(bsdInterface);
#endif
    pmPolicyMaker->acknowledgeSetPowerState();
}

void AirportItlwm::setPowerStateOn()
{
    XYLog("%s\n", __FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    IOLockLock(nativePMLock);
#endif
    pmPowerState = kPowerStateOn;
#if __IO80211_TARGET >= __MAC_15_0
    IOLockUnlock(nativePMLock);
#endif
#if __IO80211_TARGET >= __MAC_15_0
    // PM-off shuts down firmware but intentionally retains the user's radio
    // preference. Restore that radio before acknowledging wake; never invent
    // a join or retain credentials here. WCL continues to own reconnection.
    while (nativeRadioTransition && nativeAvailable())
        _fCommandGate->commandSleep(&nativeRadioTransition, THREAD_UNINT);
    IOReturn result = kIOReturnSuccess;
    ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOnCleanupRetries", static_cast<uint64_t>(0), 32);
    // A wake that finds the radio off has to bring it back. Record the two
    // inputs to that decision, so a refusal can be told apart from a wake that
    // never tried: the user's radio preference and the native interface state.
    ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOnUserRadio", power_state ? 1 : 0, 32);
    ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOnNativeAvailable", nativeAvailable() ? 1 : 0, 32);
    if (power_state && nativeAvailable()) {
        // A drained pre-sleep join can still await its final WCL notification.
        // The join timer retries that notification, but cannot initialize the
        // radio. Keep this PM worker alive for bounded queue backpressure so
        // a transient failed post does not leave wake permanently unpowered.
        serviceNativeJoin();
        unsigned retries = 0;
        while (nativeRadioAvailable() && power_state && nativeJoin.busy &&
               nativeJoin.drained && !nativeJoin.worker &&
               (nativeJoin.cancel || nativeJoin.error) && retries < 100) {
            AbsoluteTime deadline;
            clock_interval_to_deadline(10, kMillisecondScale, &deadline);
            _fCommandGate->commandSleep(&nativeJoin, deadline, THREAD_UNINT);
            ++retries;
            if (!nativeRadioAvailable() || !power_state) break;
            serviceNativeJoin();
        }
        ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOnCleanupRetries", retries, 32);
        // Sleeping releases the gate: stop, another PM-off or a user radio-off
        // can supersede wake. Never initialize hardware for a stale request.
        result = nativeRadioAvailable() && power_state ?
            enableAdapter(bsdInterface) : kIOReturnNotReady;
        if (result == kIOReturnSuccess) {
            // The radio is back, but WCL may still believe the link from
            // before the sleep is up. Tell it that the link is gone, or it
            // keeps the association it thinks it has and asks for no new one.
            const auto balanced = balanceNativeLinkDown();
            ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOnLinkBalance",
                balanced == kIOReturnSuccess ? 1 : 0, 32);
        }
    }
    ITLWM_PUBLISH_DIAGNOSTIC(this, "SequoiaNativePowerOnError", static_cast<uint64_t>(result), 32);
#endif
    pmPolicyMaker->acknowledgeSetPowerState();
}
