//
//  IOPCIEDeviceWrapper.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "IOPCIEDeviceWrapper.hpp"
#include "Apple80211.h"

#include "ItlIwm.hpp"
#include "ItlIwx.hpp"
#include "ItlIwn.hpp"
#if __IO80211_TARGET >= __MAC_15_0
#include <HAL/SequoiaTrialPolicy.h>
#include <libkern/version.h>
#endif
// The peer manager is built from the native lifecycle instead: this translation
// unit already carries the port's own copies of the 80211 declarations, and the
// two header families redefine each other's typedefs, so the call lives where the
// SDK's declarations are the ones in scope.

#define super IOService
OSDefineMetaClassAndStructors(IOPCIEDeviceWrapper, IOService);

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static IOPMPowerState powerStateArray[2] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    UInt16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    UInt16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

extern IOWorkLoop *_fWorkloop;

IOWorkLoop *IOPCIEDeviceWrapper::getWorkLoop() const
{
    return _fWorkloop;
}

IOService* IOPCIEDeviceWrapper::
probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool isMatch = false;
    super::probe(provider, score);
    UInt8 msiCap;
    UInt8 msixCap;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device)
        return NULL;
#if __IO80211_TARGET >= __MAC_15_0
    int nativeOptIn = 0;
    int diagnostic = 0;
    const char *diagnosticArguments[] = {
        "itlwm-sequoia-probe", "itlwm-sequoia-nvm", "itlwm-sequoia-firmware"};
    for (const char *argument : diagnosticArguments)
        if (PE_parse_boot_argn(argument, &diagnostic, sizeof(diagnostic))) return nullptr;
    if (!PE_parse_boot_argn("itlwm_native", &nativeOptIn, sizeof(nativeOptIn)) ||
        !SequoiaTrial::allowed(nativeOptIn, version_major, version_minor, version,
            device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID)))
        return nullptr;
#endif
    if (ItlIwx::iwx_match(device)) {
        isMatch = true;
        fHalService = new ItlIwx;
    }
    if (!isMatch && ItlIwm::iwm_match(device)) {
        isMatch = true;
        fHalService = new ItlIwm;
    }
    if (!isMatch && ItlIwn::iwn_match(device)) {
        isMatch = true;
        fHalService = new ItlIwn;
    }
    if (isMatch) {
        if (!fHalService) return nullptr;
        msiCap = msixCap = 0;
        XYLog("%s Found\n", __FUNCTION__);
        device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap);
#if __IO80211_TARGET < __MAC_15_0
        if (msixCap)
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
#endif
        device->findPCICapability(PCI_CAP_ID_MSI, &msiCap);
#if __IO80211_TARGET < __MAC_15_0
        if (msiCap)
            pciMsiSetEnable(device, msiCap, 1);
        if (!msiCap && !msixCap) {
#else
        if (!msiCap) {
#endif
            XYLog("%s No MSI cap\n", __FUNCTION__);
            fHalService->release();
            fHalService = NULL;
            return NULL;
        }
        this->pciNub = device;
        return this;
    }
    return NULL;
}

bool IOPCIEDeviceWrapper::
start(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    // BSD timeout/mbuf compatibility still has one global owner.
    if (_fWorkloop) return false;
    if (provider != pciNub || !nativePCI.acquire(pciNub, this)) return false;
#endif
    _fWorkloop = IO80211WorkQueue::workQueue();
    if (!_fWorkloop) {
#if __IO80211_TARGET >= __MAC_15_0
        nativePCI.release();
#endif
        return false;
    }
#if __IO80211_TARGET >= __MAC_15_0
    ownsNativeWorkLoop = true;
#endif
    if (!super::start(provider)) {
#if __IO80211_TARGET >= __MAC_15_0
        OSSafeReleaseNULL(_fWorkloop);
        ownsNativeWorkLoop = false;
        nativePCI.release();
#endif
        return false;
    }
    IOLog("%s::super start succeed\n", getName());
    UInt8 builtIn = 0;
    setProperty("built-in", &builtIn, sizeof(builtIn));
    PMinit();
    registerPowerDriver(this, powerStateArray, 2);
    provider->joinPMtree(this);
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (!startDeferredPublish()) {
        // Losing the controller to a diagnostic would be worse than the early
        // start this defers, so publish exactly as before when the deferral
        // cannot be armed.
        IOLog("%s::publishing immediately (deferred publish unavailable)\n", getName());
        registerService();
    }
#else
    registerService();
#endif
    return true;
}

void IOPCIEDeviceWrapper::
stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    // Close the deferred publish before anything else: it holds this object and
    // may otherwise publish a node that is already going away.
    stopDeferredPublish();
#endif
    PMstop();
    super::stop(provider);
#if __IO80211_TARGET >= __MAC_15_0
    nativePCI.release();
#endif
}

IOReturn IOPCIEDeviceWrapper::
setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}

#if __IO80211_TARGET >= __MAC_15_0
void IOPCIEDeviceWrapper::free()
{
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    // stop() already closed this; repeat only to be certain no callback can
    // still be holding the object once it is released.
    stopDeferredPublish();
#endif
    OSSafeReleaseNULL(fHalService);
    nativePCI.release();
    // Child stop releases its retained reference before provider teardown.
    if (ownsNativeWorkLoop) OSSafeReleaseNULL(_fWorkloop);
    super::free();
}
#endif

#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
namespace {

const unsigned kPublishPollMillis = 500;
const unsigned kPublishFallbackPolls = 360; // 180 s of polling.
// The desktop window is the proven-safe start point: the root volume appears
// during the cryptex/launchd phase, and a controller started inside that phase
// wedges the machine. 90 s covers the desktop on this hardware; an explicit
// itlwm_publish_delay_ms still wins.
const unsigned kPublishSettleMillis = 90000;

// itlwm_publish_delay_ms=<n> publishes after n milliseconds.
bool publishDelayMillis(unsigned &delayMs)
{
    int value = 0;
    if (!PE_parse_boot_argn("itlwm_publish_delay_ms", &value, sizeof(value)) || value < 0)
        return false;
    delayMs = static_cast<unsigned>(value);
    return true;
}

// The root volume is the first signal that the boot has moved past the storage
// bring-up this publish is trying to avoid overlapping. waitForMatchingService
// does not consume the dictionary (waitForService does), takes nanoseconds, and
// returns a reference the caller releases.
bool rootVolumeIsUp()
{
    OSDictionary *match = IOService::serviceMatching("AppleAPFSVolume");
    if (!match) return false;
    IOService *volume = IOService::waitForMatchingService(match,
        static_cast<uint64_t>(kPublishPollMillis) * 1000000ULL);
    match->release();
    if (!volume) return false;
    volume->release();
    return true;
}

} // namespace

void IOPCIEDeviceWrapper::publishAction(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<IOPCIEDeviceWrapper *>(param0);
    unsigned delayMs = 0;
    const bool haveDelay = publishDelayMillis(delayMs);
    const char *reason = "fallback";
    // An explicit delay outranks the fallback cap: the cap exists so the node is
    // never lost when waiting for the root volume, but a control run that asks
    // for ten minutes must really get ten minutes, not sixty seconds.
    const unsigned polls = haveDelay
        ? (delayMs + kPublishPollMillis - 1) / kPublishPollMillis
        : kPublishFallbackPolls;
    for (unsigned poll = 0; poll < polls; ++poll) {
        // stop()/free() set this and then wait for the callback to return, so
        // the poll interval bounds how long that wait can take.
        if (__atomic_load_n(&self->publishCancelled, __ATOMIC_RELAXED)) return;
        const unsigned elapsed = (poll + 1) * kPublishPollMillis;
        if (haveDelay) {
            if (elapsed >= delayMs) { reason = "delay"; break; }
        } else if (elapsed >= kPublishSettleMillis && rootVolumeIsUp()) {
            // The root volume alone is NOT enough: it appears while the boot is
            // still in the cryptex/launchd phase, and starting the controller
            // inside that phase wedges the machine (r95/r96/r97/r99). r98
            // reached the desktop with the controller held back and only failed
            // later, in PeerManager, so the desktop window is the proven-safe
            // point; 90 s covers it on this machine.
            reason = "root-volume-and-settle";
            break;
        }
        IOSleep(kPublishPollMillis);
    }
    if (__atomic_load_n(&self->publishCancelled, __ATOMIC_RELAXED)) return;
    IOLog("%s::publishing after %s wait (%u ms); the controller starts only now\n",
          self->getName(), reason, static_cast<unsigned>(kPublishFallbackPolls) * kPublishPollMillis);
    self->registerService();
}

bool IOPCIEDeviceWrapper::startDeferredPublish()
{
    if (publishCall) return true;
    int immediate = 0;
    if (PE_parse_boot_argn("itlwm_publish_now", &immediate, sizeof(immediate))) {
        IOLog("%s::publishing now (itlwm_publish_now)\n", getName());
        return false;
    }
    publishCall = SequoiaThreadCall::allocate(publishAction, this);
    if (!publishCall) return false;
    if (!publishCall->enter()) {
        SequoiaThreadCall::destroy(publishCall);
        return false;
    }
    unsigned delayMs = 0;
    if (publishDelayMillis(delayMs))
        IOLog("%s::deferring publish by %u ms\n", getName(), delayMs);
    else
        IOLog("%s::deferring publish until the boot settles (root volume + 90 s)\n", getName());
    return true;
}

void IOPCIEDeviceWrapper::stopDeferredPublish()
{
    __atomic_store_n(&publishCancelled, true, __ATOMIC_RELAXED);
    SequoiaThreadCall::destroy(publishCall);
}
#endif
